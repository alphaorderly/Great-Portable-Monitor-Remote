#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "host/ble_hs.h"
#include "mac_hid.h"

#define DEVICE_NAME "ESP32 Remote Bridge"
#define REPORT_LENGTH 12
#define REPORT_ID 1
#define FLAG_REMOTE_READY 1
#define FLAG_INPUT_EVENT 2

/* All interfaces run on the NimBLE host task. Link state has one owner. */
typedef struct {
    uint16_t conn;
    bool encrypted;
    ble_addr_t identity;
    bool have_identity;
} host_link_context_t;
const host_link_context_t *host_link_state(void);
int host_link_init(void);
int host_link_advertise(uint8_t own_addr_type);
void host_link_reset(void);

typedef struct {
    uint8_t id, length, data[8];
    uint16_t handle;
    bool subscribed, dirty;
} native_report_t;
/* Input owns these report slots; GATT registration binds their handles. */
extern native_report_t native[3];
extern uint16_t input_handle;
int hid_input_init(void);
int hid_input_start(void);
void hid_input_reset(void);
void hid_input_connected(void);
void hid_input_publish(void);
void hid_input_subscribe(uint16_t handle, bool enabled);
void hid_input_tx_failed(uint16_t handle);
void hid_input_suspend(bool enabled);
void hid_input_mapping_changed(void);
const uint8_t *hid_input_vendor_report(void);
int hid_service_init(void);
void hid_service_connected(void);
void hid_service_update_schema(void);
void hid_service_schema_reset(void);
