/* Real policy and the SDK write/retry contract with deterministic stores. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/bond_store.c"

static union ble_store_value records[4][32];
static unsigned sizes[4], limits[4], sequence[4];
static ble_addr_t remote, connected[3];
static unsigned connected_count, deletes, writes, deleted_id;
static int iterate_error, read_error, connection_error, delete_error, count_error, write_error;
static bool no_delete, always_full, remote_read_error;
static bool fail_retry;

void test_log(const char *fmt, ...) { (void)fmt; }
bool bridge_peer_equal(const ble_addr_t *a, const ble_addr_t *b)
{ return (a->type & 1) == (b->type & 1) && !memcmp(a->val, b->val, 6); }
bool bridge_peer_load(const char *role, ble_addr_t *out)
{ assert(!strcmp(role, "remote")); *out = remote; return remote.val[0] != 0; }
bool bridge_peer_load_checked(const char *role, ble_addr_t *out, bool *found)
{ *found = bridge_peer_load(role, out); return !remote_read_error; }
int ble_gap_conn_find_by_addr(const ble_addr_t *peer, struct ble_gap_conn_desc *desc)
{
    (void)desc;
    if (connection_error) { return connection_error; }
    for (unsigned i = 0; i < connected_count; ++i) {
        /* Require exact type, proving policy checks identity variants too. */
        if (!memcmp(peer, &connected[i], sizeof(*peer))) { return 0; }
    }
    return BLE_HS_ENOTCONN;
}
int ble_store_iterate(int type, ble_store_iterator_fn *fn, void *arg)
{
    if (iterate_error) { return iterate_error; }
    for (unsigned i = 0; i < sizes[type]; ++i) {
        if (fn(type, &records[type][i], arg)) { break; }
    }
    return 0; /* SDK stops iteration on callback error but returns success. */
}
int ble_store_util_count(int type, int *out)
{ *out = (int)sizes[type]; return count_error; }
static int read_sec(int type, const struct ble_store_key_sec *key, struct ble_store_value_sec *out)
{
    if (read_error) { return read_error; }
    for (unsigned i = 0; i < sizes[type]; ++i) {
        if (bridge_peer_equal(&key->peer_addr, &records[type][i].sec.peer_addr)) {
            *out = records[type][i].sec; return 0;
        }
    }
    return BLE_HS_ENOENT;
}
int ble_store_read_our_sec(const struct ble_store_key_sec *key, struct ble_store_value_sec *out)
{ return read_sec(BLE_STORE_OBJ_TYPE_OUR_SEC, key, out); }
int ble_store_read_peer_sec(const struct ble_store_key_sec *key, struct ble_store_value_sec *out)
{ return read_sec(BLE_STORE_OBJ_TYPE_PEER_SEC, key, out); }
int ble_store_util_delete_peer(const ble_addr_t *peer)
{
    ++deletes; deleted_id = peer->val[0];
    assert(!bridge_peer_equal(peer, &remote));
    for (unsigned i = 0; i < connected_count; ++i) { assert(!bridge_peer_equal(peer, &connected[i])); }
    if (delete_error) { return delete_error; }
    if (no_delete) { return 0; }
    for (int type = 1; type <= 3; ++type) {
        for (unsigned i = 0; i < sizes[type];) {
            if (bridge_peer_equal(peer, value_peer(type, &records[type][i]))) {
                --sizes[type];
                memmove(&records[type][i], &records[type][i+1], (sizes[type]-i)*sizeof(records[type][i]));
            } else { ++i; }
        }
    }
    if (fail_retry) { write_error = BLE_HS_EUNKNOWN; }
    return 0;
}
static int underlying_write(int type, const union ble_store_value *value)
{
    ++writes;
    if (write_error) { return write_error; }
    if (always_full) { return BLE_HS_ESTORE_CAP; }
    unsigned i;
    for (i = 0; i < sizes[type]; ++i) {
        if (bridge_peer_equal(value_peer(type, value), value_peer(type, &records[type][i])) &&
            (type != 3 || records[type][i].cccd.chr_val_handle == value->cccd.chr_val_handle)) { break; }
    }
    if (i == sizes[type]) {
        if (sizes[type] >= limits[type]) { return BLE_HS_ESTORE_CAP; }
        ++sizes[type];
    }
    records[type][i] = *value;
    if (security_type(type)) { records[type][i].sec.bond_count = ++sequence[type]; }
    return 0;
}
static ble_addr_t address(unsigned id) { return (ble_addr_t){.val={(uint8_t)id}}; }
static union ble_store_value value(int type, unsigned id)
{
    union ble_store_value v = {0};
    if (security_type(type)) { v.sec.peer_addr = address(id); v.sec.ltk_present = true; }
    else { v.cccd.peer_addr = address(id); v.cccd.chr_val_handle = 10; }
    return v;
}
static int persist(int type, unsigned id)
{
    union ble_store_value v = value(type, id);
    for (unsigned attempts = 0; attempts < 40; ++attempts) {
        int rc = bond_store_write(type, &v);
        if (rc != BLE_HS_ESTORE_CAP) { return rc; }
        struct ble_store_status_event event = {.event_code=BLE_STORE_EVENT_OVERFLOW,
            .overflow={.obj_type=type, .value=&v}};
        rc = bond_store_status(&event, NULL);
        if (rc) { return rc; }
    }
    assert(!"unbounded retry"); return -1;
}
static void seed(int type, unsigned id)
{ union ble_store_value v = value(type,id); assert(!underlying_write(type,&v)); }
static void reset(void)
{
    memset(records,0,sizeof(records)); memset(sizes,0,sizeof(sizes)); memset(sequence,0,sizeof(sequence));
    limits[1]=limits[2]=4; limits[3]=16;
    remote=address(0); connected_count=deletes=writes=deleted_id=0;
    iterate_error=read_error=connection_error=delete_error=count_error=write_error=0;
    no_delete=always_full=remote_read_error=fail_retry=false;
    assert(!bond_store_init(underlying_write));
}
static void full_security(void)
{ for (unsigned i=1;i<=4;++i) { seed(1,i); seed(2,i); } }

int main(void)
{
    reset(); full_security();
    struct ble_store_status_event advisory={.event_code=BLE_STORE_EVENT_FULL,.full={.obj_type=1,.conn_handle=0}};
    assert(!bond_store_status(&advisory,NULL) && !deletes);
    assert(!persist(1,1) && !deletes); /* Existing record fits in full store. */
    assert(!persist(1,5) && deleted_id==2); /* Updated peer 1 is newest. */
    assert(!persist(2,5) && deletes==1); /* Both halves removed together. */

    reset(); full_security(); remote=address(1); connected[0]=address(2); connected[0].type=BLE_ADDR_PUBLIC_ID; connected_count=1;
    assert(!persist(2,5) && deleted_id==3 && sizes[1]==3 && sizes[2]==4);
    assert(bond_store_peer_ok(&remote));
    /* Selection uses bond age, not array position (including after restore). */
    reset(); full_security(); records[2][3].sec.bond_count=0;
    assert(!persist(2,5) && deleted_id==4);
    /* Peer-only records must be eligible; bonded_peers() cannot enumerate them. */
    reset(); for(unsigned i=1;i<=4;++i)seed(2,i);
    assert(!persist(2,5) && deleted_id==1);

    /* CCCD capacity only evicts owners of CCCDs, with associated bonds. */
    reset(); full_security(); limits[3]=2; seed(3,3); seed(3,2);
    assert(!persist(3,5) && deleted_id==2 && sizes[1]==3 && sizes[3]==2);
    reset(); limits[3]=2; seed(3,8); seed(3,9);
    assert(!persist(3,10) && deleted_id==8); /* Orphan CCCD insertion order. */
    reset(); seed(2,3); limits[3]=2; seed(3,8); seed(3,3);
    assert(!persist(3,10) && deleted_id==3); /* Peer-only bond before orphans. */
    /* Target is protected even when not in GAP; deleting it would lose its key. */
    reset(); seed(1,1); limits[3]=1; seed(3,1);
    union ble_store_value extra=value(3,1); extra.cccd.chr_val_handle=11;
    assert(bond_store_write(3,&extra)==BLE_HS_ESTORE_CAP);
    struct ble_store_status_event overflow={.event_code=BLE_STORE_EVENT_OVERFLOW,.overflow={.obj_type=3,.value=&extra}};
    assert(bond_store_status(&overflow,NULL)==BLE_HS_ESTORE_CAP && !deletes);

    reset(); full_security(); remote=address(1); connected_count=3;
    for(unsigned i=0;i<3;++i)connected[i]=address(i+2);
    assert(persist(1,5)==BLE_HS_ESTORE_CAP && !deletes);
    ble_addr_t target=address(5), other=address(2);
    assert(!bond_store_peer_ok(&target) && bond_store_peer_ok(&other));
    assert(persist(2,5)==BLE_HS_ESTORE_CAP); /* Same protected store also fails. */
    connected_count=0; assert(!persist(1,5));
    assert(!bond_store_peer_ok(&target)); /* Success must not clear other type failure. */
    assert(!persist(2,5) && bond_store_peer_ok(&target));

    reset(); full_security(); iterate_error=17;
    assert(persist(1,5)==17 && !deletes);
    reset(); full_security(); connection_error=17;
    assert(persist(1,5)==17 && !deletes);
    reset(); full_security(); remote_read_error=true;
    assert(persist(1,5)==BLE_HS_EUNKNOWN && !deletes);
    reset(); full_security(); delete_error=17;
    assert(persist(1,5)==17 && deletes==1 && sizes[1]==4);
    reset(); full_security(); no_delete=true;
    assert(persist(1,5)==BLE_HS_ESTORE_CAP && deletes==1);
    reset(); full_security(); count_error=17;
    assert(persist(1,5)==17 && deletes==1);
    reset(); seed(1,1); seed(3,1); limits[3]=1; read_error=17;
    assert(persist(3,5)==17 && !deletes);
    reset(); full_security(); always_full=true;
    assert(persist(1,5)==BLE_HS_ESTORE_CAP && deletes==4); /* Strictly bounded by records. */
    reset(); full_security(); fail_retry=true;
    assert(persist(1,5)==BLE_HS_EUNKNOWN && deletes==1 && !bond_store_peer_ok(&target));
    reset(); write_error=17;
    assert(persist(1,5)==17 && !bond_store_peer_ok(&target));
    target.type=BLE_ADDR_PUBLIC_ID; assert(!bond_store_peer_ok(&target));
    bond_store_clear_peer(&target); assert(bond_store_peer_ok(&target));
    overflow.overflow.obj_type=99;
    assert(bond_store_status(&overflow,NULL)==BLE_HS_ESTORE_CAP && !deletes);
    puts("PASS: storage advisory, protected FIFO, per-type capacity, CCCDs, bounded failures and peer security results");
}
