#include "hid_internal.h"
#include <string.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "bridge_peers.h"
#include "keymap.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
static const char *TAG = "HID_SERVICE";
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
static bool schema_checked, map_read;
void hid_service_update_schema(void)
{
    if (!host_link_state()->encrypted || !host_link_state()->have_identity) { return; }
    if (map_read && native[0].subscribed && native[1].subscribed && native[2].subscribed) {
        bridge_peer_save("hid_v3", &host_link_state()->identity);
        schema_checked = true;
    } else if (!schema_checked) {
        ble_addr_t previous;
        if (!bridge_peer_load("hid_v3", &previous) || !bridge_peer_equal(&previous, &host_link_state()->identity)) {
            ESP_LOGI(TAG, "HID schema migration: send Service Changed once for this connection");
            ble_svc_gatt_changed(1, 0xffff);
        }
        schema_checked = true;
    }
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
            if (!rc && conn == host_link_state()->conn) { map_read = true; hid_service_update_schema(); }
            return rc;
        }
        case 0x2a4a: {
            const uint8_t info[] = {0x11, 0x01, 0x00, 0x02}; /* HID 1.11, normally connectable */
            return append(ctxt->om, info, sizeof(info));
        }
        case 0x2a4d: return r ? append(ctxt->om, r->data, r->length) : append(ctxt->om, hid_input_vendor_report(), REPORT_LENGTH);
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
        hid_input_suspend(value == 0);
        ESP_LOGI(TAG, "host suspend=%u", value == 0);
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
    if (conn != host_link_state()->conn || !host_link_state()->encrypted) { return BLE_ATT_ERR_INSUFFICIENT_AUTHEN; }
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
        hid_input_mapping_changed();
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


void hid_service_connected(void) { schema_checked = map_read = false; }
void hid_service_schema_reset(void) { schema_checked = false; }
int hid_service_init(void)
{
    keymap_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (!rc) { rc = ble_gatts_count_cfg(services); }
    return rc ? rc : ble_gatts_add_svcs(services);
}
