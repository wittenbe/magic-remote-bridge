// Magic Remote Bridge: LG Magic Remote MR25GB ↔ LG TV relay with a PC mode (USB HID).
// Central to the real remote + peripheral to the TV (relay); red long press switches to PC mode (USB HID).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "store/config/ble_store_config.h"
#include "pc.h"
#include "remote.h"
#include "settings.h"
#include "tv.h"
#include "usb.h"

static const char *TAG = "main";

void ble_store_config_init(void);

static uint16_t last_btn;
static uint32_t fd_count;
static uint8_t last_fd[19];

static void log_hex(const char *what, const uint8_t *d, uint16_t len)
{
    char hex[3 * 16 + 4] = "";
    for (int i = 0; i < len && i < 16; i++) {
        sprintf(hex + 3 * i, "%02x ", d[i]);
    }
    ESP_LOGI(TAG, "%s %s%s", what, hex, len > 16 ? "…" : "");
}

static void on_remote_notify(uint16_t handle, const uint8_t *d, uint16_t len)
{
    if (handle == REMOTE_H_FD_IN) {
        pc_on_fd(d, len);  // forwards to the TV in TV mode, USB HID in PC mode
    } else if (pc_mode() == MODE_TV) {
        tv_notify(handle == REMOTE_H_F9_IN ? TV_F9_IN : TV_FE_IN, d, len);
    }

    if (handle == REMOTE_H_FD_IN && len >= 19) {
        memcpy(last_fd, d, sizeof(last_fd));
        fd_count++;
        uint16_t btn = (d[16] << 8) | d[17];
        int8_t wheel = (int8_t)d[18];
        if (btn != last_btn || wheel) {
            ESP_LOGI(TAG, "FD btn=0x%04x wheel=%d (%lu reports)", btn, wheel, (unsigned long)fd_count);
            last_btn = btn;
        }
    } else {
        log_hex(handle == REMOTE_H_F9_IN ? "remote→TV F9" : "remote→TV FE", d, len);
    }
}

// TV writes that arrive while the real remote isn't connected yet (during registration the TV usually finds the
// fake before we have grabbed the remote) are queued and forwarded once it is.
#define QUEUE_LEN 16
static struct {
    uint16_t handle;
    uint16_t len;
    uint8_t data[128];
} queue[QUEUE_LEN];
static int queued;

static void forward(uint16_t remote_handle, const uint8_t *d, uint16_t len)
{
    if (remote_handle == REMOTE_H_F9_OUT && len >= 1 && d[0] == 0x17) {
        remote_handshake_seen();  // 0x17 starts the TV's init sequence
    }
    int rc = remote_write(remote_handle, d, len);
    if (rc) {
        ESP_LOGW(TAG, "forward failed: %d", rc);
    }
}

static void on_remote_state(bool r)
{
    ESP_LOGI(TAG, "remote %s", r ? "READY" : "gone");
    if (r && queued) {
        ESP_LOGI(TAG, "forwarding %d queued TV writes", queued);
        for (int i = 0; i < queued; i++) {
            forward(queue[i].handle, queue[i].data, queue[i].len);
        }
        queued = 0;
    }
}

static void on_tv_write(uint16_t remote_handle, const uint8_t *d, uint16_t len)
{
    log_hex(remote_handle == REMOTE_H_F9_OUT ? "TV→remote F9out" : "TV→remote F9feat", d, len);
    if (pc_mode() == MODE_PC) {
        ESP_LOGI(TAG, "  blocked (PC mode)");  // e.g. the TV's "02" would put the remote to sleep
        return;
    }
    if (remote_handle == REMOTE_H_F9_OUT && len == 1 && d[0] == 0x02 && !pc_tv_sleep_request()) {
        return;  // sleep command held back (tvsleep setting)
    }
    if (remote_ready()) {
        forward(remote_handle, d, len);
    } else if (queued < QUEUE_LEN && len <= sizeof(queue[0].data)) {
        queue[queued].handle = remote_handle;
        queue[queued].len = len;
        memcpy(queue[queued].data, d, len);
        queued++;
    }
}

// The remote sends its connect event 0x803E when its central subscribes; ours went to us long before the TV
// subscribed, so replay it on top of the last real report.
static void on_tv_fd_subscribed(void)
{
    uint8_t r[19];
    memcpy(r, last_fd, sizeof(r));
    r[16] = 0x80;
    r[17] = 0x3e;
    r[18] = 0;
    tv_notify(TV_FD_IN, r, sizeof(r));
    ESP_LOGI(TAG, "sent connect event 0x803E to the TV");
}

static void pc_to_tv(const uint8_t *r, uint16_t len)
{
    tv_notify(TV_FD_IN, r, len);
}

static void pc_to_remote(const uint8_t *d, uint16_t len)
{
    remote_write(REMOTE_H_F9_OUT, d, len);
}

static void on_line(const char *line)
{
    int a = 0, b = 0, c = 0;
    float f = 0;
    char name[24];
    if (!strcmp(line, "dl")) {
        usb_reboot_to_download();
    } else if (!strcmp(line, "mode")) {
        pc_toggle();
    } else if (sscanf(line, "setremote %23s", name) == 1) {
        if (remote_set_addr(name)) {
            ESP_LOGW(TAG, "usage: setremote AA:BB:CC:DD:EE:FF");
        }
    } else if (sscanf(line, "set %15s %f", name, &f) == 2) {
        if (settings_set(name, f)) {
            ESP_LOGW(TAG, "unknown setting %s (gain | accel | tvsleep | pcsleep | smooth | dead)", name);
        }
    } else if (!strcmp(line, "settings")) {
        settings_print();
    } else if (!strcmp(line, "status")) {
        remote_status();
        tv_status();
        pc_status();
        settings_print();
        ESP_LOGI(TAG, "hid mounted: %d, free heap: %u", usb_hid_mounted(), (unsigned)esp_get_free_heap_size());
    } else if (!strcmp(line, "timing")) {
        pc_toggle_timing_log();
    } else if (!strcmp(line, "forget")) {
        remote_forget();
    } else if (sscanf(line, "key %i", &a) == 1) {
        usb_key(a, true);
        vTaskDelay(pdMS_TO_TICKS(30));
        usb_key(a, false);
    } else if (sscanf(line, "link %i %i %i", &a, &b, &c) == 3) {
        remote_update_link(a, b, c);
    } else if (sscanf(line, "move %i %i %i", &a, &b, &c) >= 2) {
        usb_mouse_move(a, b, c);
    } else {
        ESP_LOGW(TAG, "commands: status | settings | set <gain|accel|tvsleep|pcsleep|smooth|dead> <value> | mode | timing | forget | "
                      "setremote <addr> | link <itvl_min> <itvl_max> <latency> | dl | key <usage> | move <dx> <dy> [wheel]");
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    uint8_t addr[6];
    ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr, NULL);
    ESP_LOGI(TAG, "BLE up, address %02X:%02X:%02X:%02X:%02X:%02X", addr[5], addr[4], addr[3], addr[2], addr[1],
             addr[0]);
    remote_start();
    // Advertise whenever the TV isn't connected, also during registration (Home+Back+OK): the TV then finds the
    // fake instead of competing with us for the real remote, which we grab meanwhile.
    tv_start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset: %d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    usb_init(on_line);
    vTaskDelay(pdMS_TO_TICKS(1500));  // give the host time to open the console
    ESP_LOGI(TAG, "Magic Remote Bridge, phase 3 (relay)");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    settings_load();
    settings_print();
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // Just Works with bonding. The remote only does Legacy pairing; the TV prefers SC.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();

    static const pc_io_t pc_io = {.to_tv = pc_to_tv, .to_remote = pc_to_remote};
    pc_init(&pc_io);
    remote_init(on_remote_notify, on_remote_state);
    tv_init(on_tv_write, on_tv_fd_subscribed);
    nimble_port_freertos_init(host_task);
}
