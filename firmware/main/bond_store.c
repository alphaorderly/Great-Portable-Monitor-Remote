#include "bond_store.h"
#include <string.h>
#include "esp_log.h"
#include "host/ble_hs.h"
#include "bridge_peers.h"

static const char *TAG = "BOND_STORE";
static ble_store_write_fn *write_record;
/* Failures live only until their connection ends; allow both security records
 * for all connections plus records restored from the bond store. */
static struct {
    bool used;
    int type;
    ble_addr_t peer;
} failures[2 * (CONFIG_BT_NIMBLE_MAX_BONDS + CONFIG_BT_NIMBLE_MAX_CONNECTIONS)];
static bool results_exhausted;
static struct { bool active; int type; ble_addr_t peer; } retry;

static bool security_type(int type)
{
    return type == BLE_STORE_OBJ_TYPE_OUR_SEC || type == BLE_STORE_OBJ_TYPE_PEER_SEC;
}

static const ble_addr_t *value_peer(int type, const union ble_store_value *value)
{
    if (security_type(type)) { return &value->sec.peer_addr; }
    if (type == BLE_STORE_OBJ_TYPE_CCCD) { return &value->cccd.peer_addr; }
    return NULL;
}

static void log_peer(const char *stage, int type, const ble_addr_t *peer, int rc)
{
    ESP_LOGI(TAG, "%s: type=%d peer=%02X:%02X:%02X:%02X:%02X:%02X addr_type=%u status=%d",
             stage, type, peer->val[5], peer->val[4], peer->val[3],
             peer->val[2], peer->val[1], peer->val[0], peer->type, rc);
}

static void security_result(int type, const ble_addr_t *peer, bool failed)
{
    if (!security_type(type)) { return; }
    unsigned free_slot = sizeof(failures) / sizeof(failures[0]);
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        if (!failures[i].used) { free_slot = i; }
        else if (failures[i].type == type && bridge_peer_equal(peer, &failures[i].peer)) {
            failures[i].used = failed;
            return;
        }
    }
    if (!failed) { return; }
    if (free_slot == sizeof(failures) / sizeof(failures[0])) {
        /* Never silently lose a persistence failure. Cleared on host reset. */
        results_exhausted = true;
        ESP_LOGE(TAG, "security result tracking exhausted");
        return;
    }
    failures[free_slot].used = true;
    failures[free_slot].type = type;
    failures[free_slot].peer = *peer;
}

bool bond_store_peer_ok(const ble_addr_t *peer)
{
    if (results_exhausted) { return false; }
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        if (failures[i].used && bridge_peer_equal(peer, &failures[i].peer)) { return false; }
    }
    return true;
}

void bond_store_clear_peer(const ble_addr_t *peer)
{
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        if (failures[i].used && bridge_peer_equal(peer, &failures[i].peer)) { failures[i].used = false; }
    }
}

void bond_store_reset_results(void)
{
    memset(failures, 0, sizeof(failures));
    results_exhausted = false;
    retry.active = false;
}

int bond_store_init(ble_store_write_fn *write)
{
    if (!write) { return BLE_HS_EINVAL; }
    write_record = write;
    bond_store_reset_results();
    return 0;
}

int bond_store_write(int type, const union ble_store_value *value)
{
    /* NimBLE calls this under its lock. Do not call GAP/store APIs here. */
    const ble_addr_t *peer = value_peer(type, value);
    const int rc = write_record(type, value);
    if (peer) {
        if (rc != BLE_HS_ESTORE_CAP) { security_result(type, peer, rc != 0); }
        if (retry.active && retry.type == type && bridge_peer_equal(peer, &retry.peer)) {
            log_peer(rc == 0 ? "retry succeeded" : "retry failed", type, peer, rc);
            retry.active = false;
        } else if (rc != 0 || security_type(type)) {
            log_peer(rc == BLE_HS_ESTORE_CAP ? "capacity reached" : "write result", type, peer, rc);
        }
    }
    return rc;
}

/* Return 0 only when positively known to be disconnected and unprotected.
 * Check both normalized and identity address forms against the live GAP table. */
static int protected_peer(const ble_addr_t *peer, const ble_addr_t *target)
{
    ble_addr_t remote;
    bool found;
    if (bridge_peer_equal(peer, target)) { return 1; }
    if (!bridge_peer_load_checked("remote", &remote, &found)) { return -BLE_HS_EUNKNOWN; }
    if (found && bridge_peer_equal(peer, &remote)) { return 1; }
    ble_addr_t address = *peer;
    address.type &= 1;
    for (unsigned i = 0; i < 2; ++i, address.type |= 2) {
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find_by_addr(&address, &desc);
        if (rc == 0) { return 1; }
        if (rc != BLE_HS_ENOTCONN) { return -rc; }
    }
    return 0;
}

typedef struct {
    ble_addr_t target, selected;
    bool found, selected_orphan;
    unsigned count, order, selected_order;
    int error;
} selection_t;

static int select_record(int type, union ble_store_value *value, void *arg)
{
    selection_t *s = arg;
    const ble_addr_t *peer = value_peer(type, value);
    ++s->count;
    ++s->order;
    int protected = protected_peer(peer, &s->target);
    if (protected < 0) { s->error = -protected; return s->error; }
    if (protected) { return 0; }
    unsigned order = s->order;
    bool orphan = false;
    if (security_type(type)) { order = value->sec.bond_count; }
    else {
        struct ble_store_key_sec key = {.peer_addr = *peer};
        struct ble_store_value_sec sec;
        key.peer_addr.type &= 1;
        int rc = ble_store_read_our_sec(&key, &sec);
        if (rc == BLE_HS_ENOENT) { rc = ble_store_read_peer_sec(&key, &sec); }
        if (rc == 0) { order = sec.bond_count; }
        else if (rc == BLE_HS_ENOENT) { orphan = true; }
        else { s->error = rc; return rc; }
    }
    /* Bonded CCCD owners use bond order; orphan owners follow in CCCD order.
     * Ties retain the first record returned by the store. */
    if (!s->found || orphan < s->selected_orphan ||
        (orphan == s->selected_orphan && order < s->selected_order)) {
        s->found = true;
        s->selected = *peer;
        s->selected.type &= 1;
        s->selected_orphan = orphan;
        s->selected_order = order;
    }
    return 0;
}

int bond_store_status(struct ble_store_status_event *event, void *arg)
{
    (void)arg;
    if (event->event_code == BLE_STORE_EVENT_FULL) {
        ESP_LOGW(TAG, "capacity advisory: type=%d handle=%u; continue without eviction",
                 event->full.obj_type, event->full.conn_handle);
        return 0;
    }
    if (event->event_code != BLE_STORE_EVENT_OVERFLOW) { return BLE_HS_EINVAL; }
    int type = event->overflow.obj_type;
    const ble_addr_t *peer = value_peer(type, event->overflow.value);
    if (!peer) {
        ESP_LOGE(TAG, "unsupported overflow: type=%d; records retained", type);
        return BLE_HS_ESTORE_CAP;
    }
    selection_t selection = {.target = *peer};
    const char *failure_stage = "final failure: candidate lookup";
    int rc = ble_store_iterate(type, select_record, &selection);
    if (!rc) { rc = selection.error; }
    if (!rc && !selection.found) {
        failure_stage = "final failure: no eligible peer";
        rc = BLE_HS_ESTORE_CAP;
    }
    if (!rc) {
        failure_stage = "final failure: protection recheck";
        int protected = protected_peer(&selection.selected, peer);
        if (protected) { rc = protected < 0 ? -protected : BLE_HS_ESTORE_CAP; }
    }
    if (!rc) {
        failure_stage = "final failure: protection recheck";
        int protected = protected_peer(&selection.selected, peer);
        if (protected) { rc = protected < 0 ? -protected : BLE_HS_ESTORE_CAP; }
    }
    if (!rc) {
        failure_stage = "final failure: peer deletion";
        log_peer("evict oldest eligible peer", type, &selection.selected, 0);
        rc = ble_store_util_delete_peer(&selection.selected);
    }
    int after = 0;
    if (!rc) {
        failure_stage = "final failure: capacity recount";
        rc = ble_store_util_count(type, &after);
    }
    if (!rc && (unsigned)after >= selection.count) {
        failure_stage = "final failure: no space freed";
        rc = BLE_HS_ESTORE_CAP;
    }
    if (rc) {
        security_result(type, peer, true);
        log_peer(failure_stage, type, peer, rc);
        return rc;
    }
    /* Each successful callback strictly decreases this store's record count.
     * Thus even repeated overflows terminate without an unbounded retry loop. */
    retry.active = true;
    retry.type = type;
    retry.peer = *peer;
    return 0;
}
