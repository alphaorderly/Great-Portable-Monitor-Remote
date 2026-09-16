/* Exercise the real Central callbacks with deterministic GAP completions. */
#include <assert.h>
#include <stdio.h>
#include "../../main/main.c"

extern bool test_bond_store_failure;
static const uint8_t TEST_ADDRESS[6] = {1, 2, 3, 4, 5, 6};
static bool saved_remote = true;
static unsigned scans,connects,security_requests,hid_starts,hid_stops,adverts;
static unsigned delay;
static bool scanning, bonded=true, secure=true;
static int connect_error,scan_error;
struct test_hs_cfg ble_hs_cfg;
void test_log(const char *fmt,...) { (void)fmt; }
void test_hex(const void *data,unsigned len) { (void)data; (void)len; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "stub"; }
const char *esp_get_idf_version(void) { return "test"; }
void esp_log_level_set(const char *tag,int level) { (void)tag; (void)level; }
int nvs_flash_init(void) { return 0; }
int nimble_port_init(void) { return 0; }
void nimble_port_run(void) {}
void nimble_port_freertos_init(void (*fn)(void *)) { (void)fn; }
void nimble_port_freertos_deinit(void) {}
void *nimble_port_get_dflt_eventq(void) { return NULL; }
void ble_store_config_init(void) {}
bool input_log_init(void) { return true; }
int usb_hid_init(void) { return 0; }
int usb_hid_start(void) { ++adverts; return 0; }
void usb_hid_on_reset(void) {}
void hid_client_start(uint16_t conn) { assert(conn==7); ++hid_starts; }
void hid_client_stop(uint16_t conn) { (void)conn; ++hid_stops; }
void hid_client_on_notify(const struct ble_gap_event *event) { (void)event; }
bool bridge_peer_load(const char *role,ble_addr_t *peer)
{ assert(!strcmp(role,"remote")); peer->type=BLE_ADDR_PUBLIC; memcpy(peer->val,TEST_ADDRESS,6); return saved_remote; }
bool bridge_peer_save(const char *role,const ble_addr_t *peer) { (void)role; (void)peer; return true; }
bool bridge_peer_equal(const ble_addr_t *a,const ble_addr_t *b)
{ return (a->type&1)==(b->type&1) && !memcmp(a->val,b->val,6); }
bool bridge_peer_bonded(const ble_addr_t *peer,bool central)
{ assert(central); return bonded && !memcmp(peer->val,TEST_ADDRESS,6); }
uint32_t ble_npl_time_ms_to_ticks32(uint32_t n) { return n; }
int ble_npl_callout_reset(struct ble_npl_callout *c,uint32_t n) { assert(c==&scan_retry); delay=n; return 0; }
void ble_npl_callout_stop(struct ble_npl_callout *c) { assert(c==&scan_retry); delay=0; }
int ble_npl_callout_init(struct ble_npl_callout *c,void *q,void (*fn)(struct ble_npl_event *),void *arg)
{ (void)c; (void)q; (void)fn; (void)arg; return 0; }
int ble_hs_util_ensure_addr(int privacy) { assert(!privacy); return 0; }
int ble_hs_id_infer_auto(int privacy,uint8_t *out) { assert(!privacy); *out=0; return 0; }
int ble_gap_disc_active(void) { return scanning; }
int ble_gap_disc(uint8_t type,int duration,const struct ble_gap_disc_params *p,int (*cb)(struct ble_gap_event *,void *),void *arg)
{ assert(!type && duration==30000 && p && cb==gap_callback && !arg); ++scans; scanning=!scan_error; return scan_error; }
int ble_gap_disc_cancel(void)
{
    scanning=false;
    struct ble_gap_event event={.type=BLE_GAP_EVENT_DISC_COMPLETE}; gap_callback(&event,NULL);
    return 0;
}
int ble_gap_connect(uint8_t type,const ble_addr_t *peer,int duration,const void *p,int (*cb)(struct ble_gap_event *,void *),void *arg)
{ assert(!type && peer && duration==10000 && !p && cb==gap_callback && !arg); ++connects; return connect_error; }
int ble_gap_security_initiate(uint16_t conn) { assert(conn==7); ++security_requests; return 0; }
int ble_gap_terminate(uint16_t conn,int reason) { assert(conn==7 && reason==19); return 0; }
int ble_gap_conn_find(uint16_t conn,struct ble_gap_conn_desc *desc)
{
    assert(conn==7); memset(desc,0,sizeof(*desc)); desc->conn_handle=7;
    desc->sec_state.encrypted=secure; desc->sec_state.bonded=bonded;
    memcpy(desc->peer_id_addr.val,TEST_ADDRESS,6); return 0;
}
int ble_store_read_peer_sec(const struct ble_store_key_sec *key,struct ble_store_value_sec *value)
{ (void)key; memset(value,0,sizeof(*value)); value->ltk_present=bonded; return bonded?0:1; }
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *f,const uint8_t *data,unsigned len)
{ (void)data; (void)len; memset(f,0,sizeof(*f)); return 0; }
uint16_t ble_uuid_u16(const ble_uuid_t *u) { return u->value; }
char *ble_uuid_to_str(const ble_uuid_t *u,char *out) { (void)u; out[0]=0; return out; }

static void awake(void)
{
    struct ble_gap_event event={.type=BLE_GAP_EVENT_DISC,.disc={.event_type=BLE_HCI_ADV_RPT_EVTYPE_DIR_IND}};
    memcpy(event.disc.addr.val,TEST_ADDRESS,6);
    gap_callback(&event,NULL);
}
int main(void)
{
    on_sync(); assert(scans==1 && scanning && have_remote_identity && adverts==1);
    /* Late wake after any number of 30-second windows. */
    struct ble_gap_event event={.type=BLE_GAP_EVENT_DISC_COMPLETE};
    scanning=false; gap_callback(&event,NULL); assert(delay==2000);
    start_scan(NULL); assert(scans==2);
    start_scan(NULL); assert(scans==2); /* No overlapping GAP procedures. */
    delay=0; awake(); assert(connects==1 && connection_attempted && !delay);
    awake(); assert(connects==1);
    event=(struct ble_gap_event){.type=BLE_GAP_EVENT_CONNECT,.connect={.status=1}};
    gap_callback(&event,NULL); assert(delay==2000 && !connection_attempted);
    start_scan(NULL); connect_error=2; awake(); assert(connects==2 && !connection_attempted && delay==2000);
    connect_error=0; start_scan(NULL); awake();
    event=(struct ble_gap_event){.type=BLE_GAP_EVENT_CONNECT,.connect={.conn_handle=7}};
    gap_callback(&event,NULL); assert(remote_conn==7 && security_requests==1 && !hid_starts);
    unsigned previous=scans; start_scan(NULL); assert(scans==previous);
    event=(struct ble_gap_event){.type=BLE_GAP_EVENT_ENC_CHANGE,.enc_change={.conn_handle=7}};
    test_bond_store_failure=true;
    gap_callback(&event,NULL); assert(hid_starts==0);
    test_bond_store_failure=false;
    gap_callback(&event,NULL); assert(hid_starts==1);
    event=(struct ble_gap_event){.type=BLE_GAP_EVENT_DISCONNECT,.disconnect={.conn={.conn_handle=7}}};
    gap_callback(&event,NULL); assert(remote_conn==BLE_HS_CONN_HANDLE_NONE && delay==2000 && hid_stops);
    start_scan(NULL); awake(); assert(connection_attempted);
    event=(struct ble_gap_event){.type=BLE_GAP_EVENT_CONNECT,.connect={.conn_handle=7}}; gap_callback(&event,NULL);
    event=(struct ble_gap_event){.type=BLE_GAP_EVENT_ENC_CHANGE,.enc_change={.conn_handle=7}}; gap_callback(&event,NULL);
    assert(hid_starts==2 && security_requests==2);
    scanning=false; on_reset(1); assert(!host_synced && !delay && remote_conn==BLE_HS_CONN_HANDLE_NONE);
    start_scan(NULL); previous=scans; on_sync(); assert(scans==previous+1);
    /* Never select an unidentified nameless device, even with a stored bond. */
    saved_remote=false; scanning=false; on_reset(1); on_sync(); previous=connects; awake(); assert(connects==previous);
    saved_remote=true;
    /* A saved identity alone is not sufficient without its bond. */
    bonded=false; scanning=false; on_reset(1); on_sync(); previous=connects; awake(); assert(connects==previous);
    scanning=false; scan_error=1; start_scan(NULL); assert(delay==2000 && !scanning);
    puts("PASS: late wake, bonded nameless/directed ADV, scan/connection failures, disconnect retry, resync and security gate");
}
