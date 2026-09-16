#include "hid_input.h"
#include "host_output.h"
#include <string.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "keymap.h"
#include "macro.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "input_codec.h"
static const char *TAG = "HID_INPUT";
static uint8_t report[REPORT_LENGTH] = {1};
static uint16_t sequence;
static bool suspended, remote_ready;
static bool cursor_mode, mode_known;
static bool home_held;
static int motion_remainder[2];
static uint8_t remote_keys[8], remote_mouse_buttons;
static struct ble_npl_callout heartbeat;
static struct ble_npl_callout macro_timer;
static uint16_t macro_down, macro_armed;
static int macro_pending_slot=-1;
static uint32_t macro_now(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }
uint32_t macro_random(void) { return esp_random(); }
#define MOUSE_QUEUE_SIZE 8
static unsigned mouse_dropped;
typedef struct { uint8_t buttons; int16_t axis[3]; } mouse_pending_t;
static mouse_pending_t mouse_queue[MOUSE_QUEUE_SIZE];
static unsigned mouse_count;
static void flush_mouse(void);
native_report_t native[3] = {
    {.id = 2, .length = 8}, {.id = 3, .length = 4}, {.id = 4, .length = 1},
};
static bool notify_native(native_report_t *r, const uint8_t *data)
{
    return !suspended && host_output_ready(r->id) && host_output_send(r->id, data, r->length);
}

static void flush_native(void)
{
    for (unsigned i = 0; i < 3; ++i) {
        native_report_t *r = &native[i];
        if (i == 0 && macro_active()) { continue; }
        if (i == 1) { flush_mouse(); continue; }
        if (r->dirty && notify_native(r, r->data)) { r->dirty = false; }
    }
}

static void clear_native(void)
{
    macro_pending_slot=-1;
    macro_cancel();
    macro_down = macro_armed = 0;
    host_output_clear();
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
    if (suspended || !host_output_ready(REPORT_ID)) { return; }
    ++sequence;
    report[2] = sequence & 0xff;
    report[3] = sequence >> 8;
    host_output_send(REPORT_ID, report, sizeof(report));
}

static void on_heartbeat(struct ble_npl_event *event)
{
    (void)event;
    if (mouse_dropped) {
        ESP_LOGW(TAG, "HID flow: mouse_dropped=%u", mouse_dropped);
        mouse_dropped = 0;
    }
    publish(false);
    flush_native(); /* Retry latest held/released state, never relative movement. */
    int rc = ble_npl_callout_reset(&heartbeat, ble_npl_time_ms_to_ticks32(1000));
    if (rc) { ESP_LOGE(TAG, "heartbeat scheduling failed: rc=%d", rc); }
}

void hid_input_remote_ready(bool ready)
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

void hid_input_sensor_mode(bool cursor)
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
    unsigned mode=cursor_mode?1:0;
    uint8_t filtered[8];memcpy(filtered,remote_keys,8);
    uint8_t native_buttons=remote_mouse_buttons;
    uint16_t pressed=0;
    for(unsigned i=2;i<9;++i) {
        uint8_t button=i<8?remote_keys[i]:(remote_mouse_buttons&1?0x28:0);
        int slot=macro_slot(button,mode);
        if(slot<0)continue;
        pressed|=1u<<(slot%14);
        if(macro_enabled((unsigned)slot)) {
            if(i<8)filtered[i]=0;else native_buttons&=~1u;
        }
    }
    uint16_t rising=pressed & ~macro_down & macro_armed;
    macro_armed |= (uint16_t)~pressed;
    macro_down=pressed;
    for(unsigned i=0;i<14;++i)if(rising & (1u<<i)) {
        if(macro_active() || macro_pending_slot>=0) { clear_native();flush_native();return; }
        if(macro_enabled(mode*14+i) && host_output_ready(2) && !suspended) {
            /* Clear prior held outputs before starting. Never mix manual modifiers
             * or clicks with a string. Any new mapped button stops the macro. */
            clear_native();
            macro_down=pressed;macro_armed=(uint16_t)~pressed;
            flush_native();
            /* clear_native schedules a release; the timer starts after it drains. */
            macro_pending_slot=(int)(mode*14+i);
            break;
        }
    }
    input_map(filtered,mode,native_buttons,mapped,&consumer,&mouse[0]);
    if (motion && !(cursor_mode && home_held)) { memcpy(mouse + 1, motion + 1, 3); }
    if (macro_active() || macro_pending_slot>=0) { return; }
    update_native(&native[0], mapped);
    update_native(&native[2], &consumer);
    hid_input_mouse(mouse);
    flush_native();
}

void hid_input_keyboard(const uint8_t keyboard[8])
{
    remote_ready = true;
    memcpy(report + 4, keyboard, 8);
    publish(true);
    memcpy(remote_keys, keyboard, sizeof(remote_keys));
    home_held = false;
    for (unsigned i = 2; i < 8; ++i) { home_held |= keyboard[i] == 0x4a; }
    if (cursor_mode && home_held) {
        host_output_discard_motion();
        /* Remove unsent movement too, while preserving queued click transitions.
         * Never replay movement or fractional deltas after HOME is released. */
        for (unsigned i = 0; i < mouse_count; ++i) {
            memset(mouse_queue[i].axis, 0, sizeof(mouse_queue[i].axis));
        }
        memset(motion_remainder, 0, sizeof(motion_remainder));
    }
    map_remote(NULL);
}

void hid_input_remote_mouse(const uint8_t mouse[4])
{
    /* Mouse traffic recovers an unknown mode after bridge reconnect. Once an
     * explicit stop was seen, trailing sensor packets cannot re-enable it. */
    if (!mode_known) { set_mapping_mode(true); }
    if (!cursor_mode) { return; }
    remote_mouse_buttons = mouse[0];
    map_remote(mouse);
}

/* Send immediately when USB has room. While a mouse report is outstanding,
 * combine motion with the same buttons and preserve separate button edges.
 * Only the NimBLE owner accesses this accumulator, including completion events. */
void hid_input_mouse(const uint8_t mouse[4])
{
    native_report_t *r = &native[1];
    bool changed = r->data[0] != mouse[0];
    r->data[0] = mouse[0];
    if (!host_output_ready(r->id) || suspended) {
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
            /* Bound overload to two maximum source deltas; split into HID reports. */
            int scaled_limit = 254 * keymap_mouse_speed() / KEYMAP_SPEED_DEFAULT;
            if (scaled_limit > limit) { limit = scaled_limit; }
        }
        int value = p->axis[i] + delta;
        if (value > limit) { value = limit; ++mouse_dropped; }
        if (value < -limit) { value = -limit; ++mouse_dropped; }
        p->axis[i] = value;
    }
    r->dirty = true;
    flush_mouse();
}

static void flush_mouse(void)
{
    native_report_t *r = &native[1];
    if (suspended || !host_output_ready(r->id) || host_output_pending(r->id)) { return; }
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
        }
    } else if (r->dirty && notify_native(r, r->data)) {
        r->dirty = false;
    }
}

void hid_input_output_ready(void) { flush_native(); }


static void on_macro_tick(struct ble_npl_event *event)
{
    (void)event;
    if(!suspended && host_output_ready(2)) {
        macro_tick(macro_now());
        if(macro_pending_slot>=0 && !macro_active()) {
            int slot=macro_pending_slot;macro_pending_slot=-1;macro_start((unsigned)slot,macro_now());
        }
    }
    ble_npl_callout_reset(&macro_timer,ble_npl_time_ms_to_ticks32(5));
}
void hid_input_macro_stop(void)
{
    uint16_t held=macro_down;
    clear_native();
    /* Saving while idle must allow the very next press. Held buttons remain
     * disarmed until released, including after an explicit stop or TX failure. */
    macro_down=held;macro_armed=(uint16_t)~held;
    flush_native();
}

int hid_input_init(void)
{
    int rc = ble_npl_callout_init(&heartbeat, nimble_port_get_dflt_eventq(), on_heartbeat, NULL);
    if (rc) { return rc; }
    return ble_npl_callout_init(&macro_timer,nimble_port_get_dflt_eventq(),on_macro_tick,NULL);
}
int hid_input_start(void)
{
    int rc = ble_npl_callout_reset(&heartbeat, ble_npl_time_ms_to_ticks32(1000));
    return rc?rc:ble_npl_callout_reset(&macro_timer,ble_npl_time_ms_to_ticks32(5));
}
void hid_input_connected(void)
{
    suspended = false;
    sequence = 0;
    clear_native();
}
void hid_input_reset(void)
{
    ble_npl_callout_stop(&heartbeat);
    ble_npl_callout_stop(&macro_timer);
    hid_input_connected();
}
void hid_input_publish(void) { publish(false); flush_native(); }
void hid_input_suspend(bool enabled)
{
    suspended = enabled;
    if (suspended) { clear_native(); }
    else { flush_native(); }
}
void hid_input_mapping_changed(void)
{
    home_held = false;
    clear_native(); flush_native();
    memset(remote_keys, 0, sizeof(remote_keys)); remote_mouse_buttons = 0;
}
const uint8_t *hid_input_vendor_report(void) { return report; }
