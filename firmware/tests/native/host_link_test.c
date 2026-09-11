/* Link behavior through module interfaces and captured GAP callbacks, no radio. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../../main/hid_internal.h"
#include "host/ble_hs_adv.h"
#include "host/ble_store.h"

native_report_t native[3] = {{.id=2,.handle=20},{.id=3,.handle=21},{.id=4,.handle=22}};
uint16_t input_handle=10;
static uint32_t clock_ms;
static bool active, exists, secured, bonded=true, saved=true, metadata_error;
static int terminate_error, delete_error;
static unsigned closes, deletes, guidance, delayed, faults, publications;
static int adv_duration, adv_mode;
static int (*gap)(struct ble_gap_event *,void *);
static void *token;
static unsigned timer_inits;
static void (*advertise_timer_fn)(struct ble_npl_event *);
static const ble_addr_t peer={.val={42}};
void test_log(const char *fmt,...)
{
    if(strstr(fmt,"Repeated host security"))++guidance;
    if(strstr(fmt,"HID subscription delayed"))++delayed;
    if(strstr(fmt,"phase=%u")) {
        va_list args; va_start(args,fmt); if(va_arg(args,unsigned)==HOST_LINK_FAULT)++faults; va_end(args);
    }
}
uint32_t ble_npl_time_get(void) { return clock_ms; }
uint32_t ble_npl_time_ticks_to_ms32(uint32_t n) { return n; }
uint32_t ble_npl_time_ms_to_ticks32(uint32_t n) { return n; }
int ble_npl_callout_reset(struct ble_npl_callout *c,uint32_t n) { (void)c;(void)n;return 0; }
void ble_npl_callout_stop(struct ble_npl_callout *c) { (void)c; }
int ble_npl_callout_init(struct ble_npl_callout *c,void *q,void (*fn)(struct ble_npl_event *),void *a)
{ (void)c;(void)q;(void)a;if(++timer_inits==2)advertise_timer_fn=fn;return 0; }
void *nimble_port_get_dflt_eventq(void) { return NULL; }
bool bridge_peer_equal(const ble_addr_t *a,const ble_addr_t *b) { return (a->type&1)==(b->type&1)&&!memcmp(a->val,b->val,6); }
bool bridge_peer_load(const char *role,ble_addr_t *out) { *out=peer; return saved&&strcmp(role,"remote"); }
bool bridge_peer_bonded(const ble_addr_t *p,bool central) { (void)p;assert(!central);return bonded; }
bool bridge_peer_save(const char *r,const ble_addr_t *p) { (void)r;assert(bridge_peer_equal(p,&peer));saved=true;return true; }
bool bridge_peer_forget(const char *r,const ble_addr_t *p) { (void)r;assert(bridge_peer_equal(p,&peer));return !metadata_error; }
int ble_store_util_delete_peer(const ble_addr_t *p) { assert(bridge_peer_equal(p,&peer));++deletes;return delete_error; }
int ble_store_util_bonded_peers(ble_addr_t *p,int *n,int max) { (void)p;(void)max;*n=0;return 0; }
int ble_store_read_cccd(const struct ble_store_key_cccd *k,struct ble_store_value_cccd *v) { (void)k;(void)v;return 1; }
int ble_gap_conn_find(uint16_t conn,struct ble_gap_conn_desc *d)
{
    assert(conn==7); if(!exists)return BLE_HS_ENOTCONN;
    *d=(struct ble_gap_conn_desc){.conn_handle=7,.peer_id_addr=peer,.sec_state={.encrypted=secured,.bonded=bonded}};return 0;
}
int ble_gap_terminate(uint16_t conn,int reason) { assert(conn==7&&reason==BLE_ERR_REM_USER_CONN_TERM);++closes;return terminate_error; }
int ble_gap_adv_active(void) { return active; }
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *f) { (void)f;return 0; }
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *f) { (void)f;return 0; }
int ble_gap_adv_start(uint8_t a,const ble_addr_t *p,int duration,const struct ble_gap_adv_params *params,int (*cb)(struct ble_gap_event *,void *),void *arg)
{ (void)a;(void)p;active=true;adv_duration=duration;adv_mode=params->conn_mode;gap=cb;token=arg;return 0; }
void hid_input_connected(void) { for(unsigned i=0;i<3;++i)native[i].subscribed=false; }
void hid_input_reset(void) { hid_input_connected(); }
int hid_input_start(void) { return 0; }
void hid_input_publish(void) { ++publications; }
void hid_input_subscribe(uint16_t handle,bool enabled) { for(unsigned i=0;i<3;++i)if(native[i].handle==handle)native[i].subscribed=enabled; }
void hid_input_tx_failed(uint16_t handle) { (void)handle; }
void hid_service_connected(void) {}
void hid_service_update_schema(void) {}
void hid_service_schema_reset(void) {}

static void start(void)
{
    host_link_reset();active=false;exists=false;secured=false;bonded=true;
    terminate_error=delete_error=0;metadata_error=false;
    assert(host_link_advertise(0)==0);
    exists=true;active=false;
    struct ble_gap_event e={.type=BLE_GAP_EVENT_CONNECT,.connect={.conn_handle=7}};gap(&e,token);
    assert(host_link_state()->phase==HOST_LINK_WAIT_SECURITY);
}
static void advance(uint32_t n)
{ clock_ms+=n;host_link_tick(host_link_state()->generation,clock_ms); }
static void encrypt(int status)
{
    secured=status==0;
    struct ble_gap_event e={.type=BLE_GAP_EVENT_ENC_CHANGE,.enc_change={.conn_handle=7,.status=status}};gap(&e,token);
}
static void subscribe(uint16_t conn,uint16_t handle,void *generation)
{
    struct ble_gap_event e={.type=BLE_GAP_EVENT_SUBSCRIBE,.subscribe={.conn_handle=conn,.attr_handle=handle,.cur_notify=true}};gap(&e,generation);
}
int main(void)
{
    assert(host_link_init()==0);active=false;assert(host_link_advertise(0)==0);
    assert(adv_mode==BLE_GAP_CONN_MODE_DIR&&adv_duration==3000);
    active=false;
    struct ble_gap_event ended={.type=BLE_GAP_EVENT_ADV_COMPLETE};gap(&ended,token);
    advertise_timer_fn(NULL);
    assert(adv_mode==BLE_GAP_CONN_MODE_UND&&adv_duration==BLE_HS_FOREVER);
    start();unsigned before=closes;advance(14999);assert(closes==before);advance(1);
    assert(closes==before+1&&host_link_state()->phase==HOST_LINK_CLOSING);
    exists=false;advance(1000);assert(host_link_state()->conn==BLE_HS_CONN_HANDLE_NONE);

    start();before=closes;secured=true;advance(15000);
    assert(closes==before&&host_link_state()->encrypted); /* Stack/event race at deadline. */
    start();before=closes;advance(14999);encrypt(0);advance(1);assert(closes==before);
    advance(10000);assert(delayed==1&&host_link_state()->encrypted);
    subscribe(99,20,token);assert(!native[0].subscribed);
    subscribe(7,20,token);assert(host_link_state()->phase==HOST_LINK_PARTIAL);
    subscribe(7,21,token);subscribe(7,22,token);assert(host_link_state()->phase==HOST_LINK_READY);

    uint32_t stale_generation=host_link_state()->generation;void *old_token=token;
    start();before=closes;host_link_tick(stale_generation,clock_ms+60000);
    subscribe(7,20,old_token);
    struct ble_gap_event stale={.type=BLE_GAP_EVENT_DISCONNECT,.disconnect={.conn={.conn_handle=7}}};gap(&stale,old_token);
    assert(closes==before&&!native[0].subscribed&&host_link_state()->conn==7);
    advance(14999);assert(closes==before);advance(1);assert(closes==before+1);

    start();terminate_error=BLE_HS_EAPP;before=closes;encrypt(1);
    advance(1000);advance(1000);advance(1000);advance(100000);
    assert(closes==before+4&&host_link_state()->phase==HOST_LINK_FAULT&&faults==1);
    assert(!deletes);

    start();encrypt(0); /* A healthy session resets the peer failure streak. */
    unsigned old_guidance=guidance;
    for(unsigned i=0;i<3;++i){start();encrypt(1);}
    assert(guidance==old_guidance+1&&!deletes);
    start();encrypt(1);assert(guidance==old_guidance+1);

    start();struct ble_gap_event repair={.type=BLE_GAP_EVENT_REPEAT_PAIRING,.repeat_pairing={.conn_handle=7,.cur_key_size=16,.new_key_size=16,.new_bonding=1}};
    metadata_error=true;assert(gap(&repair,token)==BLE_GAP_REPEAT_PAIRING_IGNORE&&!deletes);
    start();delete_error=1;assert(gap(&repair,token)==BLE_GAP_REPEAT_PAIRING_IGNORE&&deletes==1);
    assert(gap(&repair,token)==BLE_GAP_REPEAT_PAIRING_IGNORE&&deletes==1);
    start();assert(gap(&repair,token)==BLE_GAP_REPEAT_PAIRING_RETRY&&deletes==2);
    assert(gap(&repair,token)==BLE_GAP_REPEAT_PAIRING_IGNORE&&deletes==2);
    encrypt(0);assert(host_link_state()->encrypted);
    puts("PASS: link deadlines, partial subscriptions, stale generation/handle guards, bounded close retries, per-peer failure guidance and repair failures");
}
