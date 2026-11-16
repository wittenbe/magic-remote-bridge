#include "keymap.h"

#include <string.h>
#include "class/hid/hid.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "usb.h"

static const char *TAG = "keymap";

// What the remote's labels suggest on a PC. Buttons without an entry do nothing in PC mode.
static const keymap_entry_t DEFAULTS[] = {
    {0x8040, OUT_KEY, 0, HID_KEY_ARROW_UP},
    {0x8041, OUT_KEY, 0, HID_KEY_ARROW_DOWN},
    {0x8007, OUT_KEY, 0, HID_KEY_ARROW_LEFT},
    {0x8006, OUT_KEY, 0, HID_KEY_ARROW_RIGHT},
    {0x8044, OUT_MOUSE, 0, MOUSE_BUTTON_LEFT},   // OK / wheel click
    {0x8043, OUT_MOUSE, 0, MOUSE_BUTTON_RIGHT},  // Settings
    {0x8028, OUT_KEY, 0, HID_KEY_ESCAPE},        // Back
    {0x807C, OUT_KEY, 0, HID_KEY_GUI_LEFT},      // Home → Start menu
    {0x8002, OUT_CONSUMER, 0, HID_USAGE_CONSUMER_VOLUME_INCREMENT},
    {0x8003, OUT_CONSUMER, 0, HID_USAGE_CONSUMER_VOLUME_DECREMENT},
    {0x8009, OUT_CONSUMER, 0, HID_USAGE_CONSUMER_MUTE},
    {0x8000, OUT_KEY, 0, HID_KEY_PAGE_UP},       // Ch+
    {0x8001, OUT_KEY, 0, HID_KEY_PAGE_DOWN},     // Ch-
    {0x8071, OUT_KEY, 0, HID_KEY_F2},            // Green
    {0x8063, OUT_KEY, 0, HID_KEY_F3},            // Yellow
    {0x8061, OUT_KEY, 0, HID_KEY_F4},            // Blue
    {0x8010, OUT_KEY, 0, HID_KEY_0}, {0x8011, OUT_KEY, 0, HID_KEY_1}, {0x8012, OUT_KEY, 0, HID_KEY_2},
    {0x8013, OUT_KEY, 0, HID_KEY_3}, {0x8014, OUT_KEY, 0, HID_KEY_4}, {0x8015, OUT_KEY, 0, HID_KEY_5},
    {0x8016, OUT_KEY, 0, HID_KEY_6}, {0x8017, OUT_KEY, 0, HID_KEY_7}, {0x8018, OUT_KEY, 0, HID_KEY_8},
    {0x8019, OUT_KEY, 0, HID_KEY_9},
};

static keymap_entry_t map[KEYMAP_MAX];
static size_t count;
// Edited from the console (USB task), read from the BLE host task on every button change.
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static void save(void)
{
    keymap_entry_t copy[KEYMAP_MAX];
    portENTER_CRITICAL(&lock);
    size_t n = count;
    memcpy(copy, map, n * sizeof(map[0]));
    portEXIT_CRITICAL(&lock);

    nvs_handle_t h;
    if (nvs_open("mrb", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    // An empty map is stored as a 1-byte marker: a zero-length blob can't be told apart from "never saved".
    uint8_t empty = 0;
    nvs_set_blob(h, "keymap", n ? (const void *)copy : &empty, n ? n * sizeof(copy[0]) : 1);
    nvs_commit(h);
    nvs_close(h);
}

static void set_defaults(void)
{
    portENTER_CRITICAL(&lock);
    count = sizeof(DEFAULTS) / sizeof(DEFAULTS[0]);
    memcpy(map, DEFAULTS, sizeof(DEFAULTS));
    portEXIT_CRITICAL(&lock);
}

void keymap_load(void)
{
    set_defaults();
    nvs_handle_t h;
    if (nvs_open("mrb", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    keymap_entry_t stored[KEYMAP_MAX];
    size_t len = sizeof(stored);
    if (nvs_get_blob(h, "keymap", stored, &len) == ESP_OK) {
        if (len == 1) {
            count = 0;
        } else if (len % sizeof(stored[0]) == 0) {
            count = len / sizeof(stored[0]);
            memcpy(map, stored, len);
        }
    }
    nvs_close(h);
}

bool keymap_find(uint16_t code, keymap_entry_t *out)
{
    bool found = false;
    portENTER_CRITICAL(&lock);
    for (size_t i = 0; i < count; i++) {
        if (map[i].code == code) {
            *out = map[i];
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&lock);
    return found;
}

int keymap_set(const keymap_entry_t *e)
{
    int ret = 0;
    portENTER_CRITICAL(&lock);
    size_t i = 0;
    while (i < count && map[i].code != e->code) {
        i++;
    }
    if (e->type == OUT_NONE) {
        if (i < count) {
            map[i] = map[--count];
        }
    } else if (i < count) {
        map[i] = *e;
    } else if (count < KEYMAP_MAX) {
        map[count++] = *e;
    } else {
        ret = -1;
    }
    portEXIT_CRITICAL(&lock);
    if (ret == 0) {
        save();
    }
    return ret;
}

void keymap_reset(void)
{
    set_defaults();
    save();
}

void keymap_print_entry(const keymap_entry_t *e)
{
    static const char *const NAMES[] = {"none", "key", "consumer", "mouse"};
    if (e->type == OUT_NONE) {
        ESP_LOGI(TAG, "unmap 0x%04x", e->code);
    } else {
        ESP_LOGI(TAG, "map 0x%04x %s 0x%02x 0x%02x", e->code, NAMES[e->type < 4 ? e->type : 0], e->usage, e->mods);
    }
}

void keymap_print(void)
{
    keymap_entry_t copy[KEYMAP_MAX];
    portENTER_CRITICAL(&lock);
    size_t n = count;
    memcpy(copy, map, n * sizeof(map[0]));
    portEXIT_CRITICAL(&lock);

    ESP_LOGI(TAG, "keymap begin (%u entries)", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        keymap_print_entry(&copy[i]);
        usb_console_drain();
    }
    ESP_LOGI(TAG, "keymap end");
}
