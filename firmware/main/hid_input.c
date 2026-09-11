#include "hid_internal.h"
#include <string.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "bridge_peers.h"
#include "keymap.h"
#include "input_codec.h"
static const char *TAG = "HID_INPUT";
static uint8_t report[REPORT_LENGTH] = {1};
uint16_t input_handle;
static uint16_t sequence;
static bool subscribed, suspended, remote_ready;
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
native_report_t native[3] = {
    {.id = 2, .length = 8}, {.id = 3, .length = 4}, {.id = 4, .length = 1},
};
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
    if (host_link_state()->conn == BLE_HS_CONN_HANDLE_NONE || !r->subscribed || !host_link_state()->encrypted || suspended) { return false; }
    if (!tx_has_room()) { return false; }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, r->length);
    if (!om) { ++tx_errors; return false; }
    int rc = ble_gatts_notify_custom(host_link_state()->conn, r->handle, om);
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
    if (host_link_state()->conn == BLE_HS_CONN_HANDLE_NONE || !subscribed || !host_link_state()->encrypted || suspended) { return; }
    if (!tx_has_room()) { return; }
    ++sequence;
    report[2] = sequence & 0xff;
    report[3] = sequence >> 8;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (!om) { ++tx_errors; return; }
    int rc = ble_gatts_notify_custom(host_link_state()->conn, input_handle, om);
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
    if (host_link_state()->conn == BLE_HS_CONN_HANDLE_NONE || !r->subscribed || !host_link_state()->encrypted || suspended) {
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


int hid_input_init(void)
{
    int rc = ble_npl_callout_init(&heartbeat, nimble_port_get_dflt_eventq(), on_heartbeat, NULL);
    if (rc) { return rc; }
    return ble_npl_callout_init(&mouse_timer, nimble_port_get_dflt_eventq(), on_mouse_tick, NULL);
}
int hid_input_start(void)
{
    int rc = ble_npl_callout_reset(&heartbeat, ble_npl_time_ms_to_ticks32(1000));
    return rc ? rc : ble_npl_callout_reset(&mouse_timer, ble_npl_time_ms_to_ticks32(MOUSE_PERIOD_MS));
}
void hid_input_connected(void)
{
    subscribed = suspended = false;
    sequence = 0;
    clear_native();
    for (unsigned i = 0; i < 3; ++i) { native[i].subscribed = false; }
}
void hid_input_reset(void)
{
    ble_npl_callout_stop(&heartbeat);
    ble_npl_callout_stop(&mouse_timer);
    hid_input_connected();
}
void hid_input_publish(void) { publish(false); flush_native(); }
void hid_input_subscribe(uint16_t handle, bool enabled)
{
    if (handle == input_handle) { subscribed = enabled; publish(false); }
    for (unsigned i = 0; i < 3; ++i) {
        if (handle == native[i].handle) { native[i].subscribed = enabled; native[i].dirty = true; }
    }
    flush_native();
}
void hid_input_tx_failed(uint16_t handle)
{
    for (unsigned i = 0; i < 3; ++i) {
        if (native[i].handle == handle) { native[i].dirty = true; }
    }
}
void hid_input_suspend(bool enabled)
{
    suspended = enabled;
    if (suspended) { mouse_count = 0; }
    else { flush_native(); }
}
void hid_input_mapping_changed(void)
{
    clear_native(); flush_native();
    memset(remote_keys, 0, sizeof(remote_keys)); remote_mouse_buttons = 0;
}
const uint8_t *hid_input_vendor_report(void) { return report; }
