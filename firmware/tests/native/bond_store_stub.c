/* Link tests exercise their security gate; storage policy has its own suite. */
#include "../../main/bond_store.h"
bool test_bond_store_failure;
int bond_store_init(ble_store_write_fn *write)
{ (void)write; return 0; }
int bond_store_write(int type, const union ble_store_value *value)
{ (void)type; (void)value; return 0; }
int bond_store_status(struct ble_store_status_event *event, void *arg)
{ (void)event; (void)arg; return 0; }
bool bond_store_peer_ok(const ble_addr_t *peer)
{ (void)peer; return !test_bond_store_failure; }
void bond_store_clear_peer(const ble_addr_t *peer)
{ (void)peer; test_bond_store_failure = false; }
void bond_store_reset_results(void) { test_bond_store_failure = false; }
