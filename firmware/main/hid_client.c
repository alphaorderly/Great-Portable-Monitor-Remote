#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "hid_client.h"
#include "mac_hid.h"
#include "input_codec.h"
#include "input_log.h"

#define HID_SERVICE_UUID 0x1812
#define HID_REPORT_MAP_UUID 0x2a4b
#define HID_REPORT_UUID 0x2a4d
#define HID_PROTOCOL_MODE_UUID 0x2a4e
#define REPORT_REFERENCE_UUID 0x2908
#define CCCD_UUID 0x2902
#define KEYBOARD_REPORT_ID 1
#define INPUT_REPORT_TYPE 1
#define KEYBOARD_REPORT_LENGTH 8
#define MAX_HID_CHARACTERISTICS 32
#define MAX_REPORT_MAP_LENGTH 512
#define HEX_CHUNK_LENGTH 32

static const char *TAG = "REMOTE_HID";
static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(HID_SERVICE_UUID);

/* Actual keyboard collection from logs/remote_report_map.bin, offset 172.
 * Only this observed layout is decoded; a different map is still logged raw.
 */
static const uint8_t known_keyboard_collection[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xe0,
    0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08,
    0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x03,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0xff, 0x05, 0x07, 0x19, 0x00,
    0x29, 0xff, 0x81, 0x00, 0xc0,
};

/* Captured Report Map, offset 118: buttons, signed relative X/Y/wheel. */
static const uint8_t known_mouse_collection[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x03, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x03, 0x81, 0x02, 0x75, 0x05, 0x95, 0x01, 0x81, 0x01, 0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08,
    0x95, 0x03, 0x81, 0x06, 0xc0, 0xc0,
};

typedef struct {
    struct ble_gatt_chr chr;
    uint16_t cccd;
    uint16_t reference;
    uint8_t report_id;
    uint8_t report_type;
    bool write_accepted;
    bool readback_confirmed;
} hid_characteristic_t;

static struct {
    bool started;
    bool active;
    bool listening;
    bool subscribed;
    bool keyboard_layout;
    bool mouse_layout;
    uint16_t conn;
    uint16_t service_start;
    uint16_t service_end;
    uint16_t map_handle;
    uint16_t protocol_handle;
    uint16_t input_handle;
    uint16_t map_length;
    unsigned chr_count;
    unsigned report_index;
    unsigned input_count;
    unsigned notification_count;
    hid_characteristic_t chars[MAX_HID_CHARACTERISTICS];
    uint8_t map[MAX_REPORT_MAP_LENGTH];
} client;

static void next_report(void);
static int on_descriptors(uint16_t conn, const struct ble_gatt_error *error,
                          uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg);
static int on_reference(uint16_t conn, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg);
static int on_map(uint16_t conn, const struct ble_gatt_error *error,
                 struct ble_gatt_attr *attr, void *arg);

static bool current(uint16_t conn)
{
    return client.active && conn == client.conn;
}

static void fail(const char *step, int status, uint16_t handle)
{
    ESP_LOGE(TAG, "%s failed: NimBLE status=%d (0x%X) handle=0x%04X",
             step, status, (unsigned)status, handle);
    client.active = false;
    ESP_LOGE(TAG, "HID setup stopped; notification logging remains %s; keep this log",
             client.listening ? "active until disconnect" : "inactive");
}

static void requested(const char *step, int status, uint16_t handle)
{
    ESP_LOGI(TAG, "%s request: status=%d (0x%X) handle=0x%04X",
             step, status, (unsigned)status, handle);
    if (status != 0) { fail(step, status, handle); }
}

static int dump_mbuf(const struct os_mbuf *om, int report_id, uint16_t handle, bool indication)
{
    uint8_t chunk[HEX_CHUNK_LENGTH];
    const unsigned length = OS_MBUF_PKTLEN(om);
    unsigned offset = 0;
    do {
        const unsigned n = length - offset < sizeof(chunk) ? length - offset : sizeof(chunk);
        int rc = os_mbuf_copydata(om, offset, n, chunk);
        if (rc != 0) { return rc; }
        input_log_submit(client.notification_count, report_id, handle, length, offset, indication, chunk, n);
        offset += n;
    } while (offset < length);
    return 0;
}

static bool known_layout(const uint8_t *collection, size_t length)
{
    /* Walk item boundaries so a byte pattern inside a long item is not treated as a collection. */
    for (size_t offset = 0; offset < client.map_length;) {
        if (client.map_length - offset >= length &&
            memcmp(client.map + offset, collection, length) == 0) {
            return true;
        }
        const uint8_t prefix = client.map[offset];
        size_t item_length;
        if (prefix == 0xfe) {
            if (client.map_length - offset < 3) { return false; }
            item_length = 3 + client.map[offset + 1];
        } else {
            unsigned size = prefix & 3;
            item_length = 1 + (size == 3 ? 4 : size);
        }
        if (item_length > client.map_length - offset) { return false; }
        offset += item_length;
    }
    return false;
}

static int on_cccd_read(uint16_t conn, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    hid_characteristic_t *report = arg;
    if (!current(conn)) { return 0; }
    if (error->status != 0) { fail("CCCD readback", error->status, error->att_handle); return 0; }
    uint8_t value[2];
    if (!attr || attr->handle != report->cccd || !attr->om ||
        OS_MBUF_PKTLEN(attr->om) != sizeof(value) ||
        os_mbuf_copydata(attr->om, 0, sizeof(value), value) != 0) {
        fail("CCCD readback length/handle/data", BLE_HS_EBADDATA, report->cccd);
        return BLE_HS_EAPP;
    }
    ESP_LOGI(TAG, "CCCD readback: handle=0x%04X value=%02X %02X notification_enabled=%u",
             report->cccd, value[0], value[1], !!(value[0] & 1));
    if (value[0] != 1 || value[1] != 0) {
        ESP_LOGW(TAG, "CCCD readback differs from 01 00: ID=%u; keep listening and continue subscriptions",
                 report->report_id);
    } else {
        report->readback_confirmed = true;
    }
    ++client.report_index;
    next_report();
    return 0;
}

static int on_subscribed(uint16_t conn, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    hid_characteristic_t *report = arg;
    if (!current(conn)) { return 0; }
    ESP_LOGI(TAG, "GATT CCCD write result: status=%d (0x%X) cccd=0x%04X",
             error->status, (unsigned)error->status, report->cccd);
    if (error->status != 0) { fail("notification subscription", error->status, error->att_handle); return 0; }
    report->write_accepted = true;
    if (report->report_id == KEYBOARD_REPORT_ID) { client.subscribed = true; }
    requested("CCCD readback", ble_gattc_read(conn, report->cccd, on_cccd_read, arg), report->cccd);
    return 0;
}

static int on_reference(uint16_t conn, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg)
{
    hid_characteristic_t *report = arg;
    if (!current(conn)) { return 0; }
    if (error->status != 0) { fail("Report Reference read", error->status, error->att_handle); return 0; }
    uint8_t reference[2];
    if (!attr || attr->handle != report->reference || !attr->om || OS_MBUF_PKTLEN(attr->om) != sizeof(reference) ||
        os_mbuf_copydata(attr->om, 0, sizeof(reference), reference) != 0) {
        fail("Report Reference length/data", BLE_HS_EBADDATA, report->reference);
        return BLE_HS_EAPP;
    }
    ESP_LOGI(TAG, "HID report discovered: value_handle=0x%04X reference=0x%04X ID=%u type=%u",
             report->chr.val_handle, report->reference, reference[0], reference[1]);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, reference, sizeof(reference), ESP_LOG_INFO);
    report->report_id = reference[0];
    report->report_type = reference[1];
    if (report->report_type == INPUT_REPORT_TYPE) {
        if (!report->cccd) { fail("missing input CCCD", BLE_HS_ENOENT, report->chr.val_handle); return 0; }
        if (report->report_id == KEYBOARD_REPORT_ID) {
            if (client.input_handle && client.input_handle != report->chr.val_handle) {
                fail("duplicate keyboard Input Report", BLE_HS_EBADDATA, report->chr.val_handle);
                return BLE_HS_EAPP;
            }
            client.input_handle = report->chr.val_handle;
        }
        const uint8_t enable_notifications[] = {0x01, 0x00};
        requested("notification subscription (CCCD=01 00)",
                  ble_gattc_write_flat(conn, report->cccd, enable_notifications,
                                       sizeof(enable_notifications), on_subscribed, report), report->cccd);
    } else {
        ++client.report_index;
        next_report();
    }
    return 0;
}

static int on_descriptors(uint16_t conn, const struct ble_gatt_error *error,
                          uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    hid_characteristic_t *report = arg;
    if (!current(conn)) { return 0; }
    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "GATT descriptor discovery complete: value_handle=0x%04X", report->chr.val_handle);
        if (!report->reference) {
            ESP_LOGW(TAG, "Report Reference absent; skip value_handle=0x%04X", report->chr.val_handle);
            ++client.report_index;
            next_report();
        } else {
            requested("Report Reference read", ble_gattc_read(conn, report->reference, on_reference, report), report->reference);
        }
        return 0;
    }
    if (error->status != 0) { fail("descriptor discovery", error->status, error->att_handle); return 0; }
    if (!dsc || chr_val_handle != report->chr.val_handle) {
        fail("descriptor association", BLE_HS_EBADDATA, chr_val_handle);
        return BLE_HS_EAPP;
    }
    char uuid[BLE_UUID_STR_LEN];
    ESP_LOGI(TAG, "GATT descriptor: value_handle=0x%04X handle=0x%04X UUID=%s",
             chr_val_handle, dsc->handle, ble_uuid_to_str(&dsc->uuid.u, uuid));
    const uint16_t uuid16 = ble_uuid_u16(&dsc->uuid.u);
    uint16_t *slot = uuid16 == CCCD_UUID ? &report->cccd :
                     uuid16 == REPORT_REFERENCE_UUID ? &report->reference : NULL;
    if (slot) {
        if (*slot) { fail("duplicate HID descriptor", BLE_HS_EBADDATA, dsc->handle); return BLE_HS_EAPP; }
        *slot = dsc->handle;
    }
    return 0;
}

static void next_report(void)
{
    while (client.active && client.report_index < client.chr_count) {
        hid_characteristic_t *report = &client.chars[client.report_index];
        if (ble_uuid_u16(&report->chr.uuid.u) != HID_REPORT_UUID ||
            !(report->chr.properties & BLE_GATT_CHR_PROP_NOTIFY)) {
            ++client.report_index;
            continue;
        }
        const uint16_t end = client.report_index + 1 < client.chr_count
                             ? client.chars[client.report_index + 1].chr.def_handle - 1
                             : client.service_end;
        if (end <= report->chr.val_handle) {
            ESP_LOGW(TAG, "No descriptor range for Report value_handle=0x%04X", report->chr.val_handle);
            ++client.report_index;
            continue;
        }
        ESP_LOGI(TAG, "GATT descriptor discovery: value_handle=0x%04X through=0x%04X", report->chr.val_handle, end);
        /* NimBLE expects the VALUE handle here; internally discovery starts at value+1. */
        requested("descriptor discovery", ble_gattc_disc_all_dscs(client.conn, report->chr.val_handle,
                                                                 end, on_descriptors, report), report->chr.val_handle);
        return;
    }
    if (client.active) {
        if (!client.input_handle || !client.subscribed) {
            fail("Report ID 1 Input not subscribed", BLE_HS_ENOENT, client.service_start);
            return;
        }
        unsigned accepted = 0, confirmed = 0;
        for (unsigned i = 0; i < client.chr_count; ++i) {
            accepted += client.chars[i].write_accepted;
            confirmed += client.chars[i].readback_confirmed;
        }
        ESP_LOGI("REMOTE_STATUS", "HID LISTENING: Input CCCD writes_accepted=%u readbacks_confirmed=%u keyboard_handle=0x%04X",
                 accepted, confirmed, client.input_handle);
        mac_hid_remote_ready(true);
        ESP_LOGI("REMOTE_STATUS", "ORDER: POWER > GEAR > CURSOR > SETTINGS > UP > DOWN > LEFT > RIGHT > OK > BACK > HOME > APPS > VOL+ > VOL-");
        if (confirmed != accepted) {
            ESP_LOGW(TAG, "Subscription state remains uncertain; only actual notifications prove input reception");
        }
    }
}

static int on_protocol_mode(uint16_t conn, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    (void)arg;
    if (!current(conn)) { return 0; }
    if (error->status != 0) { fail("Protocol Mode read", error->status, error->att_handle); return 0; }
    uint8_t mode;
    if (!attr || attr->handle != client.protocol_handle || !attr->om ||
        OS_MBUF_PKTLEN(attr->om) != 1 || os_mbuf_copydata(attr->om, 0, 1, &mode) != 0) {
        fail("Protocol Mode length/handle/data", BLE_HS_EBADDATA, client.protocol_handle);
        return BLE_HS_EAPP;
    }
    ESP_LOGI(TAG, "HID Protocol Mode: handle=0x%04X value=0x%02X (%s)",
             client.protocol_handle, mode, mode == 1 ? "Report" : mode == 0 ? "Boot" : "Reserved");
    if (mode != 1) { ESP_LOGW(TAG, "Protocol Mode is not Report; retain value for this diagnostic trial"); }
    next_report();
    return 0;
}

static int on_map(uint16_t conn, const struct ble_gatt_error *error,
                 struct ble_gatt_attr *attr, void *arg)
{
    (void)arg;
    if (!current(conn)) { return BLE_HS_EAPP; }
    if (error->status == BLE_HS_EDONE) {
        if (!client.map_length) { fail("empty Report Map", BLE_HS_EBADDATA, client.map_handle); return 0; }
        client.keyboard_layout = known_layout(known_keyboard_collection, sizeof(known_keyboard_collection));
        client.mouse_layout = known_layout(known_mouse_collection, sizeof(known_mouse_collection));
        ESP_LOGI("REMOTE_STATUS", "verified layouts: keyboard=%u mouse=%u", client.keyboard_layout, client.mouse_layout);
        ESP_LOGI(TAG, "HID Report Map read complete: len=%u known_keyboard_layout=%u",
                 client.map_length, client.keyboard_layout);
        if (!client.keyboard_layout) { ESP_LOGW(TAG, "Unrecognized keyboard layout; input will be raw only"); }
        if (client.protocol_handle) {
            requested("Protocol Mode read", ble_gattc_read(conn, client.protocol_handle, on_protocol_mode, NULL),
                      client.protocol_handle);
        } else {
            ESP_LOGW(TAG, "Protocol Mode absent; continue with Report ID 1 subscription");
            next_report();
        }
        return 0;
    }
    if (error->status != 0) { fail("Report Map read", error->status, error->att_handle); return 0; }
    if (!attr || !attr->om || attr->handle != client.map_handle || attr->offset != client.map_length) {
        fail("Report Map offset/handle", BLE_HS_EBADDATA, client.map_handle);
        return BLE_HS_EAPP;
    }
    const unsigned length = OS_MBUF_PKTLEN(attr->om);
    if (length > sizeof(client.map) - client.map_length ||
        os_mbuf_copydata(attr->om, 0, length, client.map + client.map_length) != 0) {
        fail("Report Map capacity/data", BLE_HS_EMSGSIZE, client.map_handle);
        return BLE_HS_EAPP;
    }
    ESP_LOGI(TAG, "HID Report Map chunk: offset=%u len=%u", attr->offset, length);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, client.map + client.map_length, length, ESP_LOG_INFO);
    client.map_length += length;
    return 0;
}

static int on_characteristics(uint16_t conn, const struct ble_gatt_error *error,
                              const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (!current(conn)) { return 0; }
    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "GATT characteristic discovery complete: count=%u", client.chr_count);
        if (!client.map_handle) { fail("Report Map not found", BLE_HS_ENOENT, client.service_start); return 0; }
        requested("Report Map long read", ble_gattc_read_long(conn, client.map_handle, 0, on_map, NULL), client.map_handle);
        return 0;
    }
    if (error->status != 0) { fail("characteristic discovery", error->status, error->att_handle); return 0; }
    if (!chr || client.chr_count == MAX_HID_CHARACTERISTICS ||
        chr->def_handle < client.service_start || chr->val_handle <= chr->def_handle ||
        chr->val_handle > client.service_end ||
        (client.chr_count && chr->def_handle <= client.chars[client.chr_count - 1].chr.val_handle)) {
        fail("characteristic range/capacity", BLE_HS_EBADDATA, client.service_start);
        return BLE_HS_EAPP;
    }
    client.chars[client.chr_count++].chr = *chr;
    char uuid[BLE_UUID_STR_LEN];
    ESP_LOGI(TAG, "GATT characteristic: UUID=%s declaration=0x%04X value=0x%04X properties=0x%02X",
             ble_uuid_to_str(&chr->uuid.u, uuid), chr->def_handle, chr->val_handle, chr->properties);
    if (ble_uuid_u16(&chr->uuid.u) == HID_REPORT_MAP_UUID) {
        if (client.map_handle || !(chr->properties & BLE_GATT_CHR_PROP_READ)) {
            fail("ambiguous/unreadable Report Map", BLE_HS_EBADDATA, chr->val_handle);
            return BLE_HS_EAPP;
        }
        client.map_handle = chr->val_handle;
    }
    if (ble_uuid_u16(&chr->uuid.u) == HID_PROTOCOL_MODE_UUID) {
        if (client.protocol_handle || !(chr->properties & BLE_GATT_CHR_PROP_READ)) {
            fail("ambiguous/unreadable Protocol Mode", BLE_HS_EBADDATA, chr->val_handle);
            return BLE_HS_EAPP;
        }
        client.protocol_handle = chr->val_handle;
    }
    return 0;
}

static int on_service(uint16_t conn, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (!current(conn)) { return 0; }
    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "GATT HID service discovery complete");
        if (!client.service_start) { fail("HID service not found", BLE_HS_ENOENT, 0); return 0; }
        requested("characteristic discovery", ble_gattc_disc_all_chrs(conn, client.service_start,
                                                                     client.service_end, on_characteristics, NULL), client.service_start);
        return 0;
    }
    if (error->status != 0) { fail("HID service discovery", error->status, error->att_handle); return 0; }
    if (!service || client.service_start || !service->start_handle || service->end_handle <= service->start_handle) {
        fail("HID service range/multiple instances", BLE_HS_EBADDATA, 0);
        return BLE_HS_EAPP;
    }
    client.service_start = service->start_handle;
    client.service_end = service->end_handle;
    ESP_LOGI(TAG, "HID service discovered: UUID=0x1812 start=0x%04X end=0x%04X", client.service_start, client.service_end);
    return 0;
}

void hid_client_start(uint16_t conn_handle)
{
    if (client.started) { return; }
    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0 || !desc.sec_state.encrypted || !desc.sec_state.bonded) {
        fail("HID security gate", rc ? rc : BLE_HS_EENCRYPT, 0);
        return;
    }
    memset(&client, 0, sizeof(client));
    client.started = true;
    client.conn = conn_handle;
    client.active = true;
    client.listening = true;
    ESP_LOGI(TAG, "GATT service discovery starting after encryption/bond verification");
    requested("HID service discovery", ble_gattc_disc_svc_by_uuid(conn_handle, &hid_uuid.u, on_service, NULL), 0);
}

void hid_client_stop(uint16_t conn_handle)
{
    if (!client.started || client.conn != conn_handle) { return; }
    client.active = false;
    client.listening = false;
    client.started = false;
    mac_hid_remote_ready(false);
    client.subscribed = false;
    ESP_LOGI(TAG, "HID client stopped: input_reports=%u total_notifications=%u", client.input_count, client.notification_count);
}

void hid_client_on_notify(const struct ble_gap_event *event)
{
    if (!client.listening || event->notify_rx.conn_handle != client.conn || !event->notify_rx.om) { return; }
    ++client.notification_count;
    const unsigned length = OS_MBUF_PKTLEN(event->notify_rx.om);
    const bool selected = client.input_handle && event->notify_rx.attr_handle == client.input_handle;
    ESP_LOGI(TAG, "GAP BLE_GAP_EVENT_NOTIFY_RX: handle=0x%04X len=%u indication=%u subscribed=%u selected=%u",
             event->notify_rx.attr_handle, length, event->notify_rx.indication, client.subscribed, selected);
    const hid_characteristic_t *report_info = NULL;
    for (unsigned i = 0; i < client.chr_count; ++i) {
        if (client.chars[i].chr.val_handle == event->notify_rx.attr_handle &&
            client.chars[i].report_type == INPUT_REPORT_TYPE) {
            report_info = &client.chars[i];
            break;
        }
    }
    if (report_info) {
        ESP_LOGI(TAG, "HID INPUT len=%u: Report ID=%u write_accepted=%u readback_confirmed=%u",
                 length, report_info->report_id, report_info->write_accepted, report_info->readback_confirmed);
    }
    if (selected) { ++client.input_count; }
    int rc = dump_mbuf(event->notify_rx.om, report_info ? report_info->report_id : -1,
                       event->notify_rx.attr_handle, event->notify_rx.indication);
    if (rc != 0) { ESP_LOGE(TAG, "notification buffer copy failed: status=%d (0x%X)", rc, (unsigned)rc); return; }
    if (report_info && report_info->report_id == 91 && length == 20) {
        uint8_t status[20];
        if (!os_mbuf_copydata(event->notify_rx.om, 0, sizeof(status), status)) {
            int mode = input_sensor_mode(status, sizeof(status));
            if (mode >= 0) { mac_hid_sensor_mode(mode == 1); }
        }
        return;
    }
    if (report_info && report_info->report_id == 3 && client.mouse_layout) {
        uint8_t raw[20], mouse[4];
        if ((length == 4 || length == 20) &&
            os_mbuf_copydata(event->notify_rx.om, 0, length, raw) == 0 &&
            input_mouse_decode(raw, length, mouse)) {
            mac_hid_remote_mouse(mouse);
        } else {
            ESP_LOGW(TAG, "ID 3 format differs from captured mouse layout; raw only");
        }
        return;
    }
    if (!selected || !client.keyboard_layout || length != KEYBOARD_REPORT_LENGTH) {
        ESP_LOGI(TAG, "Raw report only: expected mapped ID 1 with verified 8-byte keyboard layout");
        return;
    }
    uint8_t report[KEYBOARD_REPORT_LENGTH];
    if (os_mbuf_copydata(event->notify_rx.om, 0, sizeof(report), report) != 0) { return; }
    mac_hid_keyboard(report);
    static const uint8_t released[KEYBOARD_REPORT_LENGTH] = {0};
    if (memcmp(report, released, sizeof(report)) == 0) { ESP_LOGI(TAG, "KEY: RELEASE_ALL"); return; }
    ESP_LOGI(TAG, "KEYBOARD modifiers=0x%02X reserved=0x%02X", report[0], report[1]);
    for (unsigned i = 2; i < sizeof(report); ++i) {
        switch (report[i]) {
        case 0x00: break;
        case 0x4f: ESP_LOGI(TAG, "KEY: RIGHT"); break;
        case 0x50: ESP_LOGI(TAG, "KEY: LEFT"); break;
        case 0x51: ESP_LOGI(TAG, "KEY: DOWN"); break;
        case 0x52: ESP_LOGI(TAG, "KEY: UP"); break;
        default: ESP_LOGI(TAG, "KEY usage=0x%02X", report[i]); break;
        }
    }
}
