/* Actual peripheral callbacks with a fake transport; no radio/OS claims. */
#include <assert.h>
#include <stdio.h>
#define TAG input_tag
#include "../../main/hid_input.c"
#undef TAG
#define TAG service_tag
#include "../../main/hid_service.c"
#undef TAG
#define TAG link_tag
#include "../../main/host_link.c"
#undef TAG
#include "../../main/mac_hid.c"

static struct { uint16_t handle; uint8_t data[12]; unsigned len; } sent[256];
static unsigned sent_count, changed_count;
static int send_error;
static int mbuf_used;
static bool allocation_error;
static unsigned allocations;
int os_msys_count(void) { return 36; }
int os_msys_num_free(void) { return 36 - mbuf_used; }
static unsigned security_requests, adv_calls;
static bool adv_active;
static int adv_mode, adv_duration;
static uint32_t adv_delay;
static ble_addr_t saved_peer;
static bool saved_mac, saved_schema;
static bool peer_encrypted=true, bond_present=true, remote_matches, forget_error;
static int delete_error, conn_find_error;
static unsigned deleted_peers, terminated;
bool bridge_peer_equal(const ble_addr_t *a,const ble_addr_t *b) { return !memcmp(a,b,sizeof(*a)); }
bool bridge_peer_load(const char *role,ble_addr_t *peer)
{ *peer=saved_peer; return !strcmp(role,"remote") ? remote_matches : !strcmp(role,"mac") ? saved_mac : saved_schema; }
bool bridge_peer_save(const char *role,const ble_addr_t *peer)
{ saved_peer=*peer; if(!strcmp(role,"mac"))saved_mac=true; else saved_schema=true; return true; }
bool bridge_peer_forget(const char *role,const ble_addr_t *peer)
{ assert(!strcmp(role,"mac") || !strcmp(role,"hid_v3")); if(forget_error)return false;
  if(bridge_peer_equal(peer,&saved_peer)) { if(!strcmp(role,"mac"))saved_mac=false; else saved_schema=false; } return true; }
bool bridge_peer_bonded(const ble_addr_t *peer,bool central) { (void)peer; (void)central; return bond_present; }
int ble_store_util_delete_peer(const ble_addr_t *peer)
{ assert(peer->type==BLE_ADDR_PUBLIC && peer->val[0]==42 && !remote_matches); ++deleted_peers; return delete_error; }
int ble_store_read_cccd(const struct ble_store_key_cccd *key,struct ble_store_value_cccd *value)
{ (void)key; (void)value; return 1; }
int ble_store_util_bonded_peers(ble_addr_t *peers,int *count,int max)
{ (void)peers; (void)max; *count=0; return 0; }
int ble_gap_adv_active(void) { return adv_active; }
static struct os_mbuf buffer;
static uint8_t buffer_data[512];
void test_log(const char *fmt,...) { (void)fmt; }
uint16_t ble_uuid_u16(const ble_uuid_t *u) { return u->value; }
int os_mbuf_copydata(const struct os_mbuf *om,int off,int len,void *out)
{ assert(off>=0 && len>=0 && (unsigned)(off+len)<=om->len); memcpy(out,om->data+off,len); return 0; }
int os_mbuf_append(struct os_mbuf *om,const void *data,unsigned len)
{ assert(om==&buffer && om->len+len<=sizeof(buffer_data)); memcpy(buffer_data+om->len,data,len); om->len+=len; return 0; }
struct os_mbuf *ble_hs_mbuf_from_flat(const void *data,uint16_t len)
{ ++allocations; if (allocation_error) return NULL; buffer=(struct os_mbuf){buffer_data,len}; memcpy(buffer_data,data,len); return &buffer; }
int ble_gatts_notify_custom(uint16_t conn,uint16_t handle,struct os_mbuf *om)
{
    assert(conn==7 && om->len<=12 && sent_count<256);
    if(send_error) return send_error;
    sent[sent_count].handle=handle; sent[sent_count].len=om->len;
    memcpy(sent[sent_count++].data,om->data,om->len); return 0;
}
int ble_npl_callout_reset(struct ble_npl_callout *c,uint32_t n)
{ if(c==&advertise_retry)adv_delay=n; else assert(n==(c==&mouse_timer?MOUSE_PERIOD_MS:1000)); return 0; }
void ble_npl_callout_stop(struct ble_npl_callout *c) { if(c==&advertise_retry)adv_delay=0; }
uint32_t ble_npl_time_ms_to_ticks32(uint32_t n) { return n; }
int ble_npl_callout_init(struct ble_npl_callout *c,void *q,void (*fn)(struct ble_npl_event *),void *arg)
{ (void)c; (void)q; (void)fn; (void)arg; return 0; }
void *nimble_port_get_dflt_eventq(void) { return NULL; }
int ble_gap_security_initiate(uint16_t conn) { assert(conn==7); ++security_requests; return 0; }
int ble_gap_terminate(uint16_t conn,int reason) { assert(conn==7 && reason==BLE_ERR_REM_USER_CONN_TERM); ++terminated; return 0; }
int ble_gap_conn_find(uint16_t conn,struct ble_gap_conn_desc *desc)
{ assert(conn==7); memset(desc,0,sizeof(*desc)); desc->peer_id_addr.val[0]=42; desc->sec_state.encrypted=peer_encrypted; desc->sec_state.bonded=true; return conn_find_error; }
void ble_svc_gatt_changed(uint16_t start,uint16_t end) { assert(start==1 && end==0xffff); ++changed_count; }
void ble_svc_gap_init(void) {}
void ble_svc_gatt_init(void) {}
int ble_svc_gap_device_name_set(const char *s) { (void)s; return 0; }
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *s) { (void)s; return 0; }
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *s) { (void)s; return 0; }
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *f) { (void)f; return 0; }
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *f) { (void)f; return 0; }
int ble_gap_adv_start(uint8_t type,const ble_addr_t *peer,int duration,const struct ble_gap_adv_params *p,int (*cb)(struct ble_gap_event *,void *),void *arg)
{ (void)type; (void)cb; (void)arg; ++adv_calls; adv_active=true; adv_mode=p->conn_mode; adv_duration=duration;
  assert((peer!=NULL)==(adv_mode==BLE_GAP_CONN_MODE_DIR)); return 0; }

static void descriptor_check(void)
{
    /* Walk actual HID items and count input bits for each Report ID. */
    unsigned id=0,size=0,count=0,bits[6]={0},feature_bits[6]={0},collections=0;
    for(unsigned off=0; off<sizeof(report_map);) {
        uint8_t prefix=report_map[off++]; unsigned n=prefix&3; if(n==3)n=4;
        assert(off+n<=sizeof(report_map)); unsigned value=0;
        for(unsigned j=0;j<n;++j) value|=(unsigned)report_map[off++]<<(8*j);
        switch(prefix&0xfc) {
        case 0x84: id=value; assert(id>0 && id<6); break;
        case 0x74: size=value; break;
        case 0x94: count=value; break;
        case 0x80: bits[id]+=size*count; break;
        case 0xb0: feature_bits[id]+=size*count; break;
        case 0xa0: ++collections; break;
        case 0xc0: assert(collections); --collections; break;
        }
    }
    assert(!collections && bits[1]==96 && bits[2]==64 && bits[3]==32 && bits[4]==8);
    assert(!bits[5] && feature_bits[5]==512);
    for(unsigned i=0;i<4;++i) {
        const struct ble_gatt_chr_def *chr=&services[0].characteristics[3+i];
        buffer=(struct os_mbuf){buffer_data,0};
        struct ble_gatt_access_ctxt ctxt={.op=BLE_GATT_ACCESS_OP_READ_DSC,.om=&buffer};
        assert(chr->descriptors[0].access_cb(7,0,&ctxt,chr->descriptors[0].arg)==0);
        assert(buffer.len==2 && buffer.data[0]==i+1 && buffer.data[1]==1);
    }
    buffer=(struct os_mbuf){buffer_data,0};
    struct ble_gatt_access_ctxt ctxt={.op=BLE_GATT_ACCESS_OP_READ_DSC,.om=&buffer};
    assert(access_keymap(7,0,&ctxt,NULL)==0 && buffer.len==2 && buffer.data[0]==5 && buffer.data[1]==3);
}


static void mouse_flow_check(void)
{
    link.conn=7; link.encrypted=true; suspended=false; subscribed=false;
    for(unsigned i=0;i<3;++i) { native[i].subscribed=true; native[i].dirty=false; }
    mouse_count=0; memset(native[1].data,0,8); sent_count=0;
    uint8_t motion[4]={0,3,0xfe,0};
    unsigned before_alloc=allocations;
    for(unsigned i=0;i<10;++i)mac_hid_mouse(motion);
    assert(allocations==before_alloc && !sent_count && mouse_count==1);
    on_mouse_tick(NULL);
    assert(sent_count==1 && sent[0].data[1]==30 && (int8_t)sent[0].data[2]==-20);
    on_mouse_tick(NULL); assert(sent_count==1); /* No replay of consumed motion. */

    /* Press + release within one tick must be delivered in order. */
    uint8_t button[4]={1,0,0,0}; mac_hid_mouse(button);
    button[0]=0; mac_hid_mouse(button);
    assert(mouse_count==2); on_mouse_tick(NULL); on_mouse_tick(NULL);
    assert(sent_count==3 && sent[1].data[0]==1 && sent[2].data[0]==0);

    /* Saturated link: no allocation, bounded work, release survives recovery. */
    mbuf_used=HID_MBUF_MAX_USED; before_alloc=allocations;
    for(unsigned i=0;i<10000;++i) {
        motion[0]=i&1; mac_hid_mouse(motion);
        if(i%2==0)on_mouse_tick(NULL);
        assert(mouse_count<=MOUSE_QUEUE_SIZE);
    }
    button[0]=0; mac_hid_mouse(button); on_mouse_tick(NULL);
    assert(allocations==before_alloc && native[1].dirty && !mouse_count);
    mbuf_used=0; on_mouse_tick(NULL);
    assert(sent_count==4 && !memcmp(sent[3].data,"\0\0\0\0",4));
    on_mouse_tick(NULL); assert(sent_count==4);

    /* Allocation/send failures discard stale deltas, retry only latest buttons. */
    allocation_error=true; motion[0]=1; mac_hid_mouse(motion); on_mouse_tick(NULL);
    assert(!mouse_count && native[1].dirty);
    allocation_error=false; on_mouse_tick(NULL);
    assert(sent_count==5 && sent[4].data[0]==1 && !sent[4].data[1] && !sent[4].data[2]);
    send_error=1; motion[0]=0; mac_hid_mouse(motion); on_mouse_tick(NULL);
    send_error=0; on_mouse_tick(NULL);
    assert(sent_count==6 && !memcmp(sent[5].data,"\0\0\0\0",4));

    /* Bound the button queue too, and converge to release when it overflows. */
    for(unsigned i=0;i<20;++i) { button[0]=(i+1)&1; mac_hid_mouse(button); }
    assert(mouse_count<=MOUSE_QUEUE_SIZE);
    while(mouse_count)on_mouse_tick(NULL);
    assert(sent[sent_count-1].data[0]==0);
    mac_hid_mouse(motion); mac_hid_remote_ready(false);
    assert(!mouse_count && !native[1].data[0]);
    mac_hid_mouse(motion); mac_hid_on_reset(); assert(!mouse_count);
}

static void bond_recovery_check(void)
{
    adv_active=false;
    assert(mac_hid_advertise(0)==0);
    struct ble_gap_event connect={.type=BLE_GAP_EVENT_CONNECT,.connect={.conn_handle=7}};
    struct ble_gap_event repair={.type=BLE_GAP_EVENT_REPEAT_PAIRING,.repeat_pairing={
        .conn_handle=7,.cur_key_size=16,.new_key_size=16,.new_bonding=1}};
    mac_gap_event(&connect,NULL);
    saved_peer=(ble_addr_t){.val={42}}; saved_mac=saved_schema=true;
    /* Healthy link.encrypted links and unrelated handles never lose their bond. */
    assert(mac_gap_event(&repair,NULL)==BLE_GAP_REPEAT_PAIRING_IGNORE && !deleted_peers);
    peer_encrypted=false;
    repair.repeat_pairing.conn_handle=8;
    assert(mac_gap_event(&repair,NULL)==BLE_GAP_REPEAT_PAIRING_IGNORE && !deleted_peers);
    repair.repeat_pairing.conn_handle=7;
    remote_matches=true;
    assert(mac_gap_event(&repair,NULL)==BLE_GAP_REPEAT_PAIRING_IGNORE && !deleted_peers);
    remote_matches=false;
    repair.repeat_pairing.new_key_size=7;
    assert(mac_gap_event(&repair,NULL)==BLE_GAP_REPEAT_PAIRING_IGNORE && !deleted_peers);
    repair.repeat_pairing.new_key_size=16;
    repair.repeat_pairing.cur_sc=1;
    assert(mac_gap_event(&repair,NULL)==BLE_GAP_REPEAT_PAIRING_IGNORE && !deleted_peers);
    repair.repeat_pairing.cur_sc=0;
    forget_error=true;
    assert(mac_gap_event(&repair,NULL)==BLE_GAP_REPEAT_PAIRING_IGNORE && !deleted_peers);
    forget_error=false;
    mac_gap_event(&connect,NULL);
    delete_error=1;
    assert(mac_gap_event(&repair,NULL)==BLE_GAP_REPEAT_PAIRING_IGNORE && deleted_peers==1);
    assert(mac_gap_event(&repair,NULL)==BLE_GAP_REPEAT_PAIRING_IGNORE && deleted_peers==1);
    mac_gap_event(&connect,NULL);
    delete_error=0; saved_mac=saved_schema=true; link.have_identity=true; link.identity=saved_peer;
    assert(mac_gap_event(&repair,NULL)==BLE_GAP_REPEAT_PAIRING_RETRY && deleted_peers==2);
    assert(!saved_mac && !saved_schema && !link.have_identity);
    assert(mac_gap_event(&repair,NULL)==BLE_GAP_REPEAT_PAIRING_IGNORE && deleted_peers==2);
    /* Freshly bonded Mac saves its identity again; next boot can target it. */
    peer_encrypted=true;
    struct ble_gap_event secured={.type=BLE_GAP_EVENT_ENC_CHANGE,.enc_change={.conn_handle=7}};
    mac_gap_event(&secured,NULL);
    assert(link.encrypted && saved_mac && link.have_identity);
    assert(mac_gap_event(&repair,NULL)==BLE_GAP_REPEAT_PAIRING_IGNORE && deleted_peers==2);
    unsigned old_terminated=terminated;
    secured.enc_change.conn_handle=8; secured.enc_change.status=1;
    mac_gap_event(&secured,NULL); assert(terminated==old_terminated && link.encrypted);
    secured.enc_change.conn_handle=7;
    mac_gap_event(&secured,NULL); assert(!link.encrypted && terminated==old_terminated+1 && deleted_peers==2);
    /* A missing persisted LTK must not be reported as a usable bonded link. */
    secured.enc_change.status=0; bond_present=false;
    mac_gap_event(&secured,NULL); assert(!link.encrypted && terminated==old_terminated+2);
    bond_present=true;
    struct ble_gap_event disconnect={.type=BLE_GAP_EVENT_DISCONNECT};
    mac_gap_event(&disconnect,NULL); assert(adv_delay==1000);
    adv_active=false; retry_advertising(NULL); assert(adv_mode==BLE_GAP_CONN_MODE_DIR);
    assert(!security_requests);
    puts("PASS: Mac stale-bond recovery, remote/healthy-bond protection, no downgrade, storage failure, encryption failure and advertising retry");
}

/* Replays the meaningful edges captured from the user's two-mode sequence.
 * In particular STOP -> LEFT -> START -> LEFT+6A must remain a right click. */
static void mode_mapping_check(void)
{
    mac_hid_remote_ready(false);
    uint8_t key[8]={0,0,0x50}, mouse[4]={0};
    mac_hid_keyboard(key); assert(native[0].data[2]==0x50 && !native[1].data[0]);
    key[2]=0; mac_hid_keyboard(key);
    key[2]=0x6a; mac_hid_keyboard(key); mac_hid_sensor_mode(true);
    assert(cursor_mode && mode_known);
    key[2]=0; mac_hid_keyboard(key);
    mac_hid_remote_mouse(mouse);
    mac_hid_sensor_mode(false); /* Sensor pauses automatically before LEFT. */
    assert(cursor_mode);
    key[2]=0x50; mac_hid_keyboard(key);
    assert(!native[0].data[2] && native[1].data[0]==2);
    mac_hid_sensor_mode(true); assert(native[1].data[0]==2);
    key[3]=0x6a; mac_hid_keyboard(key); assert(cursor_mode && native[1].data[0]==2);
    key[3]=0; mac_hid_keyboard(key);
    /* Native OK plus mapped LEFT can be held and released independently. */
    mouse[0]=1; mac_hid_remote_mouse(mouse); assert(native[1].data[0]==3);
    key[2]=0; mac_hid_keyboard(key); assert(native[1].data[0]==1);
    mouse[0]=0; mac_hid_remote_mouse(mouse); assert(!native[1].data[0]);
    /* Explicit cursor-off (standalone 6A then STOP) releases all outputs. */
    key[2]=0x50; mac_hid_keyboard(key); assert(native[1].data[0]==2);
    key[2]=0x6a; mac_hid_keyboard(key); mac_hid_sensor_mode(false);
    assert(!cursor_mode && !native[1].data[0] && !native[0].data[2]);
    mouse[0]=1; mac_hid_remote_mouse(mouse); assert(!native[1].data[0]); /* Trailing mouse ignored. */
    key[2]=0; mac_hid_keyboard(key);
    key[2]=0x50; mac_hid_keyboard(key); assert(native[0].data[2]==0x50 && !native[1].data[0]);
    mac_hid_remote_ready(false); assert(!mode_known && !native[0].data[2]);
    mac_hid_remote_mouse(mouse); assert(cursor_mode && native[1].data[0]==1); /* Reconnect in cursor mode. */
    mac_hid_remote_ready(false);
    puts("PASS: captured sensor restart preserves cursor mapping, explicit mode switch releases, dual-source clicks, reconnect mode recovery");
}

static void mouse_settings_check(void)
{
    link.conn=7; link.encrypted=true; suspended=false; subscribed=false;
    for(unsigned i=0;i<3;++i) { native[i].subscribed=true; }
    mac_hid_remote_ready(false); mac_hid_sensor_mode(true); sent_count=0;
    uint8_t config[KEYMAP_LENGTH], key[8]={0}, motion[4]={0,1,0xff,1};
    /* Every slider value preserves signed fractional motion and leaves wheel speed unchanged. */
    for(unsigned speed=1;speed<=KEYMAP_SPEED_MAX;++speed) {
        keymap_read(config); config[7]=speed; assert(keymap_apply(config,sizeof(config))==KEYMAP_OK);
        clear_native(); flush_native(); sent_count=0;
        for(unsigned n=0;n<4;++n) { mac_hid_remote_mouse(motion); on_mouse_tick(NULL); }
        int x=0,y=0,wheel=0;
        for(unsigned n=0;n<sent_count;++n) {
            assert(sent[n].handle==native[1].handle);
            x+=(int8_t)sent[n].data[1]; y+=(int8_t)sent[n].data[2]; wheel+=(int8_t)sent[n].data[3];
        }
        assert(x==(int)speed && y==-(int)speed && wheel==4);
    }
    /* Maximum speed must split large deltas without signed-byte overflow or clipping. */
    clear_native(); flush_native(); sent_count=0;
    motion[1]=127; motion[2]=(uint8_t)-127; motion[3]=0;
    mac_hid_remote_mouse(motion);
    while(mouse_count) { on_mouse_tick(NULL); }
    assert(sent_count==3);
    for(unsigned n=0;n<3;++n) { assert((int8_t)sent[n].data[1]==127 && (int8_t)sent[n].data[2]==-127); }

    /* HOME cancels pending motion immediately, including fractions, even under backpressure. */
    keymap_read(config); config[7]=1; assert(keymap_apply(config,sizeof(config))==KEYMAP_OK);
    clear_native(); flush_native(); sent_count=0;
    motion[1]=7; motion[2]=(uint8_t)-7;
    mac_hid_remote_mouse(motion); /* Queued +/-1 and fractional +/-3 quarters. */
    key[7]=0x4a; mac_hid_keyboard(key);
    assert(home_held && !native[0].data[2] && !native[2].data[0]);
    mbuf_used=HID_MBUF_MAX_USED;
    for(unsigned n=0;n<1000;++n) { mac_hid_remote_mouse(motion); }
    mac_hid_sensor_mode(false); mac_hid_sensor_mode(true); /* Automatic sensor pause/restart. */
    assert(cursor_mode && home_held);
    mbuf_used=0;
    while(mouse_count) { on_mouse_tick(NULL); }
    for(unsigned n=0;n<sent_count;++n) {
        assert(sent[n].handle==native[1].handle && !sent[n].data[1] && !sent[n].data[2] && !sent[n].data[3]);
    }
    /* Click press and release remain usable while the cursor is frozen. */
    sent_count=0; motion[0]=1; mac_hid_remote_mouse(motion); on_mouse_tick(NULL);
    motion[0]=0; mac_hid_remote_mouse(motion); on_mouse_tick(NULL);
    assert(sent_count==2 && sent[0].data[0]==1 && !sent[1].data[0]);
    assert(!sent[0].data[1] && !sent[0].data[2] && !sent[1].data[1] && !sent[1].data[2]);
    key[7]=0; mac_hid_keyboard(key); sent_count=0;
    on_mouse_tick(NULL); on_heartbeat(NULL); assert(!sent_count);
    motion[1]=1; motion[2]=(uint8_t)-1;
    int x=0,y=0;
    for(unsigned n=0;n<4;++n) {
        mac_hid_remote_mouse(motion); on_mouse_tick(NULL);
        assert((int8_t)sent[n].data[1]==(n==3?1:0));
        x+=(int8_t)sent[n].data[1]; y+=(int8_t)sent[n].data[2];
    }
    assert(x==1 && y==-1); /* No contribution from repositioning or previous fractions. */

    /* HOME already held before the first sensor packet also freezes recovery into mouse mode. */
    mac_hid_remote_ready(false); key[7]=0x4a; mac_hid_keyboard(key);
    assert(native[0].data[2]==0x4a); /* Normal mode retains HOME. */
    mac_hid_remote_mouse(motion); assert(cursor_mode && home_held && !native[0].data[2]);
    assert(!mouse_count);
    mac_hid_remote_ready(false); assert(!home_held);
    keymap_read(config); config[7]=KEYMAP_SPEED_DEFAULT; assert(keymap_apply(config,sizeof(config))==KEYMAP_OK);
    mac_hid_remote_mouse(motion); assert(cursor_mode && mouse_count);
    mac_hid_remote_ready(false);
    puts("PASS: all mouse speeds, signed subpixel accumulation, large deltas, HOME pause/discard/resume, click preservation and mode recovery");
}

int main(void)
{
    keymap_init();
    descriptor_check();
    input_handle=10; for(unsigned i=0;i<3;++i)native[i].handle=20+i;
    struct ble_gap_event event={.type=BLE_GAP_EVENT_CONNECT,.connect={.conn_handle=7}};
    mac_gap_event(&event,NULL);
    assert(!security_requests);
    uint8_t key[8]={0,0,0x50}, mouse[4]={1,0xff,3,0};
    mac_hid_keyboard(key); mac_hid_mouse(mouse); assert(!sent_count);
    for(unsigned i=0;i<3;++i) {
        event=(struct ble_gap_event){.type=BLE_GAP_EVENT_SUBSCRIBE,.subscribe={.attr_handle=20+i,.cur_notify=true}};
        mac_gap_event(&event,NULL);
    }
    assert(!sent_count); /* Neither keys nor motion may bypass encryption. */
    map_read=true; /* Host has read the expanded Report Map. */
    event=(struct ble_gap_event){.type=BLE_GAP_EVENT_ENC_CHANGE,.enc_change={.conn_handle=7}}; mac_gap_event(&event,NULL);
    assert(changed_count==0 && sent_count==3 && saved_schema); /* All new Reports already subscribed. */
    assert(sent[0].data[2]==0x50 && sent[1].data[0]==1 && !sent[1].data[1] && !sent[1].data[2]);
    sent_count=0;
    mac_hid_mouse(mouse); assert(sent_count==0); on_mouse_tick(NULL); assert(sent_count==1 && !memcmp(sent[0].data,mouse,4));
    on_heartbeat(NULL); on_heartbeat(NULL); assert(sent_count==1);
    /* GATT read must also return zero relative axes. */
    buffer=(struct os_mbuf){buffer_data,0};
    struct ble_gatt_access_ctxt ctxt={.op=BLE_GATT_ACCESS_OP_READ_CHR,.om=&buffer,.chr=&services[0].characteristics[5]};
    access_hid(7,21,&ctxt,&native[1]); assert(buffer.len==4 && buffer.data[0]==1 && !buffer.data[1] && !buffer.data[2]);
    send_error=1; mouse[0]=0; mac_hid_mouse(mouse); on_mouse_tick(NULL); assert(native[1].dirty);
    send_error=0; on_heartbeat(NULL); assert(sent_count==2 && !memcmp(sent[1].data,"\0\0\0\0",4));
    memset(key,0,8); key[2]=0x80; mac_hid_keyboard(key);
    assert(sent_count==4 && !sent[2].data[2] && sent[3].data[0]==1);
    mac_hid_remote_ready(false);
    assert(sent_count==7 && !sent[6].data[0]);
    unsigned before=sent_count; on_heartbeat(NULL); assert(sent_count==before);
    event=(struct ble_gap_event){.type=BLE_GAP_EVENT_NOTIFY_TX,.notify_tx={.attr_handle=21,.status=1}};
    mac_gap_event(&event,NULL); assert(native[1].dirty);
    on_heartbeat(NULL); assert(sent_count==before+1 && !memcmp(sent[before].data,"\0\0\0\0",4));
    key[2]=0x76; mac_hid_keyboard(key); assert(native[0].data[0]==8);
    event=(struct ble_gap_event){.type=BLE_GAP_EVENT_DISCONNECT}; mac_gap_event(&event,NULL);
    assert(link.conn==BLE_HS_CONN_HANDLE_NONE && !native[0].data[0] && !native[0].subscribed);
    /* Reboot-like start targets the saved Mac, then alternates open windows. */
    assert(mac_hid_advertise(0)==0 && adv_mode==BLE_GAP_CONN_MODE_DIR && adv_duration==10000);
    unsigned ads=adv_calls;
    retry_advertising(NULL); assert(adv_calls==ads);
    adv_active=false;
    event=(struct ble_gap_event){.type=BLE_GAP_EVENT_ADV_COMPLETE}; mac_gap_event(&event,NULL);
    assert(adv_delay==50); retry_advertising(NULL);
    assert(adv_mode==BLE_GAP_CONN_MODE_UND && adv_duration==20000);
    adv_active=false; mac_gap_event(&event,NULL); retry_advertising(NULL);
    assert(adv_mode==BLE_GAP_CONN_MODE_DIR);
    adv_active=false;
    event=(struct ble_gap_event){.type=BLE_GAP_EVENT_CONNECT,.connect={.conn_handle=7}}; mac_gap_event(&event,NULL);
    assert(!adv_delay && !security_requests);
    event=(struct ble_gap_event){.type=BLE_GAP_EVENT_ENC_CHANGE,.enc_change={.conn_handle=7}}; mac_gap_event(&event,NULL);
    assert(changed_count==0); /* Saved schema must not trigger rediscovery every reconnect. */
    uint8_t configuration[KEYMAP_LENGTH]; keymap_read(configuration);
    configuration[8]=0x84; configuration[9]=0x10; /* Command+A, compact v2. */
    buffer=(struct os_mbuf){configuration,KEYMAP_LENGTH};
    struct ble_gatt_access_ctxt write={.op=BLE_GATT_ACCESS_OP_WRITE_CHR,.om=&buffer};
    assert(access_keymap(6,0,&write,NULL)==BLE_ATT_ERR_INSUFFICIENT_AUTHEN);
    link.encrypted=false; assert(access_keymap(7,0,&write,NULL)==BLE_ATT_ERR_INSUFFICIENT_AUTHEN);
    link.encrypted=true;
    native[0].data[0]=8; native[0].data[2]=0x2b;
    assert(access_keymap(7,0,&write,NULL)==0 && !native[0].data[0] && !native[0].data[2]);
    buffer=(struct os_mbuf){buffer_data,0};
    struct ble_gatt_access_ctxt read={.op=BLE_GATT_ACCESS_OP_READ_CHR,.om=&buffer};
    assert(access_keymap(7,0,&read,NULL)==0 && buffer.len==64 && buffer.data[4]==1 && buffer.data[8]==0x84);
    mac_hid_on_reset(); assert(!advertising_ready && !link.encrypted && link.conn==BLE_HS_CONN_HANDLE_NONE);
    mode_mapping_check();
    mouse_flow_check();
    mouse_settings_check();
    bond_recovery_check();
    puts("PASS: bounded mouse queue, pressure/allocation recovery, click order; GATT report sizes/references, security/subscription gates, relative motion once, lost release retry, disconnect release");
}
