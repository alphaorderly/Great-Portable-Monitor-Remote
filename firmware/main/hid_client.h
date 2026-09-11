#pragma once

#include <stdint.h>
#include "host/ble_gap.h"

/* Single-connection diagnostic client; called on the NimBLE host task. */
void hid_client_start(uint16_t conn_handle);
void hid_client_stop(uint16_t conn_handle);
void hid_client_on_notify(const struct ble_gap_event *event);
