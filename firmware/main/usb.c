#include "usb.h"

#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_private/periph_ctrl.h"
#include "soc/rtc_cntl_reg.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "tusb_console.h"
#include "class/hid/hid_device.h"

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
    ESP_LOGI(TAG, "USB up (CDC console + HID keyboard/mouse/consumer)");
}

bool usb_hid_mounted(void)
{
    return tud_mounted();
}

/* ---- HID ---- */

static bool hid_wait_ready(void)
{
    for (int i = 0; i < 20; i++) {
        if (!tud_mounted()) {
            return false;
        }
        if (tud_suspended()) {
            tud_remote_wakeup();
        }
        if (tud_hid_ready()) {
            return true;
        }
        vTaskDelay(1);
    }
    return false;
}

static uint8_t kbd_modifiers;
static uint8_t kbd_keys[6];

static void kbd_send(void)
{
    if (hid_wait_ready()) {
        tud_hid_keyboard_report(RID_KEYBOARD, kbd_modifiers, kbd_keys);
    }
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

static uint8_t mouse_buttons;

void usb_mouse_buttons(uint8_t buttons)
{
    mouse_buttons = buttons;
    if (hid_wait_ready()) {
        tud_hid_mouse_report(RID_MOUSE, mouse_buttons, 0, 0, 0, 0);
    }
}

static int8_t clamp8(int v)
{
    return v > 127 ? 127 : v < -127 ? -127 : v;
}

void usb_mouse_move(int dx, int dy, int wheel)
{
    while (dx || dy || wheel) {
        int8_t x = clamp8(dx), y = clamp8(dy), w = clamp8(wheel);
        if (!hid_wait_ready()) {
            return;
        }
        tud_hid_mouse_report(RID_MOUSE, mouse_buttons, x, y, w, 0);
        dx -= x;
        dy -= y;
        wheel -= w;
    }
}

void usb_consumer(uint16_t usage, bool down)
{
    uint16_t value = down ? usage : 0;
    if (hid_wait_ready()) {
        tud_hid_report(RID_CONSUMER, &value, sizeof(value));
    }
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
