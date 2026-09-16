#pragma once
#include <stdint.h>
#include <stdbool.h>
#define CFG_TUD_HID_EP_BUFSIZE 128
#define TUD_CONFIG_DESC_LEN 9
#define TUD_HID_DESC_LEN 25
#define HID_ITF_PROTOCOL_NONE 0
#define TUD_CONFIG_DESCRIPTOR(...) 0
#define TUD_HID_DESCRIPTOR(...) 0
#define ESP_OK 0
#define ESP_MAC_WIFI_STA 0
typedef struct { int id; } tinyusb_event_t;
#define TINYUSB_EVENT_ATTACHED 0
#define TINYUSB_EVENT_DETACHED 1
typedef struct { void (*event_cb)(tinyusb_event_t *, void *); struct { const uint8_t *full_speed_config; const char **string; unsigned string_count; } descriptor; } tinyusb_config_t;
int tinyusb_driver_install(const tinyusb_config_t *);
