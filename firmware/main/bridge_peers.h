#pragma once
#include <stdbool.h>
#include "host/ble_hs.h"

/* Role metadata only; cryptographic bonds remain owned by NimBLE. */
bool bridge_peer_load(const char *role, ble_addr_t *peer);
/* Distinguish an absent role from an NVS/read error for eviction decisions. */
bool bridge_peer_load_checked(const char *role, ble_addr_t *peer, bool *found);
bool bridge_peer_save(const char *role, const ble_addr_t *peer);
bool bridge_peer_forget(const char *role, const ble_addr_t *peer);
bool bridge_peer_equal(const ble_addr_t *a, const ble_addr_t *b);
bool bridge_peer_bonded(const ble_addr_t *peer, bool central);
