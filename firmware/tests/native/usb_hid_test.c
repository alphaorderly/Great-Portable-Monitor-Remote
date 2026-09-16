/* Actual S3 adapter + common input engine. Deterministic task/USB simulation;
 * OS enumeration, PHY and real scheduling remain hardware acceptance tests. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define TAG input_tag
#include "../../main/hid_input.c"
#undef TAG
#include "../../main/usb_hid.c"
extern bool test_storage_failure;
static TickType_t now;
static bool defer_owner, endpoint_ready = true, send_failed;
static struct ble_npl_event *events[8];
static unsigned event_count;
static usb_packet_t sent[512];
static unsigned sent_count;
static unsigned tx_notifications;
static void attach(void) { tinyusb_event_t e={.id=TINYUSB_EVENT_ATTACHED};usb_event(&e,NULL); }
static void detach(void) { tinyusb_event_t e={.id=TINYUSB_EVENT_DETACHED};usb_event(&e,NULL); }
static void drain_owner(void)
{
    while (event_count) {
        struct ble_npl_event *e = events[0];
        memmove(events, events + 1, --event_count * sizeof(*events));
        e->fn(e);
    }
}
void ble_npl_event_init(struct ble_npl_event *e, void (*fn)(struct ble_npl_event *), void *arg)
{ e->fn=fn; e->arg=arg; }
void ble_npl_eventq_put(void *q, struct ble_npl_event *e)
{
    (void)q;
    for (unsigned i=0;i<event_count;++i) { if(events[i]==e)return; }
    assert(event_count<8); events[event_count++]=e;
}
void *nimble_port_get_dflt_eventq(void) { return NULL; }
int ble_npl_callout_init(struct ble_npl_callout *c,void *q,void (*fn)(struct ble_npl_event *),void *a)
{ (void)c;(void)q;(void)fn;(void)a;return 0; }
int ble_npl_callout_reset(struct ble_npl_callout *c,uint32_t n) { (void)c;(void)n;return 0; }
void ble_npl_callout_stop(struct ble_npl_callout *c) { (void)c; }
uint32_t ble_npl_time_ms_to_ticks32(uint32_t n) { return n; }
void test_log(const char *fmt,...) { (void)fmt; }
TickType_t xTaskGetTickCount(void) { return now; }
void vTaskDelay(TickType_t n) { now+=n; }
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *handle)
{ (void)fn;(void)name;(void)stack;(void)arg;(void)priority;if(handle)*handle=&tx_notifications;return pdPASS; }
void xTaskNotifyGive(TaskHandle_t handle) { assert(handle==tx_handle);++tx_notifications; }
uint32_t ulTaskNotifyTake(int clear,TickType_t timeout)
{ (void)clear;(void)timeout;unsigned n=tx_notifications;tx_notifications=0;return n; }
static TestSemaphore semaphores[2];
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{ semaphores[0].value=1;semaphores[0].mutex=true;return &semaphores[0]; }
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{ semaphores[1].value=0;return &semaphores[1]; }
int xSemaphoreTake(SemaphoreHandle_t s,TickType_t timeout)
{
    if (!s->mutex && !s->value && timeout && !defer_owner) { drain_owner(); }
    if (s->value) { --s->value; return pdTRUE; }
    assert(!s->mutex); if(timeout)now+=timeout; return 0;
}
int xSemaphoreGive(SemaphoreHandle_t s) { assert(!s->value); s->value=1;return pdTRUE; }
int tinyusb_driver_install(const tinyusb_config_t *cfg)
{ assert(cfg->descriptor.full_speed_config==configuration);return ESP_OK; }
int esp_read_mac(uint8_t *mac,int kind) { (void)kind;memset(mac,0xab,6);return ESP_OK; }
bool tud_hid_ready(void) { return endpoint_ready; }
bool tud_hid_report(uint8_t id,const void *data,uint16_t len)
{
    if(send_failed)return false;
    assert(sent_count<512 && len<=REPORT_LENGTH);
    sent[sent_count]=(usb_packet_t){.id=id,.length=len};
    memcpy(sent[sent_count++].data,data,len); return true;
}
static void drain_usb(void)
{
    for(unsigned i=0;(count || in_flight || event_count) && i<100;++i) {
        drain_owner();
        tx_once();
        if(in_flight)tud_hid_report_complete_cb(0,NULL,0);
    }
    assert(!count && !in_flight && !event_count);
}
static void clear_sent(void) { drain_usb();sent_count=0; }
static void check_descriptor(void)
{
    unsigned id=0,size=0,n=0,bits[7]={0},features[7]={0};
    assert(tud_hid_descriptor_report_cb(0)==report_map);
    for(unsigned off=0;off<sizeof(report_map);) {
        uint8_t prefix=report_map[off++];unsigned bytes=prefix&3;if(bytes==3)bytes=4;
        unsigned value=0; assert(off+bytes<=sizeof(report_map));
        for(unsigned j=0;j<bytes;++j)value|=(unsigned)report_map[off++]<<(8*j);
        switch(prefix&0xfc) {
        case 0x84:id=value;assert(id<7);break;
        case 0x74:size=value;break;
        case 0x94:n=value;break;
        case 0x80:bits[id]+=size*n;break;
        case 0xb0:features[id]+=size*n;break;
        }
    }
    assert(bits[1]==96 && bits[2]==64 && bits[3]==32 && bits[4]==8 && features[5]==512 && features[6]==512);
}
static void check_features(void)
{
    uint8_t wire[65]={5},old[64];
    assert(tud_hid_get_report_cb(0,5,HID_REPORT_TYPE_FEATURE,wire+1,64)==64);
    memcpy(old,wire+1,64);wire[8]=8;
    tud_hid_set_report_cb(0,5,HID_REPORT_TYPE_FEATURE,wire+1,64);
    assert(tud_hid_get_report_cb(0,5,HID_REPORT_TYPE_FEATURE,wire+1,64)==64);
    assert(wire[8]==8 && wire[5]==1 && !wire[6]);
    /* Stale revision, malformed ID/type/length, and failed NVS never commit. */
    tud_hid_set_report_cb(0,5,HID_REPORT_TYPE_FEATURE,old,64);
    tud_hid_set_report_cb(0,4,HID_REPORT_TYPE_FEATURE,wire+1,64);
    tud_hid_set_report_cb(0,5,HID_REPORT_TYPE_INPUT,wire+1,64);
    tud_hid_set_report_cb(0,5,HID_REPORT_TYPE_FEATURE,wire+1,63);
    assert(!tud_hid_get_report_cb(0,5,HID_REPORT_TYPE_FEATURE,wire+1,63));
    memcpy(old,wire+1,64);wire[8]=9;test_storage_failure=true;
    tud_hid_set_report_cb(0,5,HID_REPORT_TYPE_FEATURE,wire+1,64);
    test_storage_failure=false;
    assert(tud_hid_get_report_cb(0,5,HID_REPORT_TYPE_FEATURE,wire+1,64)==64);
    assert(!memcmp(old,wire+1,64));
    keymap_init();keymap_read(wire+1);assert(!memcmp(old,wire+1,64));
    /* A timed-out request may not overwrite the next request or commit late. */
    defer_owner=true;wire[8]=10;
    tud_hid_set_report_cb(0,5,HID_REPORT_TYPE_FEATURE,wire+1,64);
    assert(feature.busy);
    assert(!tud_hid_get_report_cb(0,5,HID_REPORT_TYPE_FEATURE,wire+1,64));
    defer_owner=false;drain_owner();
    assert(tud_hid_get_report_cb(0,5,HID_REPORT_TYPE_FEATURE,wire+1,64)==64);
    assert(!memcmp(old,wire+1,64));
}
static void check_input(void)
{
    uint8_t key[8]={0,0,4},mouse[4]={1,3,0xfe,0};
    clear_sent();hid_input_keyboard(key);memset(key,0,8);hid_input_keyboard(key);drain_usb();
    bool pressed=false,released=false;
    for(unsigned i=0;i<sent_count;++i)if(sent[i].id==2) {
        if(sent[i].data[2]==4)pressed=true;
        if(pressed && !sent[i].data[2])released=true;
    }
    /* Unknown raw A is unmapped; use a mapped direction instead. */
    assert(!pressed && !released);
    key[2]=0x52;clear_sent();hid_input_keyboard(key);key[2]=0;hid_input_keyboard(key);drain_usb();
    for(unsigned i=0;i<sent_count;++i)if(sent[i].id==2) {
        if(sent[i].data[2]==0x52)pressed=true;
        if(pressed && !sent[i].data[2])released=true;
    }
    assert(pressed && released);
    clear_sent();hid_input_mouse(mouse);hid_input_output_ready();mouse[0]=0;mouse[1]=mouse[2]=0;hid_input_mouse(mouse);hid_input_output_ready();
    drain_usb();assert(sent_count==2 && sent[0].id==3 && sent[0].data[0]==1 && !sent[1].data[0]);
    /* Pressure cannot grow queues; latest key/button release eventually wins. */
    endpoint_ready=false;
    for(unsigned i=0;i<2000;++i) {
        key[2]=(i&1)?0:0x52;hid_input_keyboard(key);
        mouse[0]=i&1;mouse[1]=1;hid_input_mouse(mouse);hid_input_output_ready();
        assert(count<=USB_QUEUE_SIZE && mouse_count<=MOUSE_QUEUE_SIZE);
    }
    memset(key,0,8);memset(mouse,0,4);hid_input_keyboard(key);hid_input_mouse(mouse);hid_input_output_ready();
    endpoint_ready=true;drain_usb();clear_sent();on_heartbeat(NULL);hid_input_output_ready();drain_usb();
    assert(!native[0].dirty && !native[1].dirty);
    for(unsigned i=0;i<sent_count;++i)if(sent[i].id==2 || sent[i].id==3)assert(!sent[i].data[0] && !sent[i].data[2]);
    /* No replay across a host generation even when unmount/mount coalesce. */
    mouse[1]=20;hid_input_mouse(mouse);hid_input_output_ready();
    detach();attach();assert(!host_output_ready(3));drain_owner();clear_sent();hid_input_output_ready();drain_usb();
    for(unsigned i=0;i<sent_count;++i)if(sent[i].id==3)assert(!sent[i].data[1]);
    key[2]=0x4a;hid_input_keyboard(key);assert(home_held);
    tud_suspend_cb(false);drain_owner();assert(!host_output_ready(2) && !home_held);
    hid_input_mouse(mouse);assert(!mouse_count);
    tud_resume_cb();drain_owner();assert(host_output_ready(2));
    clear_sent();hid_input_mouse(mouse);hid_input_output_ready();send_failed=true;tx_once();send_failed=false;drain_owner();drain_usb();
    hid_input_output_ready();drain_usb();assert(!native[1].dirty);
    tud_hid_report_failed_cb(0,HID_REPORT_TYPE_INPUT,NULL,0);drain_owner();drain_usb();
    usb_hid_on_reset();assert(!host_output_ready(1) && !count);
    assert(!usb_hid_start());assert(host_output_ready(1));
}

static void check_mouse_latency(void)
{
    uint8_t saved[64],settings[64];keymap_read(saved);memcpy(settings,saved,64);
    settings[7]=KEYMAP_SPEED_DEFAULT;assert(keymap_apply(settings,64)==KEYMAP_OK);
    hid_input_mapping_changed();clear_sent();
    TickType_t started=now;
    unsigned notifications=tx_notifications;
    uint8_t first[4]={0,3,(uint8_t)-2,0};
    hid_input_mouse(first);
    assert(count==1 && tx_notifications>notifications); /* No mouse timer needed. */
    tx_once();assert(in_flight && !count && sent_count==1);
    uint8_t a[4]={0,4,(uint8_t)-3,1},b[4]={0,6,(uint8_t)-4,0};
    uint8_t down[4]={1,7,3,(uint8_t)-1},up[4]={0};
    hid_input_mouse(a);hid_input_mouse(b);hid_input_mouse(down);hid_input_mouse(up);
    assert(!count && mouse_count==3 && host_output_pending(3));
    notifications=tx_notifications;
    tud_hid_report_complete_cb(0,NULL,0);
    assert(tx_notifications>notifications && event_count);
    drain_owner();assert(count==1 && mouse_count==2);
    drain_usb();
    assert(now==started && sent_count==4 && !mouse_count && !native[1].dirty);
    const uint8_t expected[4][4]={{0,3,254,0},{0,10,249,1},{1,7,3,255},{0,0,0,0}};
    for(unsigned i=0;i<4;++i)assert(sent[i].id==3 && !memcmp(sent[i].data,expected[i],4));

    /* High speed splits signed movement into legal reports without losing distance. */
    keymap_read(settings);settings[7]=KEYMAP_SPEED_MAX;assert(keymap_apply(settings,64)==KEYMAP_OK);
    hid_input_mapping_changed();clear_sent();
    uint8_t large[4]={0,100,(uint8_t)-100,1};hid_input_mouse(large);drain_usb();
    int x=0,y=0,wheel=0;
    for(unsigned i=0;i<sent_count;++i) {
        assert(sent[i].id==3);x+=(int8_t)sent[i].data[1];y+=(int8_t)sent[i].data[2];wheel+=(int8_t)sent[i].data[3];
    }
    assert(sent_count==3 && x==300 && y==-300 && wheel==1);

    /* Fractional speed still accumulates sub-pixel deltas across completions. */
    keymap_read(settings);settings[7]=1;assert(keymap_apply(settings,64)==KEYMAP_OK);
    hid_input_mapping_changed();clear_sent();
    uint8_t small[4]={0,1,(uint8_t)-1,0};
    for(unsigned i=0;i<4;++i) { hid_input_mouse(small);drain_usb(); }
    x=y=0;
    for(unsigned i=0;i<sent_count;++i) { x+=(int8_t)sent[i].data[1];y+=(int8_t)sent[i].data[2]; }
    assert(x==1 && y==-1);

    /* A full USB queue keeps unsent motion until a completion makes room. */
    keymap_read(settings);settings[7]=KEYMAP_SPEED_DEFAULT;assert(keymap_apply(settings,64)==KEYMAP_OK);
    hid_input_mapping_changed();clear_sent();
    uint8_t empty_key[8]={0};
    for(unsigned i=0;i<USB_QUEUE_SIZE;++i)assert(host_output_send(2,empty_key,8));
    hid_input_mouse(first);assert(mouse_count==1);
    drain_usb();assert(!mouse_count);
    unsigned mice=0;
    for(unsigned i=0;i<sent_count;++i)if(sent[i].id==3) { ++mice;assert(!memcmp(sent[i].data,first,4)); }
    assert(mice==1);

    /* HOME discards both the queued packet's motion and the owner accumulator. */
    hid_input_sensor_mode(true);clear_sent();
    hid_input_remote_mouse(first);hid_input_remote_mouse(a);
    uint8_t home[8]={0,0,0x4a};hid_input_keyboard(home);drain_usb();
    for(unsigned i=0;i<sent_count;++i)if(sent[i].id==3)assert(!sent[i].data[1] && !sent[i].data[2] && !sent[i].data[3]);
    memset(home,0,8);hid_input_keyboard(home);hid_input_remote_ready(false);drain_usb();
    keymap_read(settings);settings[7]=saved[7];assert(keymap_apply(settings,64)==KEYMAP_OK);
    hid_input_mapping_changed();drain_usb();
    puts("PASS: immediate mouse submission, completion wakeups, coalescing/click order, signed and fractional speed, queue pressure and HOME discard");
}

static void macro_command(unsigned cmd,unsigned offset,const uint8_t *data,unsigned n,uint8_t out[64])
{
    uint8_t p[64]={'M','X',1,0,8,0,42,0,0,0,0,0,24,0,0,0};
    p[3]=cmd;p[10]=offset;p[14]=n;
    if(n)memcpy(p+16,data,n);
    tud_hid_set_report_cb(0,6,HID_REPORT_TYPE_FEATURE,p,64);
    assert(tud_hid_get_report_cb(0,6,HID_REPORT_TYPE_FEATURE,out,64)==64);
    assert(!out[5]);
}
static void check_macro(void)
{
    uint8_t blob[24]={'M','C',1,0,1,1,40,0,80,0,180,0,0,0,0,0,4,0,0,0,1,1,0,'A'},answer[64];
    macro_command(MACRO_BEGIN,0,NULL,0,answer);
    macro_command(MACRO_CHUNK,0,blob,24,answer);
    macro_command(MACRO_COMMIT,0,NULL,0,answer);assert(answer[8]==1);
    assert(!strcmp(strings[2],"USB Keyboard & Mouse"));
    uint8_t input[8]={0};clear_sent(); /* First press after save works without a synthetic release. */
    input[2]=0x28;hid_input_keyboard(input);input[2]=0;hid_input_keyboard(input);
    drain_usb();sent_count=0;
    for(unsigned i=0;i<4;++i) { now+=5;on_macro_tick(NULL);drain_usb(); }
    assert(macro_active());
    assert(sent_count==1 && sent[0].id==2 && sent[0].data[0]==2 && sent[0].data[2]==4);
    /* A second physical press cancels, releases, and cannot retrigger on hold. */
    input[2]=0x28;hid_input_keyboard(input);drain_usb();on_macro_tick(NULL);drain_usb();
    assert(!macro_active() && !native[0].data[0] && !native[0].data[2]);
    unsigned before=sent_count;
    for(unsigned i=0;i<100;++i) { now+=5;hid_input_keyboard(input);on_macro_tick(NULL);drain_usb(); }
    for(unsigned i=before;i<sent_count;++i)if(sent[i].id==2)assert(!sent[i].data[2]);
    /* A physical button after reconnect must be released before it can start. */
    detach();attach();drain_owner();drain_usb();
    hid_input_keyboard(input);on_macro_tick(NULL);drain_usb();assert(!macro_active());
    input[2]=0;hid_input_keyboard(input);input[2]=0x28;hid_input_keyboard(input);drain_usb();
    for(unsigned i=0;i<4;++i) { now+=5;on_macro_tick(NULL);drain_usb(); }
    assert(macro_active());
    tud_hid_report_failed_cb(0,HID_REPORT_TYPE_INPUT,NULL,0);
    on_macro_tick(NULL);assert(!count); /* Failure blocks output even before the owner handles cancellation. */
    drain_owner();drain_usb();on_macro_tick(NULL);drain_usb();
    assert(!macro_active() && !native[0].data[2]);
    /* A second press before the timer runs must cancel the deferred start. */
    input[2]=0;hid_input_keyboard(input);input[2]=0x28;hid_input_keyboard(input);
    assert(macro_pending_slot==8);
    input[2]=0;hid_input_keyboard(input);input[2]=0x28;hid_input_keyboard(input);
    drain_usb();on_macro_tick(NULL);drain_usb();assert(macro_pending_slot<0 && !macro_active());
    /* Stop through the Feature interface cancels a deferred start too. */
    input[2]=0;hid_input_keyboard(input);input[2]=0x28;hid_input_keyboard(input);
    macro_command(MACRO_STOP,0,NULL,0,answer);drain_usb();on_macro_tick(NULL);drain_usb();
    assert(macro_pending_slot<0 && !macro_active());
    puts("PASS: macro Feature upload/readback, remote trigger/retrigger/cancel, USB failure and reconnect disarming");
}
int main(void)
{
    assert(!usb_hid_init());check_descriptor();
    attach();assert(!usb_hid_start());drain_owner();
    check_features();check_input();check_mouse_latency();check_macro();
    puts("PASS: USB descriptor/Feature reports, save/readback/rollback/revision/timeout, bounded TX, releases, unplug/suspend/reset recovery");
}
