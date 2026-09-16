/* USB callbacks run on TinyUSB; input/keymap state belongs to NimBLE.
 * A bounded TX queue crosses tasks. Feature requests execute on the owner and
 * complete before GET_REPORT readback; no callback accesses keymap directly. */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "hid_descriptor.h"
#include "host_output.h"
#include "usb_hid.h"

_Static_assert(CFG_TUD_HID_EP_BUFSIZE >= KEYMAP_LENGTH + 1, "HID Feature buffer must include Report ID");
#define USB_QUEUE_SIZE 16
#define FEATURE_TIMEOUT_MS 1000
static const char *TAG = "USB_HID";
static SemaphoreHandle_t guard, feature_done;
static struct ble_npl_event link_event, feature_event, output_event;
static TaskHandle_t tx_handle;
static uint8_t in_flight_id;
static bool mounted, sleeping, in_flight, owner_started, owner_active;
static uint32_t generation, owner_generation;
static bool retry_state;
typedef struct { uint8_t id, length, data[REPORT_LENGTH]; } usb_packet_t;
static usb_packet_t packets[USB_QUEUE_SIZE];
static unsigned head, count;
static struct {
    bool busy, write;
    uint8_t id;
    int result;
    TickType_t deadline;
    uint32_t generation;
    uint8_t payload[KEYMAP_LENGTH];
} feature;
static char serial_number[13];
static const char *strings[] = {
    (const char[]){0x09, 0x04}, "Local Remote Bridge",
    "USB Keyboard & Mouse", serial_number, "Keyboard & Mouse",
};
static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN, 0, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, sizeof(report_map), 0x81, 16, 1),
};

static void lock(void) { xSemaphoreTake(guard, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(guard); }
static void wake_tx(void)
{
    if (tx_handle) { xTaskNotifyGive(tx_handle); }
}
static void on_output_ready(struct ble_npl_event *event)
{
    (void)event;
    hid_input_output_ready();
}
static void signal_link(void)
{
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &link_event);
}
static void mark_link(bool connected, bool suspended)
{
    lock();
    mounted = connected; sleeping = suspended;
    ++generation;
    head = count = 0;
    /* On suspend the transfer may still be outstanding; resume resets the
     * endpoint's readiness check. Old completions never dequeue new packets. */
    in_flight = false;
    unlock();
    signal_link();
    wake_tx();
}
static void usb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED) { mark_link(true, false); }
    else if (event->id == TINYUSB_EVENT_DETACHED) { mark_link(false, false); }
}
void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    mark_link(true, true);
}
void tud_resume_cb(void) { mark_link(true, false); }

/* Only the owner task reads or changes the input engine. */
static void on_link(struct ble_npl_event *event)
{
    (void)event;
    lock();
    bool changed = owner_generation != generation;
    owner_generation = generation;
    owner_active = mounted && !sleeping && owner_started;
    bool retry = retry_state;
    retry_state = false;
    unlock();
    if (changed) {
        hid_input_connected();
        /* Clear held source keys as well as transmitted state on host changes. */
        hid_input_mapping_changed();
        ESP_LOGI(TAG, "USB host active=%u", owner_active);
    }
    if (retry) {
        hid_input_macro_stop();
        for (unsigned i = 0; i < 3; ++i) { native[i].dirty = true; }
    }
    if (owner_active) { hid_input_publish(); }
}

bool host_output_ready(uint8_t id)
{
    if (id < 1 || id > 4) { return false; }
    lock();
    bool ready = owner_active && mounted && !sleeping && !retry_state && owner_generation == generation;
    unlock();
    return ready;
}
bool host_output_send(uint8_t id, const uint8_t *data, uint8_t length)
{
    static const uint8_t lengths[] = {0, REPORT_LENGTH, 8, 4, 1};
    if (id < 1 || id > 4 || length != lengths[id]) { return false; }
    lock();
    bool ready = owner_active && mounted && !sleeping && !retry_state && owner_generation == generation;
    /* Leave three entries for native key/button releases when debug input is busy. */
    if (!ready || count >= USB_QUEUE_SIZE || (id == REPORT_ID && count >= USB_QUEUE_SIZE - 3)) {
        unlock(); return false;
    }
    usb_packet_t *p = &packets[(head + count) % USB_QUEUE_SIZE];
    *p = (usb_packet_t){.id = id, .length = length};
    memcpy(p->data, data, length);
    ++count;
    unlock();
    wake_tx();
    return true;
}
bool host_output_pending(uint8_t id)
{
    lock();
    bool pending = in_flight && in_flight_id == id;
    for (unsigned i = 0; !pending && i < count; ++i) {
        pending = packets[(head + i) % USB_QUEUE_SIZE].id == id;
    }
    unlock();
    return pending;
}
bool macro_send_keyboard(const uint8_t data[8])
{
    lock();
    bool ready=owner_active && mounted && !sleeping && owner_generation==generation && !retry_state && !count && !in_flight;
    if(ready) {
        packets[head]=(usb_packet_t){.id=2,.length=8};
        memcpy(packets[head].data,data,8);count=1;
    }
    unlock();
    if(ready) { memcpy(native[0].data,data,8);native[0].dirty=false;wake_tx(); }
    return ready;
}

void host_output_clear(void)
{
    lock(); head = count = 0; unlock();
}
void host_output_discard_motion(void)
{
    lock();
    for (unsigned i = 0; i < count; ++i) {
        usb_packet_t *p = &packets[(head + i) % USB_QUEUE_SIZE];
        if (p->id == 3) { memset(p->data + 1, 0, 3); }
    }
    unlock();
}

void tud_hid_report_complete_cb(uint8_t instance, const uint8_t *report, uint16_t len)
{
    (void)instance; (void)report; (void)len;
    lock(); in_flight = false; unlock();
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &output_event);
    wake_tx();
}
void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t type, const uint8_t *report, uint16_t bytes)
{
    (void)instance; (void)report; (void)bytes;
    if (type != HID_REPORT_TYPE_INPUT) { return; }
    lock();
    in_flight = false; head = count = 0; retry_state = true;
    unlock();
    signal_link();
}
/* One in-flight USB transfer. Keep stack calls on this worker or TinyUSB task;
 * the NimBLE notification path only copies into the bounded queue. */
static void tx_once(void)
{
    bool failed = false;
    lock();
    if (mounted && !sleeping && !in_flight && count && tud_hid_ready()) {
        usb_packet_t p = packets[head];
        head = (head + 1) % USB_QUEUE_SIZE; --count;
        in_flight = true;
        in_flight_id = p.id;
        if (!tud_hid_report(p.id, p.data, p.length)) {
            in_flight = false;
            /* Discard old motion/click history; converge to current state. */
            head = count = 0; retry_state = failed = true;
        }
    }
    unlock();
    if (failed) { signal_link(); }
}
static void tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        tx_once();
        lock();
        bool retry = mounted && !sleeping && count && !in_flight;
        unlock();
        /* Notifications are latched, including arrivals before this wait.
         * Retry a temporarily unavailable endpoint without polling while idle
         * or while waiting for a completion callback. */
        ulTaskNotifyTake(pdTRUE, retry ? 1 : portMAX_DELAY);
    }
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return instance == 0 ? report_map : NULL;
}
static void on_feature(struct ble_npl_event *event)
{
    (void)event;
    /* The USB caller cannot reuse this request while busy, including after a
     * timeout. Keep the lock out of NVS and host_output_clear(). */
    lock();
    bool write = feature.write;
    bool valid = feature.busy && mounted && !sleeping && owner_started && feature.generation == generation &&
                 (int32_t)(feature.deadline - xTaskGetTickCount()) > 0;
    unlock();
    int rc = KEYMAP_INVALID;
    if (valid) {
        if(feature.id==MACRO_REPORT_ID) {
            if(write) {
                macro_request(feature.payload,(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS));
                uint8_t answer[64];macro_response(answer);
                if(answer[5]==MACRO_OK && (feature.payload[3]==MACRO_STOP || feature.payload[3]==MACRO_COMMIT))hid_input_macro_stop();
            } else macro_response(feature.payload);
            rc=KEYMAP_OK;
        } else if (write) {
            rc = keymap_apply(feature.payload, KEYMAP_LENGTH);
            if (rc == KEYMAP_OK) { hid_input_mapping_changed(); }
        } else { keymap_read(feature.payload); rc = KEYMAP_OK; }
    }
    lock();
    feature.result = rc;
    feature.busy = false;
    xSemaphoreGive(feature_done);
    unlock();
}
static bool feature_request(uint8_t id, bool write, uint8_t payload[KEYMAP_LENGTH])
{
    lock();
    if (feature.busy || !owner_started || !mounted || sleeping) { unlock(); return false; }
    xSemaphoreTake(feature_done, 0); /* Drain a completion from a timed-out request. */
    feature.busy = true; feature.write = write; feature.id=id; feature.generation = generation;
    feature.deadline = xTaskGetTickCount() + pdMS_TO_TICKS(FEATURE_TIMEOUT_MS);
    if (write) { memcpy(feature.payload, payload, KEYMAP_LENGTH); }
    unlock();
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &feature_event);
    if (xSemaphoreTake(feature_done, pdMS_TO_TICKS(FEATURE_TIMEOUT_MS)) != pdTRUE) { return false; }
    lock();
    bool ok = feature.result == KEYMAP_OK;
    if (ok && !write) { memcpy(payload, feature.payload, KEYMAP_LENGTH); }
    unlock();
    return ok;
}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type,
                              uint8_t *buffer, uint16_t reqlen)
{
    if (instance || (id != KEYMAP_REPORT_ID && id != MACRO_REPORT_ID) || type != HID_REPORT_TYPE_FEATURE || reqlen < KEYMAP_LENGTH) { return 0; }
    return feature_request(id, false, buffer) ? KEYMAP_LENGTH : 0;
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type,
                           const uint8_t *buffer, uint16_t length)
{
    if (instance || (id != KEYMAP_REPORT_ID && id != MACRO_REPORT_ID) || type != HID_REPORT_TYPE_FEATURE || length != KEYMAP_LENGTH) { return; }
    uint8_t payload[KEYMAP_LENGTH];
    memcpy(payload, buffer, sizeof(payload));
    /* TinyUSB's SET callback has no status return. Failed writes leave the
     * revision unchanged; the mapper always checks GET_REPORT after SET. */
    if (!feature_request(id, true, payload)) { ESP_LOGW(TAG, "USB keymap write rejected or timed out; read back before retry"); }
}

int usb_hid_init(void)
{
    guard = xSemaphoreCreateMutex();
    feature_done = xSemaphoreCreateBinary();
    if (!guard || !feature_done) { return BLE_HS_ENOMEM; }
    ble_npl_event_init(&link_event, on_link, NULL);
    ble_npl_event_init(&feature_event, on_feature, NULL);
    ble_npl_event_init(&output_event, on_output_ready, NULL);
    keymap_init();
    macro_init();
    int rc = hid_input_init();
    if (rc) { return rc; }
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) { return BLE_HS_EUNKNOWN; }
    snprintf(serial_number, sizeof(serial_number), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.event_cb = usb_event;
    cfg.descriptor.full_speed_config = configuration;
    cfg.descriptor.string = strings;
    cfg.descriptor.string_count = sizeof(strings) / sizeof(strings[0]);
    if (tinyusb_driver_install(&cfg) != ESP_OK) { return BLE_HS_EUNKNOWN; }
    return xTaskCreate(tx_task, "usb_hid_tx", 3072, NULL, 4, &tx_handle) == pdPASS ? 0 : BLE_HS_ENOMEM;
}
/* Start input timers on every NimBLE sync. */
int usb_hid_start(void)
{
    lock(); owner_started = true; ++generation; unlock();
    on_link(NULL);
    return hid_input_start();
}
void usb_hid_on_reset(void)
{
    lock(); owner_started = owner_active = false; ++generation; unlock();
    hid_input_reset();
}
