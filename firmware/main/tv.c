#include "tv.h"

#include <string.h>
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "remote.h"

static const char *TAG = "tv";

static tv_write_cb_t write_cb;
static tv_fd_subscribed_cb_t fd_sub_cb;
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static bool advertising_wanted;
static bool sub[3];

/* ---- advertising: copy of the real remote (docs/protocol.md) ---- */

static const uint8_t ADV_DATA[] = {
    // flags: LE General Discoverable. (The real remote uses 0x05 = Limited; 0x06 is what both working setups used,
    // 0x05 was never confirmed to work with the fake.)
    0x02, 0x01, 0x06,
    0x17, 0xff, 0xc4, 0x00, 'S', 'C', 'D', ' ', '2', '1', '.', '2', ',', 'B', 'A', ' ', '3', '5', ',', 'w', 'e',
    'b', 'O', 'S',
};
static const uint8_t SCAN_RSP[] = {0x0b, 0x09, 'L', 'G', 'E', ' ', 'M', 'R', '2', '5', 'G', 'B'};

/* ---- GATT values ---- */

static const uint8_t REPORT_MAP[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0xfd, 0x95, 0x1e, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x81,
    0x00, 0xc0, 0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0xf9, 0x95, 0x1e, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff,
    0x00, 0x81, 0x00, 0x95, 0xc8, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0xb1, 0x00, 0xc0, 0x05, 0x0c, 0x09,
    0x01, 0xa1, 0x01, 0x85, 0xfe, 0x95, 0x7c, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x81, 0x00, 0xc0,
};
static const uint8_t HID_INFO[] = {0x00, 0x00, 0x00, 0x01};
static const uint8_t EXT_REPORT_REF[] = {0x19, 0x2a};
static uint8_t protocol_mode = 0x01;
static const uint8_t BATTERY[] = {0x27};
static const uint8_t PNP_ID[] = {0x01, 0x5d, 0x00, 0x62, 0x87, 0x01, 0x00};
static const uint8_t SYSTEM_ID[] = {0x00, 0x01, 0x02, 0x00, 0x00, 0x03, 0x04, 0x05};
static const uint8_t OTA_FFD2[] = {0, 0, 0, 0, 0, 0};  // the remote's BD address here; the TV never reads it
static const uint8_t OTA_FFD3[] = {0x01, 0x00, 0x0c, 0x38};
static const uint8_t OTA_FFD4[] = {0x61, 0xa0, 0x0b, 0x00};
static const uint8_t OTA_FFF1[] = {0x0c, 0x01, 0x00, 0x16, 0xdc, 0x07, 0x5d, 0x00, 0xf0, 0x0f, 0x00, 0x03};
static const uint8_t OTA_FFE0[] = {0x03, 0x00, 0x00, 0x01, 0x11, 0x00, 0x00, 0x00, 0x01, 0x00,
                                   0x0c, 0x38, 0x61, 0xa0, 0x0b, 0x00, 0x01, 0xe0, 0x03, 0x08};

typedef struct {
    const void *data;
    uint16_t len;
} blob_t;
#define BLOB(x) (&(blob_t){(x), sizeof(x)})
#define STR(s) (&(blob_t){(s), sizeof(s) - 1})
#define EMPTY (&(blob_t){NULL, 0})

// Report characteristic context: Report Reference (id, type) + description + where TV writes go on the remote.
typedef struct {
    uint8_t ref[2];
    const char *desc;
    uint16_t remote_handle;  // 0 = not writable
    bool read_error;         // the real remote answers reads with ATT 0x0A
} report_t;

static const report_t R_F9_IN = {{0xf9, 0x01}, "HID_0xF9_IN_USER_SESCR", 0, true};
static const report_t R_F9_FEAT = {{0xf9, 0x03}, "HID_0xF9_FEATURE_USER_SESCR", REMOTE_H_F9_FEAT, true};
static const report_t R_FD_IN = {{0xfd, 0x01}, "HID_0xFD_IN_USER_SESCR", 0, false};
static const report_t R_FE_IN = {{0xfe, 0x01}, "HID_0xFE_IN_USER_SESCR", 0, true};
static const report_t R_F9_OUT = {{0xf9, 0x02}, "HID_0xF9_OUT_USER_SESCR", REMOTE_H_F9_OUT, true};

static uint16_t h_f9_in, h_fd_in, h_fe_in;

static int append(struct ble_gatt_access_ctxt *ctxt, const void *data, uint16_t len)
{
    return os_mbuf_append(ctxt->om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// Plain read-only (or ignored-write) value.
static int blob_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const blob_t *b = arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR || ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        ESP_LOGI(TAG, "TV read handle 0x%04x", attr_handle);
        return append(ctxt, b->data, b->len);
    }
    ESP_LOGI(TAG, "TV wrote %u bytes to handle 0x%04x (ignored)", OS_MBUF_PKTLEN(ctxt->om), attr_handle);
    return 0;
}

static int report_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const report_t *r = arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        ESP_LOGI(TAG, "TV read report %02x/%02x", r->ref[0], r->ref[1]);
        return r->read_error ? BLE_ATT_ERR_ATTR_NOT_FOUND : 0;
    }
    uint8_t buf[256];
    uint16_t len = 0;
    ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
    if (r->remote_handle && write_cb) {
        write_cb(r->remote_handle, buf, len);
    }
    return 0;
}

static int report_dsc_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                             void *arg)
{
    // arg: report_t; the descriptor UUID tells which value to return.
    const report_t *r = arg;
    uint16_t uuid = ble_uuid_u16(ctxt->dsc->uuid);
    if (uuid == 0x2908) {
        return append(ctxt, r->ref, 2);
    }
    return append(ctxt, r->desc, strlen(r->desc));
}

static int protocol_mode_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                                void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return append(ctxt, &protocol_mode, 1);
    }
    ble_hs_mbuf_to_flat(ctxt->om, &protocol_mode, 1, NULL);
    ESP_LOGI(TAG, "TV set protocol mode %u", protocol_mode);
    return 0;
}

static int control_point_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                                void *arg)
{
    uint8_t v = 0;
    ble_hs_mbuf_to_flat(ctxt->om, &v, 1, NULL);
    ESP_LOGI(TAG, "TV HID control point: %u (%s)", v, v ? "exit suspend" : "suspend");
    return 0;
}

#define REPORT_DSCS(r)                                                                                         \
    (struct ble_gatt_dsc_def[])                                                                                 \
    {                                                                                                           \
        {.uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ, .access_cb = report_dsc_access,      \
         .arg = (void *)&(r)},                                                                                  \
            {.uuid = BLE_UUID16_DECLARE(0x2901), .att_flags = BLE_ATT_F_READ, .access_cb = report_dsc_access,  \
             .arg = (void *)&(r)},                                                                              \
            {0},                                                                                                \
    }

#define RO(uuid16, blob) {.uuid = BLE_UUID16_DECLARE(uuid16), .access_cb = blob_access, .arg = (void *)(blob), \
                          .flags = BLE_GATT_CHR_F_READ}

static const ble_uuid128_t UUID_D1FF =
    BLE_UUID128_INIT(0x12, 0xa2, 0x4d, 0x2e, 0xfe, 0x14, 0x48, 0x8e, 0x93, 0xd2, 0x17, 0x3c, 0xff, 0xd1, 0x00, 0x00);
static const ble_uuid128_t UUID_D0FF =
    BLE_UUID128_INIT(0x12, 0xa2, 0x4d, 0x2e, 0xfe, 0x14, 0x48, 0x8e, 0x93, 0xd2, 0x17, 0x3c, 0xff, 0xd0, 0x00, 0x00);

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),  // HID
        .characteristics = (struct ble_gatt_chr_def[]){
            RO(0x2a4a, BLOB(HID_INFO)),
            {.uuid = BLE_UUID16_DECLARE(0x2a4c), .access_cb = control_point_access,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP},
            {.uuid = BLE_UUID16_DECLARE(0x2a4e), .access_cb = protocol_mode_access,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP},
            {.uuid = BLE_UUID16_DECLARE(0x2a4b), .access_cb = blob_access, .arg = (void *)BLOB(REPORT_MAP),
             .flags = BLE_GATT_CHR_F_READ,
             .descriptors = (struct ble_gatt_dsc_def[]){
                 {.uuid = BLE_UUID16_DECLARE(0x2907), .att_flags = BLE_ATT_F_READ, .access_cb = blob_access,
                  .arg = (void *)BLOB(EXT_REPORT_REF)},
                 {0}}},
            {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = report_access, .arg = (void *)&R_F9_IN,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &h_f9_in,
             .descriptors = REPORT_DSCS(R_F9_IN)},
            {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = report_access, .arg = (void *)&R_F9_FEAT,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
             .descriptors = REPORT_DSCS(R_F9_FEAT)},
            {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = report_access, .arg = (void *)&R_FD_IN,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY, .val_handle = &h_fd_in,
             .descriptors = REPORT_DSCS(R_FD_IN)},
            {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = report_access, .arg = (void *)&R_FE_IN,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &h_fe_in,
             .descriptors = REPORT_DSCS(R_FE_IN)},
            {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = report_access, .arg = (void *)&R_F9_OUT,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
             .descriptors = REPORT_DSCS(R_F9_OUT)},
            {0}},
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180f),  // Battery
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = BLE_UUID16_DECLARE(0x2a19), .access_cb = blob_access, .arg = (void *)BLOB(BATTERY),
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY},
            {0}},
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180a),  // Device Information
        .characteristics = (struct ble_gatt_chr_def[]){
            RO(0x2a29, STR("Realtek BT")), RO(0x2a24, STR("Model Nbr 0.9")), RO(0x2a25, STR("RTKBeeSerialNum")),
            RO(0x2a27, STR("RTKBeeHardwareRev")), RO(0x2a26, STR("RTKBeeFirmwareRev")),
            RO(0x2a28, STR("RTKBeeSoftwareRev")), RO(0x2a23, BLOB(SYSTEM_ID)),
            RO(0x2a2a, STR("RTKBeeIEEEDatalist")), RO(0x2a50, BLOB(PNP_ID)),
            {0}},
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1813),  // Scan Parameters
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = BLE_UUID16_DECLARE(0x2a4f), .access_cb = blob_access, .arg = (void *)EMPTY,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP},
            {.uuid = BLE_UUID16_DECLARE(0x2a31), .access_cb = blob_access, .arg = (void *)EMPTY,
             .flags = BLE_GATT_CHR_F_NOTIFY},
            {0}},
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_D1FF.u,  // LG vendor service (the TV never used it)
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = BLE_UUID16_DECLARE(0xa001), .access_cb = blob_access, .arg = (void *)EMPTY,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY},
            {.uuid = BLE_UUID16_DECLARE(0xa002), .access_cb = blob_access, .arg = (void *)EMPTY,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE},
            {0}},
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_D0FF.u,  // Realtek OTA: values copied, writes are ignored and never forwarded
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = BLE_UUID16_DECLARE(0xffd1), .access_cb = blob_access, .arg = (void *)EMPTY,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP},
            RO(0xffd2, BLOB(OTA_FFD2)), RO(0xffd3, BLOB(OTA_FFD3)), RO(0xffd4, BLOB(OTA_FFD4)), RO(0xffd5, EMPTY),
            {.uuid = BLE_UUID16_DECLARE(0xffd8), .access_cb = blob_access, .arg = (void *)EMPTY,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP},
            RO(0xfff1, BLOB(OTA_FFF1)),
            {.uuid = BLE_UUID16_DECLARE(0xfff2), .access_cb = blob_access, .arg = (void *)EMPTY,
             .flags = BLE_GATT_CHR_F_WRITE},
            RO(0xffe0, BLOB(OTA_FFE0)), RO(0xffe1, EMPTY), RO(0xffe2, EMPTY),
            {0}},
    },
    {0},
};

/* ---- GAP ---- */

static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    if (!advertising_wanted || conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) {
        return;
    }
    uint8_t own;
    ble_hs_id_infer_auto(0, &own);
    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x20,  // 20 ms; the real remote advertises every ~35 ms
        .itvl_max = 0x30,
    };
    int rc = ble_gap_adv_start(own, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc) {
        ESP_LOGW(TAG, "advertising failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as LGE MR25GB");
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) {
            advertise();
            return 0;
        }
        conn = event->connect.conn_handle;
        memset(sub, 0, sizeof(sub));
        ble_gap_conn_find(conn, &desc);
        ESP_LOGI(TAG, "TV connected (%02X:%02X:%02X:%02X:%02X:%02X)", desc.peer_id_addr.val[5],
                 desc.peer_id_addr.val[4], desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                 desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
        return 0;

    case BLE_GAP_EVENT_LINK_ESTAB: {
        // Only now is the link usable for SMP: the real remote (and BlueZ as the fake) send a Security Request
        // right away, and the TV answers with a Pairing Request. Sent on CONNECT it was silently dropped.
        int rc = ble_gap_security_initiate(event->link_estab.conn_handle);
        ESP_LOGI(TAG, "TV link established, security request: %d", rc);
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "TV disconnected (reason 0x%x)", event->disconnect.reason);
        conn = BLE_HS_CONN_HANDLE_NONE;
        memset(sub, 0, sizeof(sub));
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "TV link encrypted: status %d", event->enc_change.status);
        // The TV doesn't negotiate the MTU itself. With the default 23 the remote's 125-byte F9 answers
        // (calibration data for the pointer) arrive cut to 20 bytes: buttons work, no cursor.
        if (event->enc_change.status == 0 && ble_att_mtu(conn) < 247) {
            ble_gattc_exchange_mtu(conn, NULL, NULL);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // The TV re-registers us (Home+Back+OK) while we still hold its old bond: drop it and accept.
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        ESP_LOGI(TAG, "TV re-pairing: old bond dropped");
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_SUBSCRIBE: {
        uint16_t h = event->subscribe.attr_handle;
        int i = h == h_f9_in ? TV_F9_IN : h == h_fd_in ? TV_FD_IN : h == h_fe_in ? TV_FE_IN : -1;
        if (i >= 0) {
            sub[i] = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "TV %s %s notifications%s", sub[i] ? "enabled" : "disabled",
                     i == TV_F9_IN ? "F9" : i == TV_FD_IN ? "FD" : "FE",
                     event->subscribe.reason == BLE_GAP_SUBSCRIBE_REASON_RESTORE ? " (restored)" : "");
            if (i == TV_FD_IN && sub[i] && fd_sub_cb) {
                fd_sub_cb();
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "TV link: itvl %u, latency %u, timeout %u", desc.conn_itvl, desc.conn_latency,
                     desc.supervision_timeout);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "TV MTU %u", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGI(TAG, "TV pairing: passkey action %d", event->passkey.params.action);
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            struct ble_sm_io io = {.action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = 1};
            ble_sm_inject_io(event->passkey.conn_handle, &io);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.status && event->notify_tx.status != BLE_HS_EDONE) {
            ESP_LOGW(TAG, "notify to TV failed: %d", event->notify_tx.status);
        }
        return 0;
    }
    ESP_LOGI(TAG, "TV gap event %d", event->type);
    return 0;
}

void tv_init(tv_write_cb_t on_write, tv_fd_subscribed_cb_t on_fd_subscribed)
{
    write_cb = on_write;
    fd_sub_cb = on_fd_subscribed;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("LGE MR25GB");
    ble_svc_gap_device_appearance_set(0x03C1);
    ESP_ERROR_CHECK(ble_gatts_count_cfg(services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(services));
}

void tv_start_advertising(void)
{
    if (advertising_wanted) {
        return;
    }
    advertising_wanted = true;
    ESP_ERROR_CHECK(ble_gap_adv_set_data(ADV_DATA, sizeof(ADV_DATA)));
    ESP_ERROR_CHECK(ble_gap_adv_rsp_set_data(SCAN_RSP, sizeof(SCAN_RSP)));
    advertise();
}

void tv_stop_advertising(void)
{
    advertising_wanted = false;
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
}

void tv_notify(enum tv_report r, const uint8_t *data, uint16_t len)
{
    if (conn == BLE_HS_CONN_HANDLE_NONE || !sub[r]) {
        return;
    }
    uint16_t h = r == TV_F9_IN ? h_f9_in : r == TV_FD_IN ? h_fd_in : h_fe_in;
    if (len + 3 > ble_att_mtu(conn)) {
        ESP_LOGW(TAG, "%u-byte report doesn't fit MTU %u, dropped", len, ble_att_mtu(conn));
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om) {
        ble_gatts_notify_custom(conn, h, om);
    }
}

bool tv_connected(void)
{
    return conn != BLE_HS_CONN_HANDLE_NONE;
}

void tv_status(void)
{
    ESP_LOGI(TAG, "TV conn=%s advertising=%d subscribed F9=%d FD=%d FE=%d mtu=%u",
             conn == BLE_HS_CONN_HANDLE_NONE ? "none" : "up", ble_gap_adv_active(), sub[TV_F9_IN], sub[TV_FD_IN],
             sub[TV_FE_IN], conn == BLE_HS_CONN_HANDLE_NONE ? 0 : ble_att_mtu(conn));
}
