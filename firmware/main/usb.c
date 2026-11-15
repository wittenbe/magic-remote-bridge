#include "usb.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_private/periph_ctrl.h"
#include "soc/rtc_cntl_reg.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "tusb_console.h"
#include "class/hid/hid_device.h"
#include "settings.h"

static const char *TAG = "usb";

enum { RID_KEYBOARD = 1, RID_MOUSE = 2, RID_CONSUMER = 3 };

static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(RID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(RID_MOUSE)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(RID_CONSUMER)),
};

enum { ITF_CDC_CTRL, ITF_CDC_DATA, ITF_HID, ITF_COUNT };
#define EP_CDC_NOTIF 0x81
#define EP_CDC_OUT   0x02
#define EP_CDC_IN    0x82
#define EP_HID_IN    0x83
#define DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC_CTRL, 4, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(ITF_HID, 5, HID_ITF_PROTOCOL_NONE, sizeof(hid_report_descriptor), EP_HID_IN, 16, 1),
};

static const char *string_descriptor[] = {
    (char[]){0x09, 0x04},     // 0: English
    "rc",                     // 1: manufacturer
    "Magic Remote Bridge",    // 2: product
    "MRB-0001",               // 3: serial
    "Bridge console",         // 4: CDC
    "Magic Remote",           // 5: HID
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
}

/* ---- CDC line input ---- */

static usb_line_cb_t line_cb;
static char line_buf[128];
static size_t line_len;

static void cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
    size_t n = 0;
    if (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &n) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        char c = (char)buf[i];
        if (c == '\r' || c == '\n') {
            if (line_len) {
                line_buf[line_len] = 0;
                line_len = 0;
                if (line_cb) {
                    line_cb(line_buf);
                }
            }
        } else if (line_len < sizeof(line_buf) - 1) {
            line_buf[line_len++] = c;
        }
    }
}

static void cdc_line_coding_cb(int itf, cdcacm_event_t *event)
{
    // Arduino convention: opening the port at 1200 baud means "go to the bootloader".
    if (event->line_coding_changed_data.p_line_coding->bit_rate == 1200) {
        usb_reboot_to_download();
    }
}

void usb_init(usb_line_cb_t on_line)
{
    line_cb = on_line;
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,  // default: Espressif VID, class IAD (needed for CDC in a composite)
        .string_descriptor = string_descriptor,
        .string_descriptor_count = sizeof(string_descriptor) / sizeof(string_descriptor[0]),
        .external_phy = false,
        .configuration_descriptor = configuration_descriptor,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_cb,
        .callback_line_coding_changed = cdc_line_coding_cb,
    };
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));
    esp_tusb_init_console(TINYUSB_CDC_ACM_0);  // ESP_LOG / printf → CDC
    usb_hid_start();
    ESP_LOGI(TAG, "USB up (CDC console + HID keyboard/mouse/consumer)");
}

bool usb_hid_mounted(void)
{
    return tud_mounted();
}

/* ---- HID ----
 * All HID reports go out from one task. Callers (the BLE host task, the console) only queue: waiting for the
 * endpoint there stalled the BLE path for a whole FreeRTOS tick, and two tasks racing for the endpoint can lose
 * reports. Motion is not queued but accumulated and paced, see hid_task(). */

#define HID_QUEUE_LEN 16

typedef struct {
    uint8_t id;
    uint8_t len;
    uint8_t data[8];
} hid_msg_t;

static QueueHandle_t hid_queue;
static portMUX_TYPE motion_lock = portMUX_INITIALIZER_UNLOCKED;
static float motion_backlog[2], motion_step[2];  // px still to send, px per ms
static int motion_wheel;
static uint8_t mouse_buttons;
volatile uint32_t usb_mouse_reports;  // motion reports sent (pointer timing stats in pc.c)

static void hid_queue_report(uint8_t id, const void *data, uint8_t len)
{
    hid_msg_t m = {.id = id, .len = len};
    memcpy(m.data, data, len);
    if (!hid_queue || xQueueSend(hid_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "HID queue full, report %u dropped", id);
    }
}

static int8_t clamp8(float v)
{
    return v > 127 ? 127 : v < -127 ? -127 : (int8_t)v;
}

// Remote reports arrive at 100 Hz, and BLE delivers them with jitter (sometimes two in one connection event).
// Forwarding each one as a single USB report moved the cursor in visible 10 ms jumps. Instead every report's
// motion is spread evenly over the next `smooth` ms and sent at the 1 kHz USB poll rate; a backlog from a late
// burst is caught up within the same window. Costs `smooth` ms of latency.
static void hid_task(void *arg)
{
    float out[2] = {0, 0};  // sub-pixel remainder already "sent"
    for (;;) {
        vTaskDelay(1);  // 1 ms (CONFIG_FREERTOS_HZ=1000)
        if (!tud_mounted()) {
            continue;
        }

        bool motion = false;
        int wheel;
        portENTER_CRITICAL(&motion_lock);
        for (int i = 0; i < 2; i++) {
            float step = motion_step[i];
            if (fabsf(step) > fabsf(motion_backlog[i]) || step * motion_backlog[i] < 0) {
                step = motion_backlog[i];
            }
            motion_backlog[i] -= step;
            out[i] += step;
        }
        wheel = motion_wheel;
        portEXIT_CRITICAL(&motion_lock);
        motion = fabsf(out[0]) >= 1 || fabsf(out[1]) >= 1 || wheel;

        hid_msg_t m;
        bool queued = uxQueueMessagesWaiting(hid_queue) > 0;
        if (!queued && !motion) {
            continue;
        }
        if (tud_suspended()) {
            tud_remote_wakeup();
            continue;
        }
        if (!tud_hid_ready()) {
            continue;  // keep accumulating, nothing is lost
        }
        if (queued && xQueueReceive(hid_queue, &m, 0) == pdTRUE) {
            tud_hid_report(m.id, m.data, m.len);
            continue;  // buttons/keys first; motion goes out on the next poll
        }
        int8_t x = clamp8(out[0]), y = clamp8(out[1]), w = clamp8(wheel);
        if (tud_hid_mouse_report(RID_MOUSE, mouse_buttons, x, y, w, 0)) {
            usb_mouse_reports++;
            out[0] -= x;
            out[1] -= y;
            portENTER_CRITICAL(&motion_lock);
            motion_wheel -= w;
            portEXIT_CRITICAL(&motion_lock);
        }
    }
}

void usb_hid_start(void)
{
    hid_queue = xQueueCreate(HID_QUEUE_LEN, sizeof(hid_msg_t));
    xTaskCreate(hid_task, "hid", 3072, NULL, 5, NULL);
}

static uint8_t kbd_modifiers;
static uint8_t kbd_keys[6];

static void kbd_send(void)
{
    uint8_t r[8] = {kbd_modifiers, 0};
    memcpy(&r[2], kbd_keys, 6);
    hid_queue_report(RID_KEYBOARD, r, sizeof(r));
}

void usb_key(uint8_t usage, bool down)
{
    if (usage >= HID_KEY_CONTROL_LEFT && usage <= HID_KEY_GUI_RIGHT) {
        uint8_t bit = 1 << (usage - HID_KEY_CONTROL_LEFT);
        kbd_modifiers = down ? (kbd_modifiers | bit) : (kbd_modifiers & ~bit);
    } else {
        int free_slot = -1;
        for (int i = 0; i < 6; i++) {
            if (kbd_keys[i] == usage) {
                if (!down) {
                    kbd_keys[i] = 0;
                }
                free_slot = -2;  // already present
                break;
            }
            if (!kbd_keys[i] && free_slot == -1) {
                free_slot = i;
            }
        }
        if (down && free_slot >= 0) {
            kbd_keys[free_slot] = usage;
        }
    }
    kbd_send();
}

void usb_mouse_buttons(uint8_t buttons)
{
    mouse_buttons = buttons;
    uint8_t r[5] = {buttons, 0, 0, 0, 0};  // buttons, x, y, wheel, pan (TUD_HID_REPORT_DESC_MOUSE)
    hid_queue_report(RID_MOUSE, r, sizeof(r));
}

void usb_mouse_motion(float dx, float dy, int wheel)
{
    uint32_t ticks = settings.smooth ? settings.smooth : 1;
    portENTER_CRITICAL(&motion_lock);
    motion_backlog[0] += dx;
    motion_backlog[1] += dy;
    motion_step[0] = motion_backlog[0] / ticks;
    motion_step[1] = motion_backlog[1] / ticks;
    motion_wheel += wheel;
    portEXIT_CRITICAL(&motion_lock);
}

void usb_mouse_move(int dx, int dy, int wheel)
{
    usb_mouse_motion(dx, dy, wheel);
}

void usb_consumer(uint16_t usage, bool down)
{
    uint16_t value = down ? usage : 0;
    hid_queue_report(RID_CONSUMER, &value, sizeof(value));
}

void usb_reboot_to_download(void)
{
    ESP_LOGW(TAG, "rebooting into download mode");
    vTaskDelay(pdMS_TO_TICKS(50));
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    // The ROM download mode talks over the USB-Serial/JTAG unit, but the PHY is still routed to USB-OTG
    // (TinyUSB) and that routing survives esp_restart(). Switch it back first (as Arduino's ESP32 core does).
    periph_module_reset(PERIPH_USB_MODULE);
    periph_module_disable(PERIPH_USB_MODULE);
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                        RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}
