#include <string.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "mac_hid.h"
#include "input_codec.h"
#include "bridge_peers.h"
#include "host/ble_store.h"
#include "keymap.h"

#define DEVICE_NAME "ESP32 Remote Bridge"
#define REPORT_LENGTH 12
#define REPORT_ID 1
#define FLAG_REMOTE_READY 1
#define FLAG_INPUT_EVENT 2

static const char *TAG = "MAC_HID";
/* Vendor-defined HID collection for the GUI. It does not generate OS key events.
 * Payload: version, flags, sequence LE16, original 8-byte remote keyboard report.
 * The Report ID is declared here / in 2908; it is NOT included in GATT payloads.
 */
static const uint8_t report_map[] = {
    0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x85, REPORT_ID,
    0x09, 0x02, 0x15, 0x00, 0x26, 0xff, 0x00,
    0x75, 0x08, 0x95, REPORT_LENGTH, 0x81, 0x02,
    /* ID 5: mapping configuration Feature in the same vendor collection. */
    0x85, KEYMAP_REPORT_ID, 0x09, 0x03, 0x75, 0x08, 0x95, KEYMAP_LENGTH, 0xb1, 0x02, 0xc0,
    /* ID 2: keyboard, eight modifier bits, reserved byte, six key usages. */
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x02,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
    0x19, 0x00, 0x29, 0x73, 0x15, 0x00, 0x25, 0x73,
    0x75, 0x08, 0x95, 0x06, 0x81, 0x00, 0xc0,
    /* ID 3: three buttons and signed relative X, Y, wheel. */
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x03, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x03, 0x81, 0x02, 0x75, 0x05, 0x95, 0x01, 0x81, 0x01, 0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08,
    0x95, 0x03, 0x81, 0x06, 0xc0, 0xc0,
    /* ID 4: Consumer Volume Increment / Decrement, six padding bits. */
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x04,
    0x09, 0xe9, 0x09, 0xea, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x75, 0x06, 0x95, 0x01, 0x81, 0x01, 0xc0,
};
static uint8_t report[REPORT_LENGTH] = {1};
static uint16_t input_handle;
static uint16_t mac_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t sequence;
static uint8_t mac_own_addr_type;
static bool subscribed, encrypted, suspended, remote_ready;
static bool cursor_mode, mode_known;
static bool home_held;
static int motion_remainder[2];
static uint8_t remote_keys[8], remote_mouse_buttons;
static struct ble_npl_callout heartbeat;
static struct ble_npl_callout mouse_timer;
#define MOUSE_PERIOD_MS 20
#define MOUSE_QUEUE_SIZE 8
/* Bound all application notifications, leaving at least four blocks even if
 * every used block belongs to the smaller MSYS pool. Counts include both pools. */
#define HID_MBUF_MAX_USED (CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT - 4)
_Static_assert(HID_MBUF_MAX_USED > 0, "MSYS pool needs room for HID and receive traffic");
static unsigned tx_pressure, tx_errors, mouse_dropped;
typedef struct { uint8_t buttons; int16_t axis[3]; } mouse_pending_t;
static mouse_pending_t mouse_queue[MOUSE_QUEUE_SIZE];
static unsigned mouse_count;
static struct ble_npl_callout advertise_retry;
static ble_addr_t mac_identity;
static bool have_mac_identity, next_directed = true, advertising_ready;
static bool schema_checked, map_read;
static bool pairing_retried;
static int start_advertising(void);

static void schedule_advertising(uint32_t delay_ms)
{
    if (!advertising_ready || mac_conn != BLE_HS_CONN_HANDLE_NONE) { return; }
    int rc = ble_npl_callout_reset(&advertise_retry, ble_npl_time_ms_to_ticks32(delay_ms));
    if (rc) { ESP_LOGE(TAG, "advertising timer failed: rc=%d", rc); }
}

static void retry_advertising(struct ble_npl_event *event)
{
    (void)event;
    if (advertising_ready && mac_conn == BLE_HS_CONN_HANDLE_NONE && !ble_gap_adv_active()) {
        int rc = start_advertising();
        if (rc) { ESP_LOGW(TAG, "advertising failed: rc=%d; retry in 2s", rc); schedule_advertising(2000); }
    }
}

typedef struct {
    uint8_t id, length, data[8];
    uint16_t handle;
    bool subscribed, dirty;
} native_report_t;
static native_report_t native[] = {
    {.id = 2, .length = 8}, {.id = 3, .length = 4}, {.id = 4, .length = 1},
};

static void update_schema(void)
{
    if (!encrypted || !have_mac_identity) { return; }
    if (map_read && native[0].subscribed && native[1].subscribed && native[2].subscribed) {
        bridge_peer_save("hid_v3", &mac_identity);
        schema_checked = true;
    } else if (!schema_checked) {
        ble_addr_t previous;
        if (!bridge_peer_load("hid_v3", &previous) || !bridge_peer_equal(&previous, &mac_identity)) {
            ESP_LOGI(TAG, "HID schema migration: send Service Changed once for this connection");
            ble_svc_gatt_changed(1, 0xffff);
        }
        schema_checked = true;
    }
}

static bool tx_has_room(void)
{
    if (os_msys_count() - os_msys_num_free() >= HID_MBUF_MAX_USED) {
        ++tx_pressure;
        return false;
    }
    return true;
}

static bool notify_native(native_report_t *r, const uint8_t *data)
{
    if (mac_conn == BLE_HS_CONN_HANDLE_NONE || !r->subscribed || !encrypted || suspended) { return false; }
    if (!tx_has_room()) { return false; }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, r->length);
    if (!om) { ++tx_errors; return false; }
    int rc = ble_gatts_notify_custom(mac_conn, r->handle, om);
    if (rc) { ++tx_errors; }
    return rc == 0;
}

static void flush_native(void)
{
    for (unsigned i = 0; i < 3; ++i) {
        native_report_t *r = &native[i];
        if (i == 1 && mouse_count) { continue; } /* Preserve queued click order. */
        if (r->dirty && notify_native(r, r->data)) { r->dirty = false; }
    }
}

static void clear_native(void)
{
    mouse_count = 0;
    memset(motion_remainder, 0, sizeof(motion_remainder));
    for (unsigned i = 0; i < 3; ++i) {
        memset(native[i].data, 0, sizeof(native[i].data));
        native[i].dirty = true;
    }
}

static void update_native(native_report_t *r, const uint8_t *data)
{
    if (memcmp(r->data, data, r->length)) {
        memcpy(r->data, data, r->length);
        r->dirty = true;
    }
}

static void publish(bool input_event)
{
    report[1] = (remote_ready ? FLAG_REMOTE_READY : 0) | (input_event ? FLAG_INPUT_EVENT : 0);
    if (mac_conn == BLE_HS_CONN_HANDLE_NONE || !subscribed || !encrypted || suspended) { return; }
    if (!tx_has_room()) { return; }
    ++sequence;
    report[2] = sequence & 0xff;
    report[3] = sequence >> 8;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (!om) { ++tx_errors; return; }
    int rc = ble_gatts_notify_custom(mac_conn, input_handle, om);
    /* notify_custom always consumes om, including failure. Heartbeats carry the
     * latest state if a send fails; sequence gaps remain visible in the GUI. */
    if (rc) { ++tx_errors; }
}

static void on_heartbeat(struct ble_npl_event *event)
{
    (void)event;
    if (tx_pressure || tx_errors || mouse_dropped) {
        ESP_LOGW(TAG, "HID flow: deferred=%u failures=%u mouse_dropped=%u mbuf_free=%d/%d",
                 tx_pressure, tx_errors, mouse_dropped, os_msys_num_free(), os_msys_count());
        tx_pressure = tx_errors = mouse_dropped = 0;
    }
    publish(false);
    flush_native(); /* Retry latest held/released state, never relative movement. */
    int rc = ble_npl_callout_reset(&heartbeat, ble_npl_time_ms_to_ticks32(1000));
    if (rc) { ESP_LOGE(TAG, "heartbeat scheduling failed: rc=%d", rc); }
}

void mac_hid_remote_ready(bool ready)
{
    remote_ready = ready;
    if (!ready) {
        cursor_mode = mode_known = false;
        home_held = false;
        memset(remote_keys, 0, sizeof(remote_keys)); remote_mouse_buttons = 0;
        memset(report + 4, 0, 8); clear_native(); flush_native();
    }
    publish(false);
}

static void set_mapping_mode(bool cursor)
{
    if (mode_known && cursor_mode == cursor) { return; }
    cursor_mode = cursor; mode_known = true;
    memset(remote_keys, 0, sizeof(remote_keys)); remote_mouse_buttons = 0;
    clear_native(); flush_native();
    ESP_LOGI(TAG, "Mapping mode=%s (sensor state)", cursor ? "cursor" : "normal");
}

void mac_hid_sensor_mode(bool cursor)
{
    /* In the live capture LEFT in cursor mode is STOP, LEFT, START, LEFT+6A.
     * STOP alone describes a sensor pause, not a user mode change. A real
     * cursor-off operation is preceded by the dedicated, standalone 6A key. */
    bool cursor_button = false, other_button = false;
    for (unsigned i = 2; i < 8; ++i) {
        cursor_button |= remote_keys[i] == 0x6a;
        other_button |= remote_keys[i] != 0 && remote_keys[i] != 0x6a;
    }
    if (cursor || (cursor_button && !other_button) || !mode_known) {
        set_mapping_mode(cursor);
    }
}

static void map_remote(const uint8_t *motion)
{
    uint8_t mapped[8], consumer, mouse[4] = {0};
    input_map(remote_keys, cursor_mode ? 1 : 0, remote_mouse_buttons, mapped, &consumer, &mouse[0]);
    if (motion && !(cursor_mode && home_held)) { memcpy(mouse + 1, motion + 1, 3); }
    update_native(&native[0], mapped);
    update_native(&native[2], &consumer);
    mac_hid_mouse(mouse);
    flush_native();
}

void mac_hid_keyboard(const uint8_t keyboard[8])
{
    remote_ready = true;
    memcpy(report + 4, keyboard, 8);
    publish(true);
    memcpy(remote_keys, keyboard, sizeof(remote_keys));
    home_held = false;
    for (unsigned i = 2; i < 8; ++i) { home_held |= keyboard[i] == 0x4a; }
    if (cursor_mode && home_held) {
        /* Remove unsent movement too, while preserving queued click transitions.
         * Never replay movement or fractional deltas after HOME is released. */
        for (unsigned i = 0; i < mouse_count; ++i) {
            memset(mouse_queue[i].axis, 0, sizeof(mouse_queue[i].axis));
        }
        memset(motion_remainder, 0, sizeof(motion_remainder));
    }
    map_remote(NULL);
}

void mac_hid_remote_mouse(const uint8_t mouse[4])
{
    /* Mouse traffic recovers an unknown mode after bridge reconnect. Once an
     * explicit stop was seen, trailing sensor packets cannot re-enable it. */
    if (!mode_known) { set_mapping_mode(true); }
    if (!cursor_mode) { return; }
    remote_mouse_buttons = mouse[0];
    map_remote(mouse);
}

/* A timer on the NimBLE event queue sends at most one motion report per tick.
 * Button transitions get separate bounded entries; motion with the same buttons
 * is combined. Never allocate an mbuf from the remote's notification callback. */
void mac_hid_mouse(const uint8_t mouse[4])
{
    native_report_t *r = &native[1];
    bool changed = r->data[0] != mouse[0];
    r->data[0] = mouse[0];
    if (mac_conn == BLE_HS_CONN_HANDLE_NONE || !r->subscribed || !encrypted || suspended) {
        mouse_count = 0;
        memset(motion_remainder, 0, sizeof(motion_remainder));
        r->dirty |= changed;
        return;
    }
    if (!changed && !mouse[1] && !mouse[2] && !mouse[3]) { return; }
    if (mouse_count == MOUSE_QUEUE_SIZE && mouse_queue[mouse_count - 1].buttons != mouse[0]) {
        /* An overloaded link must converge to the current buttons, especially
         * release, rather than replay old clicks after recovery. */
        mouse_count = 0;
        ++mouse_dropped;
    }
    if (!mouse_count || mouse_queue[mouse_count - 1].buttons != mouse[0]) {
        mouse_queue[mouse_count++] = (mouse_pending_t){.buttons = mouse[0]};
    }
    mouse_pending_t *p = &mouse_queue[mouse_count - 1];
    for (unsigned i = 0; i < 3; ++i) {
        int delta = (int8_t)mouse[i + 1];
        int limit = 254;
        if (i < 2) {
            int scaled = delta * keymap_mouse_speed() + motion_remainder[i];
            delta = scaled / KEYMAP_SPEED_DEFAULT;
            motion_remainder[i] = scaled % KEYMAP_SPEED_DEFAULT;
            /* Two maximum source deltas, split into valid HID reports by the timer. */
            int scaled_limit = 254 * keymap_mouse_speed() / KEYMAP_SPEED_DEFAULT;
            if (scaled_limit > limit) { limit = scaled_limit; }
        }
        int value = p->axis[i] + delta;
        if (value > limit) { value = limit; ++mouse_dropped; }
        if (value < -limit) { value = -limit; ++mouse_dropped; }
        p->axis[i] = value;
    }
    r->dirty = true;
}

static void on_mouse_tick(struct ble_npl_event *event)
{
    (void)event;
    native_report_t *r = &native[1];
    if (mouse_count) {
        mouse_pending_t *p = &mouse_queue[0];
        uint8_t packet[4] = {p->buttons};
        for (unsigned i = 0; i < 3; ++i) {
            int value = p->axis[i];
            packet[i + 1] = (uint8_t)(int8_t)(value > 127 ? 127 : value < -127 ? -127 : value);
        }
        if (notify_native(r, packet)) {
            for (unsigned i = 0; i < 3; ++i) { p->axis[i] -= (int8_t)packet[i + 1]; }
            if (!p->axis[0] && !p->axis[1] && !p->axis[2]) {
                --mouse_count;
                memmove(mouse_queue, mouse_queue + 1, mouse_count * sizeof(*mouse_queue));
            }
            r->dirty = mouse_count != 0;
        } else {
            /* Failed movement is discarded. Retry only the latest buttons. */
            mouse_count = 0;
            r->dirty = true;
            ++mouse_dropped;
        }
    } else if (r->dirty && notify_native(r, r->data)) {
        r->dirty = false;
    }
    ble_npl_callout_reset(&mouse_timer, ble_npl_time_ms_to_ticks32(MOUSE_PERIOD_MS));
}

static int append(struct os_mbuf *om, const void *data, unsigned len)
{
    return os_mbuf_append(om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_hid(uint16_t conn, uint16_t handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)handle;
    native_report_t *r = arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        const uint8_t reference[] = {r ? r->id : REPORT_ID, 1};
        return append(ctxt->om, reference, sizeof(reference));
    }
    const uint16_t uuid = ble_uuid_u16(ctxt->chr->uuid);
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        switch (uuid) {
        case 0x2a4b: {
            int rc = append(ctxt->om, report_map, sizeof(report_map));
            if (!rc && conn == mac_conn) { map_read = true; update_schema(); }
            return rc;
        }
        case 0x2a4a: {
            const uint8_t info[] = {0x11, 0x01, 0x00, 0x02}; /* HID 1.11, normally connectable */
            return append(ctxt->om, info, sizeof(info));
        }
        case 0x2a4d: return r ? append(ctxt->om, r->data, r->length) : append(ctxt->om, report, sizeof(report));
        case 0x2a29: return append(ctxt->om, "Local Remote Bridge", 19);
        case 0x2a24: return append(ctxt->om, DEVICE_NAME, sizeof(DEVICE_NAME) - 1);
        default: return BLE_ATT_ERR_UNLIKELY;
        }
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && uuid == 0x2a4c) {
        uint8_t value;
        if (OS_MBUF_PKTLEN(ctxt->om) != 1 || os_mbuf_copydata(ctxt->om, 0, 1, &value) || value > 1) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        suspended = value == 0;
        if (suspended) { mouse_count = 0; }
        ESP_LOGI(TAG, "host suspend=%u", suspended);
        if (!suspended) { flush_native(); }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int access_keymap(uint16_t conn, uint16_t handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        const uint8_t reference[] = {KEYMAP_REPORT_ID, 3}; /* Feature */
        return append(ctxt->om, reference, sizeof(reference));
    }
    if (conn != mac_conn || !encrypted) { return BLE_ATT_ERR_INSUFFICIENT_AUTHEN; }
    uint8_t payload[KEYMAP_LENGTH];
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        keymap_read(payload);
        return append(ctxt->om, payload, sizeof(payload));
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (OS_MBUF_PKTLEN(ctxt->om) != sizeof(payload) || os_mbuf_copydata(ctxt->om, 0, sizeof(payload), payload)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        int rc = keymap_apply(payload, sizeof(payload));
        if (rc) {
            ESP_LOGW(TAG, "keymap rejected: reason=%d (invalid=1 conflict=2 storage=3)", rc);
            return BLE_ATT_ERR_UNLIKELY;
        }
        clear_native(); flush_native(); /* Release the old keys before new mapping takes effect. */
        memset(remote_keys, 0, sizeof(remote_keys)); remote_mouse_buttons = 0;
        ESP_LOGI(TAG, "keymap saved: %u buttons; read Feature ID 5 to verify", KEYMAP_COUNT);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

#define NATIVE_CHARACTERISTIC(index) \
    {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = access_hid, .arg = &native[index], \
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY, \
     .val_handle = &native[index].handle, \
     .descriptors = (struct ble_gatt_dsc_def[]) { \
         {.uuid = BLE_UUID16_DECLARE(0x2908), .access_cb = access_hid, .arg = &native[index], \
          .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC}, {0}}}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {.uuid = BLE_UUID16_DECLARE(0x2a4a), .access_cb = access_hid,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
            {.uuid = BLE_UUID16_DECLARE(0x2a4b), .access_cb = access_hid,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
            {.uuid = BLE_UUID16_DECLARE(0x2a4c), .access_cb = access_hid,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC},
            {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = access_hid,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
             .val_handle = &input_handle,
             .descriptors = (struct ble_gatt_dsc_def[]) {
                 {.uuid = BLE_UUID16_DECLARE(0x2908), .access_cb = access_hid,
                 .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC}, {0},
             }},
            NATIVE_CHARACTERISTIC(0),
            NATIVE_CHARACTERISTIC(1),
            NATIVE_CHARACTERISTIC(2),
            {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = access_keymap,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
             .descriptors = (struct ble_gatt_dsc_def[]) {
                 {.uuid = BLE_UUID16_DECLARE(0x2908), .access_cb = access_keymap,
                  .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC}, {0}}},
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180a),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {.uuid = BLE_UUID16_DECLARE(0x2a29), .access_cb = access_hid, .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2a24), .access_cb = access_hid, .flags = BLE_GATT_CHR_F_READ},
            {0},
        },
    },
    {0},
};

/* This callback belongs only to the peripheral advertising connection.
 * The existing Central callback continues to belong only to the remote. */
static int repair_mac_pairing(const struct ble_gap_repeat_pairing *request)
{
    struct ble_gap_conn_desc desc;
    ble_addr_t remote;
    if (mac_conn == BLE_HS_CONN_HANDLE_NONE || request->conn_handle != mac_conn ||
        pairing_retried || ble_gap_conn_find(mac_conn, &desc) != 0 || desc.sec_state.encrypted) {
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
    if (have_mac_identity && bridge_peer_equal(&mac_identity, &peer)) { have_mac_identity = false; }
    int rc = ble_store_util_delete_peer(&peer);
    if (rc != 0) {
        ESP_LOGE(TAG, "Host stale bond removal failed: rc=%d; pairing not retried", rc);
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    schema_checked = false;
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
        mac_conn = event->connect.conn_handle;
        subscribed = encrypted = suspended = false;
        clear_native();
        for (unsigned i = 0; i < 3; ++i) { native[i].subscribed = false; }
        sequence = 0;
        schema_checked = false;
        map_read = false;
        pairing_retried = false;
        ble_npl_callout_stop(&advertise_retry);
        ESP_LOGI(TAG, "Host connected: handle=%u", mac_conn);
        /* The host is Central: let it restore encryption / react to READ_ENC.
         * Do not send a simultaneous peripheral SMP Security Request. */
        ESP_LOGI(TAG, "Host security: waiting for host encryption (no duplicate security request)");
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (event->enc_change.conn_handle != mac_conn || mac_conn == BLE_HS_CONN_HANDLE_NONE) { break; }
        struct ble_gap_conn_desc desc;
        encrypted = event->enc_change.status == 0 && ble_gap_conn_find(mac_conn, &desc) == 0 &&
                    desc.sec_state.encrypted && desc.sec_state.bonded &&
                    bridge_peer_bonded(&desc.peer_id_addr, false);
        ESP_LOGI(TAG, "Host security: status=%d (%s) encrypted_and_bonded=%u", event->enc_change.status,
                 event->enc_change.status == 0 ? "OK" :
                 event->enc_change.status == BLE_HS_ENOTCONN ? "BLE_HS_ENOTCONN: link already closed" : "see NimBLE status",
                 encrypted);
        if (encrypted) {
            ESP_LOGI(TAG, "Host link: handle=%u interval=%u latency=%u timeout=%u",
                     mac_conn, desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
            mac_identity = desc.peer_id_addr;
            mac_identity.type &= 1;
            have_mac_identity = true;
            bridge_peer_save("mac", &mac_identity);
            update_schema();
        } else {
            /* Retain keys on transient errors; re-pair only on the host's request. */
            ESP_LOGW(TAG, "Host encryption/bond restore failed; close link and resume advertising");
            int rc = event->enc_change.status == BLE_HS_ENOTCONN ? 0 :
                     ble_gap_terminate(mac_conn, BLE_ERR_REM_USER_CONN_TERM);
            if (rc && rc != BLE_HS_ENOTCONN) { ESP_LOGE(TAG, "Host disconnect request failed: rc=%d", rc); }
        }
        publish(false);
        flush_native();
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == input_handle) {
            subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "Host Input subscription=%u", subscribed);
            publish(false);
        }
        for (unsigned i = 0; i < 3; ++i) {
            if (event->subscribe.attr_handle == native[i].handle) {
                native[i].subscribed = event->subscribe.cur_notify;
                native[i].dirty = true;
                ESP_LOGI(TAG, "Host Input id=%u subscription=%u", native[i].id, native[i].subscribed);
            }
        }
        update_schema();
        flush_native();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Host disconnected: reason=%d (0x%X); resume advertising for host connection",
                 event->disconnect.reason, (unsigned)event->disconnect.reason);
        mac_conn = BLE_HS_CONN_HANDLE_NONE;
        subscribed = encrypted = suspended = false;
        clear_native();
        for (unsigned i = 0; i < 3; ++i) { native[i].subscribed = false; }
        next_directed = true;
        schedule_advertising(1000);
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.status && !event->notify_tx.indication) {
            /* NimBLE emits this synchronously for a send attempt, not a radio
             * acknowledgement. Rate-limited diagnostics live in the heartbeat. */
            for (unsigned i = 0; i < 3; ++i) {
                if (event->notify_tx.attr_handle == native[i].handle) { native[i].dirty = true; }
            }
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return repair_mac_pairing(&event->repeat_pairing);
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGE(TAG, "Unexpected Mac passkey request for NoInputNoOutput");
        ble_gap_terminate(mac_conn, BLE_ERR_REM_USER_CONN_TERM);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising window ended: reason=%d; continue reconnect advertising", event->adv_complete.reason);
        schedule_advertising(50);
        break;
    default: break;
    }
    return 0;
}

int mac_hid_init(void)
{
    keymap_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc) { return rc; }
    rc = ble_gatts_count_cfg(services);
    if (rc) { return rc; }
    rc = ble_gatts_add_svcs(services);
    if (rc) { return rc; }
    rc = ble_npl_callout_init(&heartbeat, nimble_port_get_dflt_eventq(), on_heartbeat, NULL);
    if (rc) { return rc; }
    rc = ble_npl_callout_init(&mouse_timer, nimble_port_get_dflt_eventq(), on_mouse_tick, NULL);
    if (rc) { return rc; }
    return ble_npl_callout_init(&advertise_retry, nimble_port_get_dflt_eventq(), retry_advertising, NULL);
}

static void load_mac_identity(void)
{
    have_mac_identity = bridge_peer_load("mac", &mac_identity) && bridge_peer_bonded(&mac_identity, false);
    if (have_mac_identity) { return; }
    /* Migrate old firmware: a bonded peer that subscribed to our GUI Input is
     * a host of this peripheral, not the remote we use as Central. */
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = 0, matches = 0;
    if (ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS)) { return; }
    for (int i = 0; i < count; ++i) {
        struct ble_store_key_cccd key = {.peer_addr = peers[i], .chr_val_handle = input_handle};
        struct ble_store_value_cccd value;
        if (!ble_store_read_cccd(&key, &value) && (value.flags & 1) && bridge_peer_bonded(&peers[i], false)) {
            mac_identity = peers[i]; ++matches;
        }
    }
    have_mac_identity = matches == 1;
    if (have_mac_identity) { bridge_peer_save("mac", &mac_identity); }
}

static int start_advertising(void)
{
    if (mac_conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) { return 0; }
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
    bool directed = have_mac_identity && next_directed;
    next_directed = !directed;
    const struct ble_gap_adv_params params = {
        .conn_mode = directed ? BLE_GAP_CONN_MODE_DIR : BLE_GAP_CONN_MODE_UND,
        .disc_mode = directed ? BLE_GAP_DISC_MODE_NON : BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x30, .itvl_max = 0x60, /* 30-60ms, low-duty directed mode. */
    };
    int duration = have_mac_identity ? (directed ? 10000 : 20000) : BLE_HS_FOREVER;
    rc = ble_gap_adv_start(mac_own_addr_type, directed ? &mac_identity : NULL, duration, &params, mac_gap_event, NULL);
    if (rc) { return rc; }
    ESP_LOGI(TAG, "Advertising \"%s\": %s duration=%dms", DEVICE_NAME,
             directed ? "directed to bonded host" : "open for host connection", duration);
    return 0;
}

int mac_hid_advertise(uint8_t own_addr_type)
{
    mac_own_addr_type = own_addr_type;
    advertising_ready = true;
    next_directed = true;
    load_mac_identity();
    int rc = ble_npl_callout_reset(&heartbeat, ble_npl_time_ms_to_ticks32(1000));
    if (rc) { return rc; }
    rc = ble_npl_callout_reset(&mouse_timer, ble_npl_time_ms_to_ticks32(MOUSE_PERIOD_MS));
    if (rc) { return rc; }
    rc = start_advertising();
    if (rc) { schedule_advertising(2000); }
    return rc;
}

void mac_hid_on_reset(void)
{
    advertising_ready = false;
    ble_npl_callout_stop(&heartbeat);
    ble_npl_callout_stop(&mouse_timer);
    ble_npl_callout_stop(&advertise_retry);
    mac_conn = BLE_HS_CONN_HANDLE_NONE;
    subscribed = encrypted = suspended = false;
    for (unsigned i = 0; i < 3; ++i) { native[i].subscribed = false; }
    clear_native();
}
