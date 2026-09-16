#pragma once
#include "tinyusb.h"
typedef enum { HID_REPORT_TYPE_INVALID, HID_REPORT_TYPE_INPUT, HID_REPORT_TYPE_OUTPUT, HID_REPORT_TYPE_FEATURE } hid_report_type_t;
bool tud_hid_ready(void);
bool tud_hid_report(uint8_t, const void *, uint16_t);
