#include "pc.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "class/hid/hid.h"
#include "keymap.h"
#include "settings.h"
#include "usb.h"

static const char *TAG = "pc";

// FD report (19 bytes): [0] ?, [1] seq, [2:4] status, [4:10] gyro x/y/z, [10:16] accel x/y/z (int16 BE),
// [16:18] button (u16 BE), [18] wheel (int8). Gyro and accel share one frame (checked against each other),
// gyro ≈16.9 LSB per °/s. "Up" is +accel (specific force), see pointer().

#define CONNECT_EVT 0x803E
#define IDLE_EVT 0x803F
#define SWITCH_REPLAY_FRAMES 5

// Pointer tuning (base speed and acceleration strength are in settings)
#define ACCEL_FROM_LSB 300.0f   // below this rotation rate (~18 °/s): base speed only
#define ACCEL_SPAN_LSB 700.0f   // each further 700 LSB (~41 °/s) adds `accel` × base speed
#define ACCEL_MAX_STEPS 4.0f    // capped at 1 + 4 × accel
// Gyro bias: re-estimated when the readings stay within STILL_SPREAD_LSB (per axis) for STILL_SAMPLES reports.
// At rest the noise is ~1 LSB; a hand-held remote varies far more. The old test ("within 40 LSB of the current
// bias for 0.5 s") also matched slow aiming movements (< 2.4 °/s), learned them as bias, and the cursor then
// drifted the other way.
#define STILL_SPREAD_LSB 10
#define STILL_SAMPLES 100
// Activity for the sleep timers = not resting for REST_SAMPLES reports. The old test (gyro > 150 LSB, ~9 °/s)
// counted holding the remote steady while aiming as idle, so the remote was put to sleep in the user's hand and
// the cursor froze until a key press woke it.
#define REST_SAMPLES 50
#define ACC_ALPHA 0.05f
#define CLICK_FREEZE_US (150 * 1000)

static pc_io_t io;
static bridge_mode_t mode = MODE_TV;
// Switch key: held back in both modes; a long press toggles the mode, a short one is replayed to the TV (TV mode)
// or sent as its mapping (PC mode).
static int64_t sw_down_at = -1;
static uint16_t sw_code;
static bool sw_consumed;
static int sw_replay;
static uint16_t last_btn;
// What the held button sent: the release must match the press even if the map was edited in between.
static keymap_entry_t held;
static uint8_t mouse_buttons;
static uint8_t last_report[19];

static float bias[3], grav[3];
// The bias is kept in NVS: it is only learned while the remote lies still, and starting from 0 after every reset
// made the cursor drift hard until the remote was put down once.
#define BIAS_SAVE_PERIOD_MS (60 * 1000)
#define BIAS_SAVE_MIN_CHANGE 2.0f
static float bias_saved[3];
static bool bias_learned, bias_stored;  // learned since boot / a value is in NVS
static bool grav_init;
static float still_sum[3];
static int16_t still_min[3], still_max[3];
static int still_n;
static bool resting;
// Between its idle (0x803F) and active (0x803E) codes the remote keeps sending reports but its gyro is off and the
// last values repeat. Taken at face value they look like a remote at rest: they were learned as the bias (50x the
// real one), and the cursor drifted hard once real data came back.
static bool imu_off;
static int16_t g_prev[3];
static int g_changing, g_same;  // consecutive reports whose gyro reading differed from / equalled the previous one
// A live gyro has ~1 LSB of noise on every axis, so 5 identical readings in a row don't happen at rest.
#define IMU_LIVE_REPORTS 3
#define IMU_FROZEN_REPORTS 5
static int64_t freeze_until;
static bool drag_lock;
static float lock_x, lock_y;
static int64_t last_activity;
static bool sleep_sent;
static bool tv_sleep_pending;
static bool timing_log;

static void send(const keymap_entry_t *e, bool down)
{
    ESP_LOGI(TAG, "USB %s 0x%02x+0x%02x %s", e->type == OUT_KEY ? "key" : e->type == OUT_CONSUMER ? "consumer" : "mouse",
             e->usage, e->mods, down ? "down" : "up");
    switch (e->type) {
    case OUT_KEY:
        // Modifiers go down before the key and up after it, as when typed.
        if (!down && e->usage) {
            usb_key(e->usage, false);
        }
        for (int b = 0; b < 8; b++) {
            if (e->mods & (1 << b)) {
                usb_key(HID_KEY_CONTROL_LEFT + b, down);
            }
        }
        if (down && e->usage) {
            usb_key(e->usage, true);
        }
        break;
    case OUT_CONSUMER:
        usb_consumer(e->usage, down);
        break;
    case OUT_MOUSE:
        mouse_buttons = down ? (mouse_buttons | e->usage) : (mouse_buttons & ~e->usage);
        usb_mouse_buttons(mouse_buttons);
        break;
    }
}

static void press(uint16_t code)
{
    if (!keymap_find(code, &held)) {
        held.type = OUT_NONE;
        ESP_LOGI(TAG, "unmapped button 0x%04x", code);
        return;
    }
    send(&held, true);
}

static void release(void)
{
    if (held.type != OUT_NONE) {
        send(&held, false);
    }
    held.type = OUT_NONE;
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
    release();  // whatever is held
    last_btn = 0;
    mode = mode == MODE_TV ? MODE_PC : MODE_TV;
    ESP_LOGI(TAG, "MODE → %s", mode == MODE_PC ? "PC" : "TV");
    // Tell the TV the remote went idle (it hides the cursor) / came back.
    send_event_to_tv(mode == MODE_PC ? IDLE_EVT : CONNECT_EVT);
    last_activity = esp_timer_get_time();
    sleep_sent = false;
}

// Tracks whether the remote is resting (readings within STILL_SPREAD_LSB) and re-estimates the gyro bias after
// STILL_SAMPLES resting reports. Runs in both modes, before the activity check.
static void track_rest(const int16_t g[3])
{
    bool still = true;
    for (int i = 0; i < 3; i++) {
        int16_t lo = still_n ? (g[i] < still_min[i] ? g[i] : still_min[i]) : g[i];
        int16_t hi = still_n ? (g[i] > still_max[i] ? g[i] : still_max[i]) : g[i];
        still &= hi - lo <= STILL_SPREAD_LSB;
        still_min[i] = lo;
        still_max[i] = hi;
    }
    resting = still && (resting || still_n + 1 >= REST_SAMPLES);
    if (!still) {
        // start a new window with this sample
        for (int i = 0; i < 3; i++) {
            still_min[i] = still_max[i] = g[i];
            still_sum[i] = 0;
        }
        still_n = 0;
    }
    for (int i = 0; i < 3; i++) {
        still_sum[i] += g[i];
    }
    if (++still_n >= STILL_SAMPLES) {
        // A gyro at rest still has ~1 LSB of noise: a window of identical readings is frozen data, not a bias.
        bool frozen = true;
        for (int i = 0; i < 3; i++) {
            frozen &= still_max[i] == still_min[i];
        }
        if (frozen) {
            for (int i = 0; i < 3; i++) {
                still_sum[i] = 0;
            }
            still_n = 0;
            return;
        }
        float nb[3], change = 0;
        for (int i = 0; i < 3; i++) {
            nb[i] = still_sum[i] / still_n;
            change = fmaxf(change, fabsf(nb[i] - bias[i]));
            bias[i] = nb[i];
            still_sum[i] = 0;
        }
        bias_learned = true;
        still_n = 0;
        if (change >= 1.0f) {
            ESP_LOGI(TAG, "gyro bias %.1f/%.1f/%.1f (moved %.1f)", bias[0], bias[1], bias[2], change);
        }
    }
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
    // Soft dead zone (`dead` setting): a hand never holds the remote perfectly still, and at higher gains that slow
    // rotation shows as the cursor creeping. Subtracting instead of cutting keeps slow aiming continuous.
    float dead = settings.dead;
    if (rate <= dead) {
        return;
    }
    yaw *= (rate - dead) / rate;
    pitch *= (rate - dead) / rate;
    rate -= dead;
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

    bool sw_tap = false;
    if (btn && btn == settings.switch_key) {
        if (sw_down_at < 0) {
            sw_down_at = now;
            sw_code = btn;
            sw_consumed = false;
        }
        // switchhold 0: switch on the press itself; the key then has no other function
        if (!sw_consumed && now - sw_down_at >= (int64_t)settings.switch_hold * 1000) {
            sw_consumed = true;
            pc_toggle();
        }
        r[16] = r[17] = 0;
        btn = 0;
    } else if (sw_down_at >= 0 && btn != CONNECT_EVT && btn != IDLE_EVT) {
        // The remote's status codes are not a release: it sends one (e.g. 0x803F) right in the middle of a held key,
        // and taking that as a release made a held switch key switch back again.
        if (!sw_consumed) {
            if (mode == MODE_TV) {
                sw_replay = SWITCH_REPLAY_FRAMES;
            } else {
                sw_tap = true;
            }
        }
        sw_down_at = -1;
    }

    int16_t g[3], a[3];
    for (int i = 0; i < 3; i++) {
        g[i] = (int16_t)((r[4 + 2 * i] << 8) | r[5 + 2 * i]);
        a[i] = (int16_t)((r[10 + 2 * i] << 8) | r[11 + 2 * i]);
    }
    bool active = (btn && btn != CONNECT_EVT && btn != IDLE_EVT) || wheel || sw_down_at >= 0;
    // The codes alone aren't enough (a lost report, live data for a moment after the idle code): the readings
    // themselves decide too. Frozen readings never change; live ones always do.
    bool changed = memcmp(g, g_prev, sizeof(g_prev)) != 0;
    memcpy(g_prev, g, sizeof(g_prev));
    g_changing = changed ? g_changing + 1 : 0;
    g_same = changed ? 0 : g_same + 1;
    bool off = imu_off;
    if (btn == IDLE_EVT || g_same >= IMU_FROZEN_REPORTS) {
        off = true;
    } else if (btn == CONNECT_EVT || g_changing >= IMU_LIVE_REPORTS) {
        off = false;
    }
    if (off != imu_off) {
        imu_off = off;
        ESP_LOGI(TAG, "remote gyro %s", imu_off ? "off (frozen readings)" : "on");
    }
    if (!imu_off) {
        track_rest(g);
        active |= !resting;
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
        if (sw_replay && btn == 0) {
            sw_replay--;
            r[16] = sw_code >> 8;
            r[17] = sw_code & 0xff;
        }
        last_btn = btn;
        io.to_tv(r, sizeof(r));
        return;
    }

    /* PC mode */
    if (btn == CONNECT_EVT || btn == IDLE_EVT) {
        btn = last_btn;
    }
    if (sw_tap) {
        press(sw_code);
        release();
    }
    if (btn != last_btn) {
        bool was_down = held.type == OUT_MOUSE;
        release();
        if (btn) {
            press(btn);
        }
        if (was_down || held.type == OUT_MOUSE) {
            freeze_until = now + CLICK_FREEZE_US;
        }
        // A click must not turn into a drag: the hand moves a little while pressing (after the freeze above, and
        // during a slower click). With a mouse button down, the pointer stays put until it has travelled `drag` px.
        if (mouse_buttons && !was_down) {
            drag_lock = settings.drag > 0;
            lock_x = lock_y = 0;
        } else if (!mouse_buttons) {
            drag_lock = false;
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
    if (imu_off) {
        dx = dy = 0;
    } else {
        pointer(g, a, now, &dx, &dy);
    }
    if (drag_lock) {
        lock_x += dx;
        lock_y += dy;
        dx = dy = 0;
        if (hypotf(lock_x, lock_y) >= settings.drag) {
            // a real drag: catch up so the cursor ends where the remote points
            dx = lock_x;
            dy = lock_y;
            drag_lock = false;
        }
    }
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

// Saves the bias from its own task: an NVS write takes milliseconds and must not happen in the report path.
// Small changes (temperature) are not worth a flash write.
static void bias_save_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BIAS_SAVE_PERIOD_MS));
        float b[3], change = 0;
        for (int i = 0; i < 3; i++) {
            b[i] = bias[i];
            change = fmaxf(change, fabsf(b[i] - bias_saved[i]));
        }
        if (!bias_learned || (bias_stored && change < BIAS_SAVE_MIN_CHANGE)) {
            continue;
        }
        nvs_handle_t h;
        if (nvs_open("mrb", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "bias", b, sizeof(b));
            nvs_commit(h);
            nvs_close(h);
            memcpy(bias_saved, b, sizeof(b));
            bias_stored = true;
            ESP_LOGI(TAG, "gyro bias saved");
        }
    }
}

void pc_init(const pc_io_t *i)
{
    io = *i;
    last_activity = esp_timer_get_time();
    nvs_handle_t h;
    if (nvs_open("mrb", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(bias);
        if (nvs_get_blob(h, "bias", bias, &len) == ESP_OK && len == sizeof(bias)) {
            memcpy(bias_saved, bias, sizeof(bias));
            bias_stored = true;
            ESP_LOGI(TAG, "gyro bias %.1f/%.1f/%.1f (stored)", bias[0], bias[1], bias[2]);
        }
        nvs_close(h);
    }
    xTaskCreate(bias_save_task, "bias", 2560, NULL, 1, NULL);
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
