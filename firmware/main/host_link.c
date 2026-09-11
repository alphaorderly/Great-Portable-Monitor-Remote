#include "hid_internal.h"
#include <string.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "bridge_peers.h"
#include "keymap.h"
#include "host/ble_hs_adv.h"
static const char *TAG = "HOST_LINK";
static host_link_context_t link = {.conn = BLE_HS_CONN_HANDLE_NONE};
const host_link_context_t *host_link_state(void) { return &link; }
static uint8_t mac_own_addr_type;
static struct ble_npl_callout advertise_retry;
static bool next_directed = true, advertising_ready;
static bool pairing_retried;
static int start_advertising(void);

static void schedule_advertising(uint32_t delay_ms)
{
    if (!advertising_ready || link.conn != BLE_HS_CONN_HANDLE_NONE) { return; }
    int rc = ble_npl_callout_reset(&advertise_retry, ble_npl_time_ms_to_ticks32(delay_ms));
    if (rc) { ESP_LOGE(TAG, "advertising timer failed: rc=%d", rc); }
}

static void retry_advertising(struct ble_npl_event *event)
{
    (void)event;
    if (advertising_ready && link.conn == BLE_HS_CONN_HANDLE_NONE && !ble_gap_adv_active()) {
        int rc = start_advertising();
        if (rc) { ESP_LOGW(TAG, "advertising failed: rc=%d; retry in 2s", rc); schedule_advertising(2000); }
    }
}

/* This callback belongs only to the peripheral advertising connection.
 * The existing Central callback continues to belong only to the remote. */
static int repair_mac_pairing(const struct ble_gap_repeat_pairing *request)
{
    struct ble_gap_conn_desc desc;
    ble_addr_t remote;
    if (link.conn == BLE_HS_CONN_HANDLE_NONE || request->conn_handle != link.conn ||
        pairing_retried || ble_gap_conn_find(link.conn, &desc) != 0 || desc.sec_state.encrypted) {
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    /* Never replace the remote's Central bond, or downgrade an existing bond. */
    if ((bridge_peer_load("remote", &remote) && bridge_peer_equal(&remote, &desc.peer_id_addr)) ||
        !request->new_bonding || request->new_key_size < request->cur_key_size ||
        (request->cur_authenticated && !request->new_authenticated) ||
        (request->cur_sc && !request->new_sc)) {
        ESP_LOGW(TAG, "Host repeat pairing refused: remote identity or weaker security");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    ble_addr_t peer = desc.peer_id_addr;
    peer.type &= 1;
    pairing_retried = true;
    /* Clear only this host's metadata. No global NVS/bond reset. */
    if (!bridge_peer_forget("hid_v3", &peer) || !bridge_peer_forget("mac", &peer)) {
        ESP_LOGE(TAG, "Host re-pair metadata cleanup failed; bond retained");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    if (link.have_identity && bridge_peer_equal(&link.identity, &peer)) { link.have_identity = false; }
    int rc = ble_store_util_delete_peer(&peer);
    if (rc != 0) {
        ESP_LOGE(TAG, "Host stale bond removal failed: rc=%d; pairing not retried", rc);
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    hid_service_schema_reset();
    ESP_LOGW(TAG, "Host stale bond removed for current host; retry pairing once");
    return BLE_GAP_REPEAT_PAIRING_RETRY;
}

static int mac_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) {
            ESP_LOGW(TAG, "Host connection failed: status=%d; retry advertising in 2s", event->connect.status);
            schedule_advertising(2000);
            break;
        }
        link.conn = event->connect.conn_handle;
        link.encrypted = false;
        hid_input_connected();
        hid_service_connected();
        pairing_retried = false;
        ble_npl_callout_stop(&advertise_retry);
        ESP_LOGI(TAG, "Host connected: handle=%u", link.conn);
        /* The host is Central: let it restore encryption / react to READ_ENC.
         * Do not send a simultaneous peripheral SMP Security Request. */
        ESP_LOGI(TAG, "Host security: waiting for host encryption (no duplicate security request)");
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (event->enc_change.conn_handle != link.conn || link.conn == BLE_HS_CONN_HANDLE_NONE) { break; }
        struct ble_gap_conn_desc desc;
        link.encrypted = event->enc_change.status == 0 && ble_gap_conn_find(link.conn, &desc) == 0 &&
                    desc.sec_state.encrypted && desc.sec_state.bonded &&
                    bridge_peer_bonded(&desc.peer_id_addr, false);
        ESP_LOGI(TAG, "Host security: status=%d (%s) encrypted_and_bonded=%u", event->enc_change.status,
                 event->enc_change.status == 0 ? "OK" :
                 event->enc_change.status == BLE_HS_ENOTCONN ? "BLE_HS_ENOTCONN: link already closed" : "see NimBLE status",
                 link.encrypted);
        if (link.encrypted) {
            ESP_LOGI(TAG, "Host link: handle=%u interval=%u latency=%u timeout=%u",
                     link.conn, desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
            link.identity = desc.peer_id_addr;
            link.identity.type &= 1;
            link.have_identity = true;
            bridge_peer_save("mac", &link.identity);
            hid_service_update_schema();
        } else {
            /* Retain keys on transient errors; re-pair only on the host's request. */
            ESP_LOGW(TAG, "Host encryption/bond restore failed; close link and resume advertising");
            int rc = event->enc_change.status == BLE_HS_ENOTCONN ? 0 :
                     ble_gap_terminate(link.conn, BLE_ERR_REM_USER_CONN_TERM);
            if (rc && rc != BLE_HS_ENOTCONN) { ESP_LOGE(TAG, "Host disconnect request failed: rc=%d", rc); }
        }
        hid_input_publish();
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        hid_input_subscribe(event->subscribe.attr_handle, event->subscribe.cur_notify);
        hid_service_update_schema();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Host disconnected: reason=%d (0x%X); resume advertising for host connection",
                 event->disconnect.reason, (unsigned)event->disconnect.reason);
        link.conn = BLE_HS_CONN_HANDLE_NONE;
        link.encrypted = false;
        hid_input_connected();
        next_directed = true;
        schedule_advertising(1000);
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.status && !event->notify_tx.indication) {
            /* NimBLE emits this synchronously for a send attempt, not a radio
             * acknowledgement. Rate-limited diagnostics live in the heartbeat. */
            hid_input_tx_failed(event->notify_tx.attr_handle);
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return repair_mac_pairing(&event->repeat_pairing);
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGE(TAG, "Unexpected Mac passkey request for NoInputNoOutput");
        ble_gap_terminate(link.conn, BLE_ERR_REM_USER_CONN_TERM);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising window ended: reason=%d; continue reconnect advertising", event->adv_complete.reason);
        schedule_advertising(50);
        break;
    default: break;
    }
    return 0;
}

static void load_mac_identity(void)
{
    link.have_identity = bridge_peer_load("mac", &link.identity) && bridge_peer_bonded(&link.identity, false);
    if (link.have_identity) { return; }
    /* Migrate old firmware: a bonded peer that subscribed to our GUI Input is
     * a host of this peripheral, not the remote we use as Central. */
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = 0, matches = 0;
    if (ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS)) { return; }
    for (int i = 0; i < count; ++i) {
        struct ble_store_key_cccd key = {.peer_addr = peers[i], .chr_val_handle = input_handle};
        struct ble_store_value_cccd value;
        if (!ble_store_read_cccd(&key, &value) && (value.flags & 1) && bridge_peer_bonded(&peers[i], false)) {
            link.identity = peers[i]; ++matches;
        }
    }
    link.have_identity = matches == 1;
    if (link.have_identity) { bridge_peer_save("mac", &link.identity); }
}

static int start_advertising(void)
{
    if (link.conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) { return 0; }
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids16 = &hid_uuid, .num_uuids16 = 1, .uuids16_is_complete = 1,
        .appearance = 0x03c0, .appearance_is_present = 1, /* Generic HID */
    };
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc) { return rc; }
    struct ble_hs_adv_fields response = {
        .name = (const uint8_t *)DEVICE_NAME, .name_len = sizeof(DEVICE_NAME) - 1, .name_is_complete = 1,
    };
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc) { return rc; }
    bool directed = link.have_identity && next_directed;
    next_directed = !directed;
    const struct ble_gap_adv_params params = {
        .conn_mode = directed ? BLE_GAP_CONN_MODE_DIR : BLE_GAP_CONN_MODE_UND,
        .disc_mode = directed ? BLE_GAP_DISC_MODE_NON : BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x30, .itvl_max = 0x60, /* 30-60ms, low-duty directed mode. */
    };
    int duration = link.have_identity ? (directed ? 10000 : 20000) : BLE_HS_FOREVER;
    rc = ble_gap_adv_start(mac_own_addr_type, directed ? &link.identity : NULL, duration, &params, mac_gap_event, NULL);
    if (rc) { return rc; }
    ESP_LOGI(TAG, "Advertising \"%s\": %s duration=%dms", DEVICE_NAME,
             directed ? "directed to bonded host" : "open for host connection", duration);
    return 0;
}

int host_link_advertise(uint8_t own_addr_type)
{
    mac_own_addr_type = own_addr_type;
    advertising_ready = true;
    next_directed = true;
    load_mac_identity();
    int rc = hid_input_start();
    if (rc) { return rc; }
    rc = start_advertising();
    if (rc) { schedule_advertising(2000); }
    return rc;
}


int host_link_init(void)
{
    return ble_npl_callout_init(&advertise_retry, nimble_port_get_dflt_eventq(), retry_advertising, NULL);
}
void host_link_reset(void)
{
    advertising_ready = false;
    ble_npl_callout_stop(&advertise_retry);
    link.conn = BLE_HS_CONN_HANDLE_NONE;
    link.encrypted = false;
    hid_input_reset();
}
