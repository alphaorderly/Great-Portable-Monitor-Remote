#pragma once
#include <stdbool.h>
#include "host/ble_store.h"

/* Initialize after ble_store_config_init. Callbacks run on the NimBLE host task. */
int bond_store_init(ble_store_write_fn *write);
int bond_store_write(int type, const union ble_store_value *value);
int bond_store_status(struct ble_store_status_event *event, void *arg);
bool bond_store_peer_ok(const ble_addr_t *peer);
void bond_store_clear_peer(const ble_addr_t *peer);
void bond_store_reset_results(void);
