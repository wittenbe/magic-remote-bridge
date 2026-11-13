#include "remote.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "store/config/ble_store_config.h"

static const char *TAG = "remote";

// A freshly paired remote (pairing mode) expects the TV's init sequence on F9 Output; without it, it gives up after
// ~12 s (F9 In "00", "40 00", 0x803F, disconnect). Normally the TV sends it through the relay; this recorded copy is
// only a fallback (see docs/protocol.md).
static const uint8_t HS_62[] = {
    0x62, 0x00, 0x4d, 0x0f, 0x4f, 0x03, 0x00, 0x00, 0x00, 0x5d, 0x1b, 0xf8, 0x00, 0x0c, 0x02, 0x4d, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x0f, 0x4f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0xff,
    0xff, 0x00, 0x00, 0x00, 0x37, 0x00, 0x00, 0x00, 0xa4, 0x83, 0x1c, 0x8b, 0xbd, 0x85, 0x26, 0x23, 0x29, 0x19,
    0x48, 0xcc, 0x68, 0x82, 0x44, 0x22, 0x45, 0x19, 0x34, 0x95, 0x30, 0x4f, 0x7d, 0xf5, 0xd7, 0x5c, 0x40, 0x24,
    0x0b, 0xdc, 0x14, 0x92, 0x49, 0xa6, 0xd4, 0x51, 0x26, 0xa3, 0x8a, 0x94, 0x92, 0x49, 0xa6, 0xd2, 0x51, 0x27,
    0x23, 0x8a, 0x80, 0x20, 0x00, 0x00, 0x32, 0x68,
};
static const struct {
    const uint8_t *data;
    uint16_t len;
} HANDSHAKE[] = {
    {(const uint8_t[]){0x17}, 1}, {(const uint8_t[]){0x13}, 1}, {(const uint8_t[]){0x74}, 1},
    {(const uint8_t[]){0x19}, 1}, {(const uint8_t[]){0x90}, 1}, {HS_62, sizeof(HS_62)},
};

static const uint16_t CCCDS[] = {REMOTE_H_F9_IN + 1, REMOTE_H_FD_IN + 1, REMOTE_H_FE_IN + 1};

static remote_notify_cb_t notify_cb;
static remote_state_cb_t state_cb;
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static bool ready;
static bool fresh_pairing;
static int quick_fails;
static uint16_t mtu = 23;
static bool handshake_pending;
static ble_addr_t bonded_addr;
static bool have_bond;

static int gap_event(struct ble_gap_event *event, void *arg);
static void scan(void);
static void reconnect(void);

static void set_ready(bool r)
{
    if (ready != r) {
        ready = r;
        if (state_cb) {
            state_cb(r);
        }
    }
}

// NimBLE stores the bonds (with the remote and with the TV) but not which one is the remote, so its address is
// kept in NVS when it pairs. have_bond = that address is known and NimBLE still has keys for it.
static void load_bond(void)
{
    have_bond = false;
    nvs_handle_t h;
    size_t len = sizeof(bonded_addr);
    if (nvs_open("mrb", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, "remote_addr", &bonded_addr, &len) == ESP_OK && len == sizeof(bonded_addr)) {
        struct ble_store_key_sec key = {.peer_addr = bonded_addr};
        struct ble_store_value_sec value;
        have_bond = ble_store_read_peer_sec(&key, &value) == 0;
    }
    nvs_close(h);
}

static void store_remote_addr(const ble_addr_t *addr)
{
    nvs_handle_t h;
    if (nvs_open("mrb", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (addr) {
        nvs_set_blob(h, "remote_addr", addr, sizeof(*addr));
    } else {
        nvs_erase_key(h, "remote_addr");
    }
    nvs_commit(h);
    nvs_close(h);
}

static void handshake(void);

// The handshake's config blob is 98 bytes → needs ATT MTU ≥ 101. The remote starts its own MTU exchange
// right after connecting (ours then never completes), so wait for the MTU event instead of a callback.
static esp_timer_handle_t hs_timer;

static void hs_timer_cb(void *arg)
{
    if (handshake_pending && conn != BLE_HS_CONN_HANDLE_NONE) {
        handshake_pending = false;
        ESP_LOGI(TAG, "no TV handshake within 8 s: sending our own");
        handshake();
    }
}

// Fallback only: normally the TV (connected to the fake during registration) sends its own, complete
// handshake including its config blob; ours would make it skip that config write. If the TV hasn't done it
// within 8 s, send ours so the remote doesn't give up (it does after ~12-15 s).
static void maybe_handshake(void)
{
    if (handshake_pending && mtu >= sizeof(HS_62) + 3) {
        if (!hs_timer) {
            const esp_timer_create_args_t a = {.callback = hs_timer_cb, .name = "handshake"};
            esp_timer_create(&a, &hs_timer);
        }
        esp_timer_stop(hs_timer);
        esp_timer_start_once(hs_timer, 8 * 1000 * 1000);
    }
}

void remote_handshake_seen(void)
{
    if (handshake_pending) {
        handshake_pending = false;
        ESP_LOGI(TAG, "TV handshake forwarded; ours not needed");
    }
}

static void handshake(void)
{
    ESP_LOGI(TAG, "handshake with the freshly paired remote");
    for (size_t i = 0; i < sizeof(HANDSHAKE) / sizeof(HANDSHAKE[0]); i++) {
        int rc = ble_gattc_write_no_rsp_flat(conn, REMOTE_H_F9_OUT, HANDSHAKE[i].data, HANDSHAKE[i].len);
        if (rc) {
            ESP_LOGW(TAG, "handshake write %u failed: %d", (unsigned)i, rc);
        }
    }
}

static int on_cccd_written(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr,
                           void *arg)
{
    int i = (int)(intptr_t)arg;
    if (error->status) {
        ESP_LOGW(TAG, "CCCD 0x%04x write failed: %d", CCCDS[i], error->status);
    }
    if (++i < (int)(sizeof(CCCDS) / sizeof(CCCDS[0]))) {
        static const uint8_t on[2] = {0x01, 0x00};
        ble_gattc_write_flat(conn, CCCDS[i], on, 2, on_cccd_written, (void *)(intptr_t)i);
        return 0;
    }
    ESP_LOGI(TAG, "subscribed (F9/FD/FE)");
    if (fresh_pairing) {
        fresh_pairing = false;
        handshake_pending = true;
    }
    maybe_handshake();
    set_ready(true);
    return 0;
}

static bool is_lg_remote(const struct ble_gap_disc_desc *d)
{
    if (d->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND) {
        return false;
    }
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) {
        return false;
    }
    // LG manufacturer data (company 0x00C4) with the "...webOS" string the MR25GB advertises.
    return f.mfg_data_len >= 2 && f.mfg_data[0] == 0xC4 && f.mfg_data[1] == 0x00;
}

static void connect_to(const ble_addr_t *addr, int32_t timeout_ms)
{
    uint8_t own;
    ble_hs_id_infer_auto(0, &own);
    // Start like BlueZ (30-50 ms, latency 0) and let the remote request its preferred 10 ms / latency 4 itself
    // (it sends an L2CAP update request ~1 s in). Asking for 10 ms in the connect request made every connection
    // fail to establish (0x3E). 3 s supervision timeout: Linux' 420 ms default lost links while the radio was busy.
    struct ble_gap_conn_params p = {
        .scan_itvl = 0x0060, .scan_window = 0x0060,
        .itvl_min = 0x18, .itvl_max = 0x28, .latency = 0, .supervision_timeout = 300,
        .min_ce_len = 0, .max_ce_len = 0,
    };
    int rc = ble_gap_connect(own, addr, timeout_ms, &p, gap_event, NULL);
    if (rc) {
        ESP_LOGW(TAG, "connect failed to start: %d", rc);
    }
}

static void scan(void)
{
    uint8_t own;
    ble_hs_id_infer_auto(0, &own);
    struct ble_gap_disc_params dp = {.passive = 1, .itvl = 0x60, .window = 0x60, .filter_duplicates = 0};
    int rc = ble_gap_disc(own, BLE_HS_FOREVER, &dp, gap_event, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "scanning for a remote in pairing mode (Home+Back, then OK pointing at the TV)");
    } else if (rc == BLE_HS_EBUSY) {
        // e.g. a cancelled connect attempt is still winding down: try again shortly
        static esp_timer_handle_t retry;
        if (!retry) {
            const esp_timer_create_args_t a = {.callback = (esp_timer_cb_t)scan, .name = "scan_retry"};
            esp_timer_create(&a, &retry);
        }
        esp_timer_start_once(retry, 200 * 1000);
    } else if (rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "scan failed: %d", rc);
    }
}

// The bonded remote reconnects on a key press after sleeping; keep a connect pending so we catch it.
static void reconnect(void)
{
    ESP_LOGI(TAG, "waiting for the bonded remote to reconnect");
    connect_to(&bonded_addr, BLE_HS_FOREVER);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (is_lg_remote(&event->disc)) {
            ble_addr_t addr = event->disc.addr;  // copy first: cancelling the scan reuses the event buffer
            ESP_LOGI(TAG, "remote in pairing mode seen (RSSI %d), connecting", event->disc.rssi);
            ble_gap_disc_cancel();
            connect_to(&addr, 3000);
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed: 0x%x", event->connect.status);
            have_bond ? reconnect() : scan();
            return 0;
        }
        conn = event->connect.conn_handle;
        ble_gap_conn_find(conn, &desc);
        ESP_LOGI(TAG, "link: itvl %u (x1.25 ms), latency %u, timeout %u (x10 ms)", desc.conn_itvl, desc.conn_latency,
                 desc.supervision_timeout);
        load_bond();
        fresh_pairing = !have_bond;
        ESP_LOGI(TAG, "connected (%s)", fresh_pairing ? "new" : "bonded");
        return 0;

    case BLE_GAP_EVENT_LINK_ESTAB: {
        int rc = ble_gap_security_initiate(event->link_estab.conn_handle);
        ESP_LOGI(TAG, "link established, securing: %d", rc);
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status != 0) {
            ESP_LOGW(TAG, "encryption failed: 0x%x", event->enc_change.status);
            // The remote was reset (Home+Back) and lost our key → "PIN or Key Missing" on every reconnect.
            if (event->enc_change.status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING) && ++quick_fails >= 2) {
                ESP_LOGW(TAG, "remote seems to have dropped our key; forgetting the bond");
                quick_fails = 0;
                ble_gap_conn_find(event->enc_change.conn_handle, &desc);
                ble_store_util_delete_peer(&desc.peer_id_addr);
                store_remote_addr(NULL);
                have_bond = false;
            }
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        quick_fails = 0;
        ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        store_remote_addr(&desc.peer_id_addr);
        load_bond();
        ESP_LOGI(TAG, "encrypted, bonded=%d", have_bond);
        mtu = ble_att_mtu(conn);
        if (mtu < 247) {
            ble_gattc_exchange_mtu(conn, NULL, NULL);  // may be refused if the remote already started one
        }
        {
            static const uint8_t on[2] = {0x01, 0x00};
            ble_gattc_write_flat(conn, CCCDS[0], on, 2, on_cccd_written, (void *)0);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (notify_cb) {
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            uint8_t buf[256];
            if (len > sizeof(buf)) {
                len = sizeof(buf);
            }
            os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
            notify_cb(event->notify_rx.attr_handle, buf, len);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason 0x%x)", event->disconnect.reason);
        if (event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_CONN_ESTABLISHMENT) && !have_bond) {
            // Connection never came up; the remote is most likely still advertising: retry right away.
            conn = BLE_HS_CONN_HANDLE_NONE;
            scan();
            return 0;
        }
        conn = BLE_HS_CONN_HANDLE_NONE;
        mtu = 23;
        handshake_pending = false;
        set_ready(false);
        load_bond();
        have_bond ? reconnect() : scan();
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // The remote wants to pair again (it was reset): drop the old bond and continue.
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        fresh_pairing = true;
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_MTU:
        mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU %u", mtu);
        maybe_handshake();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "link update (status %d): itvl %u, latency %u, timeout %u", event->conn_update.status,
                     desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
        }
        return 0;
    }
    ESP_LOGD(TAG, "gap event %d", event->type);
    return 0;
}

void remote_init(remote_notify_cb_t on_notify, remote_state_cb_t on_state)
{
    notify_cb = on_notify;
    state_cb = on_state;
}

void remote_start(void)
{
    load_bond();
    have_bond ? reconnect() : scan();
}

bool remote_ready(void)
{
    return ready;
}

bool remote_bonded(void)
{
    return have_bond;
}

int remote_write(uint16_t handle, const uint8_t *data, uint16_t len)
{
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return BLE_HS_ENOTCONN;
    }
    return ble_gattc_write_no_rsp_flat(conn, handle, data, len);
}

void remote_forget(void)
{
    // Delete the bond first: cancelling a pending reconnect fires the connect callback right away, and that
    // must already see "no bond" (otherwise it starts a new reconnect to the old address).
    load_bond();
    if (have_bond) {
        ble_store_util_delete_peer(&bonded_addr);
        have_bond = false;
    }
    store_remote_addr(NULL);
    ESP_LOGI(TAG, "bond forgotten");
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);  // → disconnect event → scan
    } else {
        ble_gap_conn_cancel();
        scan();
    }
}

int remote_set_addr(const char *str)
{
    unsigned int b[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return -1;
    }
    ble_addr_t a = {.type = BLE_ADDR_PUBLIC};
    for (int i = 0; i < 6; i++) {
        a.val[5 - i] = b[i];
    }
    store_remote_addr(&a);
    load_bond();
    ESP_LOGI(TAG, "remote address set, bond keys present: %s", have_bond ? "yes" : "no");
    if (have_bond && conn == BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_disc_cancel();
        reconnect();
    }
    return 0;
}

void remote_status(void)
{
    load_bond();
    ESP_LOGI(TAG, "conn=%s ready=%d bond=%s (%02X:%02X:%02X:%02X:%02X:%02X) scanning=%d",
             conn == BLE_HS_CONN_HANDLE_NONE ? "none" : "up", ready, have_bond ? "yes" : "no", bonded_addr.val[5],
             bonded_addr.val[4], bonded_addr.val[3], bonded_addr.val[2], bonded_addr.val[1], bonded_addr.val[0],
             ble_gap_disc_active());
}
