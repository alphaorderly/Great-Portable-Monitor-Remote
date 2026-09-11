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
static uint32_t radio_epoch;
static int mac_gap_event(struct ble_gap_event *event, void *arg);
static bool next_directed = true;
static struct ble_npl_callout phase_timer;
static uint32_t timer_generation, phase_started, phase_delay;
static bool timer_armed, termination_accepted, failure_recorded;
static unsigned close_retries;
static ble_addr_t current_peer;
static bool have_current_peer;
static struct { ble_addr_t peer; bool used; unsigned count; } failures[CONFIG_BT_NIMBLE_MAX_BONDS];
static void schedule_advertising(uint32_t delay_ms);
static uint32_t now_ms(void) { return ble_npl_time_ticks_to_ms32(ble_npl_time_get()); }

static void cancel_phase(void)
{
    timer_armed = false;
    ble_npl_callout_stop(&phase_timer);
}
static void phase_after(uint32_t delay_ms)
{
    phase_started = now_ms(); phase_delay = delay_ms;
    timer_generation = link.generation;
    timer_armed = true;
    int rc = ble_npl_callout_reset(&phase_timer, ble_npl_time_ms_to_ticks32(delay_ms));
    if (rc) { timer_armed = false; ESP_LOGE(TAG, "phase timer failed: status=%d", rc); }
}
static void transition(host_link_phase_t phase, const char *reason, int status)
{
    link.phase = phase;
    ESP_LOGI(TAG, "phase=%u generation=%lu handle=%u elapsed_ms=%lu status=%d retries=%u reason=%s",
             (unsigned)phase, (unsigned long)link.generation, link.conn,
             (unsigned long)(now_ms() - link.connected_at), status, close_retries, reason);
}
static void security_result(bool success)
{
    if (!have_current_peer || (!success && failure_recorded)) { return; }
    unsigned slot = CONFIG_BT_NIMBLE_MAX_BONDS;
    for (unsigned i = 0; i < CONFIG_BT_NIMBLE_MAX_BONDS; ++i) {
        if (failures[i].used && bridge_peer_equal(&failures[i].peer, &current_peer)) { slot = i; break; }
        if (!failures[i].used) { slot = i; }
    }
    if (slot == CONFIG_BT_NIMBLE_MAX_BONDS) { slot = 0; }
    if (!failures[slot].used || !bridge_peer_equal(&failures[slot].peer, &current_peer)) {
        failures[slot].peer = current_peer; failures[slot].used = true; failures[slot].count = 0;
    }
    if (success) { failures[slot].count = 0; return; }
    failure_recorded = true;
    if (failures[slot].count < 3 && ++failures[slot].count == 3) {
        ESP_LOGW(TAG, "Repeated host security failure: remove ESP32 in this PC's Bluetooth settings and pair again; bonds retained");
    }
}
static void disconnected(void)
{
    cancel_phase();
    next_directed = false;
    link.conn = BLE_HS_CONN_HANDLE_NONE; link.encrypted = false;
    hid_input_connected(); hid_service_connected();
    have_current_peer = false;
    transition(HOST_LINK_IDLE, "disconnected; open advertising in 1s", 0);
    schedule_advertising(1000);
}
static void try_close(void)
{
    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(link.conn, &desc);
    if (rc == BLE_HS_ENOTCONN) { disconnected(); return; }
    if (!termination_accepted) {
        rc = ble_gap_terminate(link.conn, BLE_ERR_REM_USER_CONN_TERM);
        termination_accepted = rc == 0;
        if (rc == BLE_HS_ENOTCONN && ble_gap_conn_find(link.conn, &desc) == BLE_HS_ENOTCONN) {
            disconnected(); return;
        }
    }
    ESP_LOGI(TAG, "close generation=%lu retry=%u status=%d accepted=%u",
             (unsigned long)link.generation, close_retries, rc, termination_accepted);
    if (close_retries < 3) { phase_after(1000); }
    else { cancel_phase(); transition(HOST_LINK_FAULT, "disconnect recovery failed; no global reset", rc); }
}
static void close_link(const char *reason, int status)
{
    if (link.phase == HOST_LINK_CLOSING || link.phase == HOST_LINK_FAULT) { return; }
    cancel_phase(); link.encrypted = false;
    close_retries = 0; termination_accepted = false;
    security_result(false);
    transition(HOST_LINK_CLOSING, reason, status);
    try_close();
}
static void subscriptions_changed(void)
{
    if (!link.encrypted) { return; }
    uint8_t mask = 0;
    for (unsigned i = 0; i < 3; ++i) { if (native[i].subscribed) { mask |= 1u << i; } }
    host_link_phase_t phase = mask == 7 ? HOST_LINK_READY : mask ? HOST_LINK_PARTIAL : HOST_LINK_WAIT_SUBSCRIPTIONS;
    if (phase != link.phase) { transition(phase, "native subscription changed", 0); }
    ESP_LOGI(TAG, "subscriptions generation=%lu keyboard=%u mouse=%u volume=%u",
             (unsigned long)link.generation, !!(mask & 1), !!(mask & 2), !!(mask & 4));
}
/* Explicit generation and clock make deadline/race behavior testable without radio.
 * A cancelled/reused callout cannot act before the current generation's deadline. */
void host_link_tick(uint32_t generation, uint32_t now)
{
    if (!timer_armed || generation != link.generation || generation != timer_generation ||
        (uint32_t)(now - phase_started) < phase_delay) { return; }
    cancel_phase();
    if (link.phase == HOST_LINK_WAIT_SECURITY) {
        /* Encryption may have completed in the stack just before this queued
         * deadline, with ENC_CHANGE still pending on the host event queue. */
        struct ble_gap_conn_desc desc;
        if (!ble_gap_conn_find(link.conn, &desc) && desc.sec_state.encrypted &&
            desc.sec_state.bonded && bridge_peer_bonded(&desc.peer_id_addr, false)) {
            struct ble_gap_event secured = {.type = BLE_GAP_EVENT_ENC_CHANGE,
                                           .enc_change = {.conn_handle = link.conn}};
            mac_gap_event(&secured, (void *)(uintptr_t)link.generation);
        } else { close_link("security timeout", BLE_HS_EENCRYPT); }
    }
    else if (link.phase == HOST_LINK_CLOSING) { ++close_retries; try_close(); }
    else if (link.encrypted && link.phase != HOST_LINK_READY) {
        ESP_LOGW(TAG, "HID subscription delayed generation=%lu; keep encrypted link and available inputs",
                 (unsigned long)link.generation);
    }
}
static void on_phase_timer(struct ble_npl_event *event)
{
    (void)event;
    host_link_tick(timer_generation, now_ms());
}

static struct ble_npl_callout advertise_retry;
static bool advertising_ready;
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
        link.phase == HOST_LINK_CLOSING || link.phase == HOST_LINK_FAULT || pairing_retried || ble_gap_conn_find(link.conn, &desc) != 0 || desc.sec_state.encrypted) {
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
    cancel_phase();
    transition(HOST_LINK_WAIT_SECURITY, "stale bond removed; retry pairing once", 0);
    phase_after(15000);
    ESP_LOGW(TAG, "Host stale bond removed for current host; retry pairing once");
    return BLE_GAP_REPEAT_PAIRING_RETRY;
}

static int mac_gap_event(struct ble_gap_event *event, void *arg)
{
    /* NimBLE retains the advertising callback argument for the connection.
     * Tokens distinguish reused connection handles, including queued old events. */
    if ((uint32_t)(uintptr_t)arg != radio_epoch) { return 0; }
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) {
            ESP_LOGW(TAG, "Host connection failed: status=%d; retry advertising in 2s", event->connect.status);
            schedule_advertising(2000);
            break;
        }
        if (link.conn != BLE_HS_CONN_HANDLE_NONE) { break; }
        cancel_phase();
        link.generation = radio_epoch;
        link.connected_at = now_ms();
        link.conn = event->connect.conn_handle;
        struct ble_gap_conn_desc connected_desc;
        have_current_peer = ble_gap_conn_find(link.conn, &connected_desc) == 0;
        if (have_current_peer) { current_peer = connected_desc.peer_id_addr; current_peer.type &= 1; }
        failure_recorded = false;
        close_retries = 0;
        link.encrypted = false;
        hid_input_connected();
        hid_service_connected();
        pairing_retried = false;
        ble_npl_callout_stop(&advertise_retry);
        transition(HOST_LINK_WAIT_SECURITY, "connected; waiting for host encryption", 0);
        phase_after(15000);
        /* The host is Central: let it restore encryption / react to READ_ENC.
         * Do not send a simultaneous peripheral SMP Security Request. */
        ESP_LOGI(TAG, "Host security: waiting for host encryption (no duplicate security request)");
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (event->enc_change.conn_handle != link.conn || link.conn == BLE_HS_CONN_HANDLE_NONE ||
            link.phase == HOST_LINK_CLOSING || link.phase == HOST_LINK_FAULT) { break; }
        struct ble_gap_conn_desc desc;
        link.encrypted = event->enc_change.status == 0 && ble_gap_conn_find(link.conn, &desc) == 0 &&
                    desc.sec_state.encrypted && desc.sec_state.bonded &&
                    bridge_peer_bonded(&desc.peer_id_addr, false);
        ESP_LOGI(TAG, "Host security: status=%d (%s) encrypted_and_bonded=%u", event->enc_change.status,
                 event->enc_change.status == 0 ? "OK" :
                 event->enc_change.status == BLE_HS_ENOTCONN ? "BLE_HS_ENOTCONN: link already closed" : "see NimBLE status",
                 link.encrypted);
        if (link.encrypted) {
            cancel_phase();
            current_peer = desc.peer_id_addr; current_peer.type &= 1; have_current_peer = true;
            security_result(true);
            transition(HOST_LINK_WAIT_SUBSCRIPTIONS, "encrypted and bonded", 0);
            phase_after(10000);
            subscriptions_changed();
            ESP_LOGI(TAG, "Host link: handle=%u interval=%u latency=%u timeout=%u",
                     link.conn, desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
            link.identity = desc.peer_id_addr;
            link.identity.type &= 1;
            link.have_identity = true;
            bridge_peer_save("mac", &link.identity);
            hid_service_update_schema();
        } else {
            close_link("encryption/bond restore failed", event->enc_change.status);
        }
        hid_input_publish();
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle != link.conn || link.conn == BLE_HS_CONN_HANDLE_NONE ||
            link.phase == HOST_LINK_CLOSING || link.phase == HOST_LINK_FAULT) { break; }
        hid_input_subscribe(event->subscribe.attr_handle, event->subscribe.cur_notify);
        subscriptions_changed();
        hid_service_update_schema();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle != link.conn || link.conn == BLE_HS_CONN_HANDLE_NONE) { break; }
        ESP_LOGI(TAG, "Host disconnected: reason=%d (0x%X); resume advertising for host connection",
                 event->disconnect.reason, (unsigned)event->disconnect.reason);
        disconnected();
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.conn_handle != link.conn || link.conn == BLE_HS_CONN_HANDLE_NONE) { break; }
        if (event->notify_tx.status && !event->notify_tx.indication) {
            /* NimBLE emits this synchronously for a send attempt, not a radio
             * acknowledgement. Rate-limited diagnostics live in the heartbeat. */
            hid_input_tx_failed(event->notify_tx.attr_handle);
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return repair_mac_pairing(&event->repeat_pairing);
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.conn_handle == link.conn && link.conn != BLE_HS_CONN_HANDLE_NONE) {
            close_link("unexpected passkey request", 0);
        }
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
    next_directed = false;
    const struct ble_gap_adv_params params = {
        .conn_mode = directed ? BLE_GAP_CONN_MODE_DIR : BLE_GAP_CONN_MODE_UND,
        .disc_mode = directed ? BLE_GAP_DISC_MODE_NON : BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x30, .itvl_max = 0x60, /* 30-60ms, low-duty directed mode. */
    };
    int duration = directed ? 3000 : BLE_HS_FOREVER;
    if (++radio_epoch == 0) { ++radio_epoch; }
    rc = ble_gap_adv_start(mac_own_addr_type, directed ? &link.identity : NULL, duration, &params, mac_gap_event, (void *)(uintptr_t)radio_epoch);
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
    int rc = ble_npl_callout_init(&phase_timer, nimble_port_get_dflt_eventq(), on_phase_timer, NULL);
    return rc ? rc : ble_npl_callout_init(&advertise_retry, nimble_port_get_dflt_eventq(), retry_advertising, NULL);
}
void host_link_reset(void)
{
    advertising_ready = false;
    ++radio_epoch;
    cancel_phase();
    link.phase = HOST_LINK_IDLE;
    ble_npl_callout_stop(&advertise_retry);
    link.conn = BLE_HS_CONN_HANDLE_NONE;
    link.encrypted = false;
    hid_input_reset();
}
