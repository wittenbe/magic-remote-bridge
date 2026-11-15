#include "pc.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "class/hid/hid.h"
#include "settings.h"
#include "usb.h"

static const char *TAG = "pc";

// FD report (19 bytes): [0] ?, [1] seq, [2:4] status, [4:10] gyro x/y/z, [10:16] accel x/y/z (int16 BE),
// [16:18] button (u16 BE), [18] wheel (int8). Gyro and accel share one frame (checked against each other),
// gyro ≈16.9 LSB per °/s. "Up" is +accel (specific force), see pointer().

#define RED 0x8072
#define CONNECT_EVT 0x803E
#define IDLE_EVT 0x803F
#define LONG_PRESS_US (800 * 1000)
#define RED_REPLAY_FRAMES 5
#define MOTION_LSB 150  // gyro deviation that counts as activity (for the sleep timers)

// Pointer tuning (base speed and acceleration strength are in settings)
#define ACCEL_FROM_LSB 300.0f   // below this rotation rate (~18 °/s): base speed only
#define ACCEL_SPAN_LSB 700.0f   // each further 700 LSB (~41 °/s) adds `accel` × base speed
#define ACCEL_MAX_STEPS 4.0f    // capped at 1 + 4 × accel
#define STILL_LSB 40
#define STILL_SAMPLES 50
#define ACC_ALPHA 0.05f
#define CLICK_FREEZE_US (150 * 1000)

enum { OUT_KEY, OUT_CONSUMER, OUT_MOUSE };
static const struct {
    uint16_t code;
    uint8_t type;
    uint16_t usage;  // keyboard usage / consumer usage / mouse button bit
} KEYMAP[] = {
    {0x8040, OUT_KEY, HID_KEY_ARROW_UP},
    {0x8041, OUT_KEY, HID_KEY_ARROW_DOWN},
    {0x8007, OUT_KEY, HID_KEY_ARROW_LEFT},
    {0x8006, OUT_KEY, HID_KEY_ARROW_RIGHT},
    {0x8044, OUT_MOUSE, MOUSE_BUTTON_LEFT},       // OK / wheel click
    {0x8043, OUT_MOUSE, MOUSE_BUTTON_RIGHT},      // Settings (MR20 code, untested)
    {0x8028, OUT_KEY, HID_KEY_ESCAPE},            // Back
    {0x807C, OUT_KEY, HID_KEY_GUI_LEFT},          // Home → Start menu
    {0x8002, OUT_CONSUMER, HID_USAGE_CONSUMER_VOLUME_INCREMENT},
    {0x8003, OUT_CONSUMER, HID_USAGE_CONSUMER_VOLUME_DECREMENT},
    {0x8009, OUT_CONSUMER, HID_USAGE_CONSUMER_MUTE},
    {0x8000, OUT_KEY, HID_KEY_PAGE_UP},           // Ch+
    {0x8001, OUT_KEY, HID_KEY_PAGE_DOWN},         // Ch-
    {0x80B0, OUT_CONSUMER, HID_USAGE_CONSUMER_PLAY_PAUSE},  // Play (MR20 code, untested)
    {0x80BA, OUT_CONSUMER, HID_USAGE_CONSUMER_PLAY_PAUSE},  // Pause (MR20 code, untested)
    {0x8071, OUT_KEY, HID_KEY_F2},                // Green (MR20 code)
    {0x8063, OUT_KEY, HID_KEY_F3},                // Yellow (MR20 code)
    {0x8061, OUT_KEY, HID_KEY_F4},                // Blue (MR20 code)
    {0x8010, OUT_KEY, HID_KEY_0}, {0x8011, OUT_KEY, HID_KEY_1}, {0x8012, OUT_KEY, HID_KEY_2},
    {0x8013, OUT_KEY, HID_KEY_3}, {0x8014, OUT_KEY, HID_KEY_4}, {0x8015, OUT_KEY, HID_KEY_5},
    {0x8016, OUT_KEY, HID_KEY_6}, {0x8017, OUT_KEY, HID_KEY_7}, {0x8018, OUT_KEY, HID_KEY_8},
    {0x8019, OUT_KEY, HID_KEY_9},
};

static pc_io_t io;
static bridge_mode_t mode = MODE_TV;
static int64_t red_down_at = -1;
static bool red_consumed;
static int red_replay;
static uint16_t last_btn;
static uint8_t mouse_buttons;
static uint8_t last_report[19];

static float bias[3], grav[3];
static bool grav_init;
static float still_sum[3];
static int still_n;
static int64_t freeze_until;
static int64_t last_activity;
static bool sleep_sent;
static bool tv_sleep_pending;
static bool timing_log;

static int find_key(uint16_t code)
{
    for (size_t i = 0; i < sizeof(KEYMAP) / sizeof(KEYMAP[0]); i++) {
        if (KEYMAP[i].code == code) {
            return i;
        }
    }
    return -1;
}

static void output(uint16_t code, bool down)
{
    int i = find_key(code);
    if (i < 0) {
        if (down) {
            ESP_LOGI(TAG, "unmapped button 0x%04x", code);
        }
        return;
    }
    ESP_LOGI(TAG, "USB %s 0x%04x %s", KEYMAP[i].type == OUT_KEY ? "key" : KEYMAP[i].type == OUT_CONSUMER ? "consumer" : "mouse",
             KEYMAP[i].usage, down ? "down" : "up");
    switch (KEYMAP[i].type) {
    case OUT_KEY:
        usb_key(KEYMAP[i].usage, down);
        break;
    case OUT_CONSUMER:
        usb_consumer(KEYMAP[i].usage, down);
        break;
    case OUT_MOUSE:
        mouse_buttons = down ? (mouse_buttons | KEYMAP[i].usage) : (mouse_buttons & ~KEYMAP[i].usage);
        usb_mouse_buttons(mouse_buttons);
        break;
    }
}

static void send_event_to_tv(uint16_t evt)
{
    uint8_t r[19];
    memcpy(r, last_report, sizeof(r));
    r[16] = evt >> 8;
    r[17] = evt & 0xff;
    r[18] = 0;
    io.to_tv(r, sizeof(r));
}

void pc_toggle(void)
{
    if (last_btn && last_btn != RED) {
        output(last_btn, false);  // release whatever is held
    }
    last_btn = 0;
    mode = mode == MODE_TV ? MODE_PC : MODE_TV;
    ESP_LOGI(TAG, "MODE → %s", mode == MODE_PC ? "PC" : "TV");
    // Tell the TV the remote went idle (it hides the cursor) / came back.
    send_event_to_tv(mode == MODE_PC ? IDLE_EVT : CONNECT_EVT);
    last_activity = esp_timer_get_time();
    sleep_sent = false;
}

// Roll-compensated gyro → cursor (same math as prototype/pc_mode.py).
static void pointer(const int16_t g[3], const int16_t a[3], int64_t now, float *dx, float *dy)
{
    *dx = *dy = 0;
    if (!grav_init) {
        for (int i = 0; i < 3; i++) {
            grav[i] = a[i];
        }
        grav_init = true;
    }
    for (int i = 0; i < 3; i++) {
        grav[i] += ACC_ALPHA * (a[i] - grav[i]);
    }

    bool still = true;
    for (int i = 0; i < 3; i++) {
        still &= fabsf(g[i] - bias[i]) < STILL_LSB;
    }
    if (still) {
        for (int i = 0; i < 3; i++) {
            still_sum[i] += g[i];
        }
        if (++still_n >= STILL_SAMPLES) {
            for (int i = 0; i < 3; i++) {
                bias[i] = still_sum[i] / still_n;
                still_sum[i] = 0;
            }
            still_n = 0;
        }
    } else {
        memset(still_sum, 0, sizeof(still_sum));
        still_n = 0;
    }

    float w[3] = {g[0] - bias[0], g[1] - bias[1], g[2] - bias[2]};
    float n = sqrtf(grav[0] * grav[0] + grav[1] * grav[1] + grav[2] * grav[2]);
    if (n < 1) {
        return;
    }
    // The accelerometer reports specific force (points up at rest), the IMU is mounted upside down (flat, face up:
    // z ≈ -1 g). The readings alone fit "gravity vector, Z up" equally well, but that gives mirrored directions;
    // the guided right/left/up/down test (2026-11-14) decided it.
    float up[3] = {grav[0] / n, grav[1] / n, grav[2] / n};
    // right = forward × up with forward = (0, 1, 0)
    float right[3] = {up[2], 0, -up[0]};
    float rn = sqrtf(right[0] * right[0] + right[2] * right[2]);
    if (rn < 1e-3f) {
        return;  // pointing straight up/down
    }
    right[0] /= rn;
    right[2] /= rn;
    float yaw = w[0] * up[0] + w[1] * up[1] + w[2] * up[2];  // + = turning left
    float pitch = w[0] * right[0] + w[2] * right[2];         // + = tip going up
    if (now < freeze_until) {
        return;
    }
    // Acceleration: precise when moving slowly, fast flicks travel further.
    float rate = sqrtf(yaw * yaw + pitch * pitch);
    float steps = (rate - ACCEL_FROM_LSB) / ACCEL_SPAN_LSB;
    steps = steps < 0 ? 0 : steps > ACCEL_MAX_STEPS ? ACCEL_MAX_STEPS : steps;
    float k = settings.gain * (1.0f + settings.accel * steps);
    *dx = -yaw * k;  // sub-pixel: usb.c keeps the remainder
    *dy = -pitch * k;
}

void pc_on_fd(const uint8_t *d, uint16_t len)
{
    if (len < 19) {
        io.to_tv(d, len);
        return;
    }
    int64_t now = esp_timer_get_time();
    uint8_t r[19];
    memcpy(r, d, sizeof(r));
    memcpy(last_report, d, sizeof(last_report));
    uint16_t btn = (r[16] << 8) | r[17];
    int8_t wheel = (int8_t)r[18];

    // Red: held back in both modes. Long press toggles; a short press is replayed to the TV in TV mode.
    if (btn == RED) {
        if (red_down_at < 0) {
            red_down_at = now;
            red_consumed = false;
        } else if (!red_consumed && now - red_down_at >= LONG_PRESS_US) {
            red_consumed = true;
            pc_toggle();
        }
        r[16] = r[17] = 0;
        btn = 0;
    } else if (red_down_at >= 0) {
        if (!red_consumed && mode == MODE_TV) {
            red_replay = RED_REPLAY_FRAMES;
        }
        red_down_at = -1;
    }

    int16_t g[3], a[3];
    for (int i = 0; i < 3; i++) {
        g[i] = (int16_t)((r[4 + 2 * i] << 8) | r[5 + 2 * i]);
        a[i] = (int16_t)((r[10 + 2 * i] << 8) | r[11 + 2 * i]);
    }
    bool active = (btn && btn != CONNECT_EVT && btn != IDLE_EVT) || wheel || red_down_at >= 0;
    for (int i = 0; i < 3; i++) {
        active |= fabsf(g[i] - bias[i]) > MOTION_LSB;
    }
    if (active) {
        last_activity = now;
        sleep_sent = false;
        tv_sleep_pending = false;  // the TV will ask again once it sees the remote idle
    }

    if (mode == MODE_TV) {
        // A held-back sleep command from the TV goes out once the remote has been idle for `tvsleep` s.
        if (tv_sleep_pending && now - last_activity >= (int64_t)settings.tv_sleep * 1000000) {
            static const uint8_t sleep_cmd[] = {0x02};
            io.to_remote(sleep_cmd, 1);
            tv_sleep_pending = false;
            ESP_LOGI(TAG, "TV mode: idle %lus, forwarded the TV's sleep command", (unsigned long)settings.tv_sleep);
        }
        if (red_replay && btn == 0) {
            red_replay--;
            r[16] = RED >> 8;
            r[17] = RED & 0xff;
        }
        last_btn = btn;
        io.to_tv(r, sizeof(r));
        return;
    }

    /* PC mode */
    if (btn == CONNECT_EVT || btn == IDLE_EVT) {
        btn = last_btn;
    }
    if (btn != last_btn) {
        if (last_btn) {
            output(last_btn, false);
        }
        if (btn) {
            output(btn, true);
        }
        int li = find_key(last_btn), bi = find_key(btn);
        if ((li >= 0 && KEYMAP[li].type == OUT_MOUSE) || (bi >= 0 && KEYMAP[bi].type == OUT_MOUSE)) {
            freeze_until = now + CLICK_FREEZE_US;
        }
        last_btn = btn;
    }

    // Timing stats ("timing" on the console): report spacing as it arrives over BLE, lost reports (seq gaps),
    // and how many USB motion reports went out, once per second while the remote moves.
    static int64_t st_last, st_since;
    static uint8_t st_seq;
    static uint32_t st_n, st_lost, st_hist[5], st_max, st_usb0;
    static bool st_moving;
    if (st_last) {
        uint32_t gap = (uint32_t)((now - st_last) / 1000);
        st_hist[gap < 5 ? 0 : gap < 15 ? 1 : gap < 25 ? 2 : gap < 45 ? 3 : 4]++;
        st_max = gap > st_max ? gap : st_max;
        st_lost += (uint8_t)(r[1] - st_seq - 1);
    }
    st_last = now;
    st_seq = r[1];
    st_n++;
    st_moving |= active;
    if (now - st_since >= 1000000) {
        if (st_moving && timing_log) {
            ESP_LOGI(TAG, "timing: %lu reports, gaps <5ms %lu, 5-15 %lu, 15-25 %lu, 25-45 %lu, >45 %lu, max %lums, "
                          "seq lost %lu, USB motion reports %lu",
                     (unsigned long)st_n, (unsigned long)st_hist[0], (unsigned long)st_hist[1],
                     (unsigned long)st_hist[2], (unsigned long)st_hist[3], (unsigned long)st_hist[4],
                     (unsigned long)st_max, (unsigned long)st_lost, (unsigned long)(usb_mouse_reports - st_usb0));
        }
        st_n = st_lost = st_max = 0;
        memset(st_hist, 0, sizeof(st_hist));
        st_usb0 = usb_mouse_reports;
        st_since = now;
        st_moving = false;
    }

    float dx, dy;
    pointer(g, a, now, &dx, &dy);
    if (dx || dy || wheel) {
        usb_mouse_motion(dx, dy, wheel);
    }
    // Debug summary (no screen on the dev box): motion per 0.5 s.
    static float sum_x, sum_y;
    static int sum_w;
    static int64_t sum_since;
    sum_x += dx;
    sum_y += dy;
    sum_w += wheel;
    if (now - sum_since > 500 * 1000) {
        if (fabsf(sum_x) >= 1 || fabsf(sum_y) >= 1 || sum_w) {
            ESP_LOGI(TAG, "USB motion dx=%+5.0f dy=%+5.0f wheel=%+d", sum_x, sum_y, sum_w);
        }
        sum_x = sum_y = sum_w = 0;
        sum_since = now;
    }

    // Idle → put the remote to sleep like the TV does (otherwise it streams at 100 Hz forever).
    if (settings.pc_sleep && !sleep_sent && now - last_activity > (int64_t)settings.pc_sleep * 1000000) {
        static const uint8_t sleep_cmd[] = {0x02};
        io.to_remote(sleep_cmd, 1);
        sleep_sent = true;
        ESP_LOGI(TAG, "PC mode: idle %lus, sent sleep (02) to the remote", (unsigned long)settings.pc_sleep);
    }
}

void pc_init(const pc_io_t *i)
{
    io = *i;
    last_activity = esp_timer_get_time();
}

bridge_mode_t pc_mode(void)
{
    return mode;
}

bool pc_tv_sleep_request(void)
{
    if (!settings.tv_sleep) {
        return true;  // pass the TV's sleep command on right away (native behaviour)
    }
    if (!tv_sleep_pending) {
        ESP_LOGI(TAG, "TV asked the remote to sleep: holding it until %lus idle", (unsigned long)settings.tv_sleep);
    }
    tv_sleep_pending = true;
    return false;
}

void pc_toggle_timing_log(void)
{
    timing_log = !timing_log;
    ESP_LOGI(TAG, "timing log %s", timing_log ? "on" : "off");
}

void pc_status(void)
{
    ESP_LOGI(TAG, "mode=%s bias=%.1f/%.1f/%.1f idle=%llds", mode == MODE_PC ? "PC" : "TV", bias[0], bias[1], bias[2],
             (esp_timer_get_time() - last_activity) / 1000000);
}
