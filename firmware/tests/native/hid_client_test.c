/* Host-only protocol simulation. Real ESP-IDF compilation is checked separately.
 * No radio, flash, SMP implementation, or peripheral is simulated here.
 */
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "../../main/hid_client.c"
#include "stubs/input_log_impl.h"

static char output[65536];
static size_t output_length;
static bool encrypted;
static enum { NONE, SERVICE, CHARS, MAP, DESCRIPTORS, READ, WRITE } operation;
static uint16_t requested_start, requested_end;
static unsigned requests;
static const struct ble_gatt_error ok = {0};
static const struct ble_gatt_error done = {.status = BLE_HS_EDONE};
static uint8_t fixture[512];
static unsigned fixture_length;
static unsigned forwarded;
static uint8_t last_forwarded[8];
static bool bridge_ready;
static unsigned mouse_forwarded;
static uint8_t last_mouse[4];
void hid_input_remote_ready(bool ready) { bridge_ready=ready; }
void hid_input_keyboard(const uint8_t keyboard[8]) { ++forwarded; memcpy(last_forwarded,keyboard,8); }
void hid_input_remote_mouse(const uint8_t mouse[4]) { ++mouse_forwarded; memcpy(last_mouse,mouse,4); }
void hid_input_sensor_mode(bool cursor) { (void)cursor; }

void test_log(const char *fmt, ...)
{
    va_list args; va_start(args,fmt);
    int n=vsnprintf(output+output_length,sizeof(output)-output_length,fmt,args);
    va_end(args);
    assert(n>=0 && (size_t)n+1<sizeof(output)-output_length);
    output_length += n;
    output[output_length++]='\n'; output[output_length]=0;
}
void test_hex(const void *v,unsigned n)
{
    const uint8_t *b=v;
    for (unsigned i=0;i<n;i++) test_log("%02X",b[i]);
}
int os_mbuf_copydata(const struct os_mbuf *om,int off,int n,void *dst)
{
    if(off<0 || n<0 || (unsigned)(off+n)>om->len) return -1;
    memcpy(dst,om->data+off,n); return 0;
}
uint16_t ble_uuid_u16(const ble_uuid_t *u) { return u->value; }
char *ble_uuid_to_str(const ble_uuid_t *u,char *dst) { snprintf(dst,BLE_UUID_STR_LEN,"%04X",u->value); return dst; }
int ble_gap_conn_find(uint16_t c,struct ble_gap_conn_desc *d)
{ assert(c==7); d->sec_state.encrypted=encrypted; d->sec_state.bonded=encrypted; return 0; }
#define RECORD(kind,a,b) do { operation=kind; requested_start=a; requested_end=b; requests++; } while(0)
int ble_gattc_disc_svc_by_uuid(uint16_t c,const ble_uuid_t *u,ble_gatt_disc_svc_fn *cb,void *arg)
{ assert(c==7 && u->value==0x1812 && cb==on_service && !arg); RECORD(SERVICE,0,0); return 0; }
int ble_gattc_disc_all_chrs(uint16_t c,uint16_t a,uint16_t b,ble_gatt_chr_fn *cb,void *arg)
{ assert(c==7 && cb==on_characteristics && !arg); RECORD(CHARS,a,b); return 0; }
int ble_gattc_disc_all_dscs(uint16_t c,uint16_t a,uint16_t b,ble_gatt_dsc_fn *cb,void *arg)
{ assert(c==7 && cb==on_descriptors && arg); RECORD(DESCRIPTORS,a,b); return 0; }
int ble_gattc_read_long(uint16_t c,uint16_t a,uint16_t off,ble_gatt_attr_fn *cb,void *arg)
{ assert(c==7 && !off && cb==on_map && !arg); RECORD(MAP,a,0); return 0; }
int ble_gattc_read(uint16_t c,uint16_t a,ble_gatt_attr_fn *cb,void *arg)
{
    assert(c==7);
    assert(((cb==on_reference || cb==on_cccd_read) && arg) || (cb==on_protocol_mode && !arg));
    RECORD(READ,a,0); return 0;
}
int ble_gattc_write_flat(uint16_t c,uint16_t a,const void *v,uint16_t n,ble_gatt_attr_fn *cb,void *arg)
{
    assert(c==7 && n==2 && !memcmp(v,"\x01\x00",2) && cb==on_subscribed && arg);
    RECORD(WRITE,a,0); return 0;
}
static void reset_test(void)
{ memset(&client,0,sizeof(client)); operation=NONE; requests=0; output_length=0; output[0]=0; encrypted=true; forwarded=0; bridge_ready=false; }
static void characteristic(uint16_t def,uint16_t val,uint16_t uuid,uint8_t properties)
{
    struct ble_gatt_chr chr={.def_handle=def,.val_handle=val,.uuid={.u={uuid}},.properties=properties};
    assert(on_characteristics(7,&ok,&chr,NULL)==0);
}
static void discover(void)
{
    hid_client_start(7); assert(operation==SERVICE);
    unsigned before=requests; hid_client_start(7); assert(requests==before);
    struct ble_gatt_svc svc={.start_handle=0x20,.end_handle=0x50,.uuid={.u={0x1812}}};
    on_service(7,&ok,&svc,NULL); on_service(7,&done,NULL,NULL);
    assert(operation==CHARS && requested_start==0x20 && requested_end==0x50);
    characteristic(0x22,0x23,0x2a4b,0x02);
    characteristic(0x24,0x25,0x2a4a,0x02);
    characteristic(0x2f,0x30,0x2a4d,0x10);
    characteristic(0x33,0x34,0x2a4d,0x10);
    characteristic(0x37,0x38,0x2a4e,0x02);
    on_characteristics(7,&done,NULL,NULL);
    assert(operation==MAP && requested_start==0x23);
    for(unsigned off=0;off<fixture_length;off+=22) {
        unsigned n=fixture_length-off<22?fixture_length-off:22;
        struct os_mbuf om={fixture+off,n};
        struct ble_gatt_attr attr={.handle=0x23,.offset=off,.om=&om};
        assert(on_map(7,&ok,&attr,NULL)==0);
    }
    on_map(7,&done,NULL,NULL);
    assert(client.map_length==fixture_length && client.keyboard_layout);
    assert(operation==READ && requested_start==0x38);
    uint8_t mode=1; struct os_mbuf mode_om={&mode,1};
    struct ble_gatt_attr mode_attr={.handle=0x38,.om=&mode_om};
    on_protocol_mode(7,&ok,&mode_attr,NULL);
    assert(strstr(output,"value=0x01 (Report)"));
    /* Must start from value, not value+1; stop before the next declaration. */
    assert(operation==DESCRIPTORS && requested_start==0x30 && requested_end==0x32);
}
static void report_reference(unsigned index,uint8_t id,uint8_t type)
{
    hid_characteristic_t *r=&client.chars[index];
    struct ble_gatt_dsc cccd={.handle=r->chr.val_handle+1,.uuid={.u={0x2902}}};
    struct ble_gatt_dsc ref={.handle=r->chr.val_handle+2,.uuid={.u={0x2908}}};
    on_descriptors(7,&ok,r->chr.val_handle,&cccd,r);
    on_descriptors(7,&ok,r->chr.val_handle,&ref,r);
    on_descriptors(7,&done,r->chr.val_handle,NULL,r);
    assert(operation==READ && requested_start==ref.handle);
    uint8_t bytes[]={id,type}; struct os_mbuf om={bytes,2};
    struct ble_gatt_attr attr={.handle=ref.handle,.om=&om};
    on_reference(7,&ok,&attr,r);
}
static void descriptors(unsigned index,uint8_t id)
{ report_reference(index,id,index==2 ? 2 : 1); }
static void notify(uint16_t handle,const uint8_t *data,unsigned n)
{
    struct os_mbuf om={data,n};
    struct ble_gap_event ev={.notify_rx={.om=&om,.attr_handle=handle,.conn_handle=7}};
    output_length=0; output[0]=0; hid_client_on_notify(&ev);
}
int main(int argc,char **argv)
{
    assert(argc==2); FILE *f=fopen(argv[1],"rb"); assert(f);
    fixture_length=fread(fixture,1,sizeof(fixture),f); assert(feof(f)); fclose(f);
    assert(fixture_length==237);
    reset_test(); encrypted=false; hid_client_start(7); assert(requests==0 && !client.active);
    reset_test(); discover(); descriptors(2,2);
    assert(operation==DESCRIPTORS && requested_start==0x34 && requested_end==0x36);
    descriptors(3,1); assert(operation==WRITE && requested_start==0x35 && !client.subscribed);
    const uint8_t left[]={0,0,0x50,0,0,0,0,0};
    notify(0x34,left,8); assert(strstr(output,"KEY: LEFT")); /* Allowed before write response. */
    on_subscribed(7,&ok,NULL,&client.chars[3]); assert(client.subscribed);
    assert(operation==READ && requested_start==0x35 && !strstr(output,"HID LISTENING"));
    uint8_t enabled[]={1,0}; struct os_mbuf cccd_om={enabled,2};
    struct ble_gatt_attr cccd_attr={.handle=0x35,.om=&cccd_om};
    on_cccd_read(7,&ok,&cccd_attr,&client.chars[3]);
    assert(strstr(output,"notification_enabled=1") && strstr(output,"HID LISTENING"));
    const uint8_t usages[]={0x4f,0x50,0x51,0x52};
    const char *names[]={"KEY: RIGHT","KEY: LEFT","KEY: DOWN","KEY: UP"};
    for(unsigned i=0;i<4;i++) { uint8_t bytes[8]={0}; bytes[2]=usages[i]; notify(0x34,bytes,8); assert(strstr(output,names[i])); }
    uint8_t zero[9]={0}; notify(0x34,zero,8); assert(strstr(output,"KEY: RELEASE_ALL"));
    zero[0]=2; notify(0x34,zero,8); assert(!strstr(output,"RELEASE_ALL"));
    notify(0x30,left,8); assert(!strstr(output,"KEY: LEFT"));
    notify(0x34,zero,9); assert(strstr(output,"Raw report only"));
    client.keyboard_layout=false; notify(0x34,left,8); assert(!strstr(output,"KEY: LEFT"));
    hid_client_stop(7); unsigned count=client.input_count;
    notify(0x34,left,8); assert(client.input_count==count && !output[0]);
    reset_test(); discover(); descriptors(2,2); descriptors(3,1);
    struct ble_gatt_error denied={.status=0x105,.att_handle=0x35};
    on_subscribed(7,&denied,NULL,&client.chars[3]); assert(!client.active && !client.subscribed);
    reset_test(); discover(); descriptors(2,2); descriptors(3,1);
    on_subscribed(7,&ok,NULL,&client.chars[3]);
    enabled[0]=0;
    on_cccd_read(7,&ok,&cccd_attr,&client.chars[3]);
    assert(client.active && client.listening && strstr(output,"HID LISTENING"));
    notify(0x34,left,8); assert(strstr(output,"KEY: LEFT"));
    reset_test(); discover(); descriptors(2,2); descriptors(3,1);
    on_subscribed(7,&ok,NULL,&client.chars[3]);
    cccd_om.len=1;
    assert(on_cccd_read(7,&ok,&cccd_attr,&client.chars[3])==BLE_HS_EAPP && !client.active);
    notify(0x34,left,8); assert(strstr(output,"KEY: LEFT")); /* Procedure error must not hide incoming data. */
    reset_test(); discover();
    uint8_t boot=0; struct os_mbuf boot_om={&boot,1};
    struct ble_gatt_attr boot_attr={.handle=0x38,.om=&boot_om};
    on_protocol_mode(7,&ok,&boot_attr,NULL);
    assert(client.active && strstr(output,"value=0x00 (Boot)") && operation==DESCRIPTORS);
    boot_om.len=0;
    assert(on_protocol_mode(7,&ok,&boot_attr,NULL)==BLE_HS_EAPP && !client.active);
    reset_test(); discover();
    on_protocol_mode(7,&denied,NULL,NULL); assert(!client.active);
    reset_test(); discover(); hid_client_stop(7); unsigned before=requests;
    on_protocol_mode(7,&ok,&boot_attr,NULL);
    on_cccd_read(7,&ok,&cccd_attr,&client.chars[3]); assert(requests==before);
    reset_test(); client.active=true; client.conn=7; client.map_handle=0x23;
    struct os_mbuf gap={fixture,22}; struct ble_gatt_attr bad={.handle=0x23,.offset=22,.om=&gap};
    assert(on_map(7,&ok,&bad,NULL)==BLE_HS_EAPP && !client.active);
    reset_test(); client.active=true; client.conn=7;
    uint8_t short_ref[]={1}; struct os_mbuf short_om={short_ref,1};
    struct ble_gatt_attr short_attr={.om=&short_om};
    assert(on_reference(7,&ok,&short_attr,&client.chars[0])==BLE_HS_EAPP && requests==0);
    /* Subscribe each Input, including non-keyboard, and continue after a zero readback. */
    reset_test(); discover(); report_reference(2,2,1);
    assert(operation==WRITE && requested_start==0x31 && !client.input_handle);
    on_subscribed(7,&ok,NULL,&client.chars[2]);
    uint8_t off[]={0,0}; struct os_mbuf off_om={off,2};
    struct ble_gatt_attr off_attr={.handle=0x31,.om=&off_om};
    on_cccd_read(7,&ok,&off_attr,&client.chars[2]);
    assert(client.active && operation==DESCRIPTORS && requested_start==0x34 && requested_end==0x36);
    assert(!strstr(output,"HID LISTENING"));
    report_reference(3,1,1); on_subscribed(7,&ok,NULL,&client.chars[3]);
    enabled[0]=1; cccd_om.len=2;
    on_cccd_read(7,&ok,&cccd_attr,&client.chars[3]);
    assert(strstr(output,"writes_accepted=2 readbacks_confirmed=1"));
    assert(bridge_ready);
    notify(0x30,left,8); assert(strstr(output,"Report ID=2") && !strstr(output,"KEY: LEFT"));
    assert(forwarded==0);
    notify(0x34,left,8); assert(strstr(output,"Report ID=1") && strstr(output,"KEY: LEFT"));
    assert(forwarded==1 && !memcmp(last_forwarded,left,8));
    assert(client.input_count==1 && client.notification_count==2);
    hid_client_stop(7); notify(0x34,left,8); assert(!output[0] && client.notification_count==2);
    assert(!bridge_ready && forwarded==1);
    /* Mouse uses verified ID/type/layout, including the captured 20-byte padding. */
    reset_test(); discover(); report_reference(2,3,1);
    assert(client.mouse_layout);
    uint8_t motion[20]={1,0xff,2,0};
    notify(0x30,motion,20); assert(mouse_forwarded==1 && !memcmp(last_mouse,motion,4));
    motion[0]=0;
    notify(0x30,motion,4); assert(mouse_forwarded==2 && !memcmp(last_mouse,motion,4));
    motion[19]=1; notify(0x30,motion,20); assert(mouse_forwarded==2);
    motion[19]=0; notify(0x30,motion,19); assert(mouse_forwarded==2);
    client.mouse_layout=false; notify(0x30,motion,20); assert(mouse_forwarded==2);
    client.mouse_layout=true; client.chars[2].report_type=2;
    notify(0x30,motion,20); assert(mouse_forwarded==2);
    client.chars[2].report_type=1; client.chars[2].report_id=91;
    notify(0x30,motion,20); assert(mouse_forwarded==2);
    client.chars[2].report_id=3;
    hid_client_stop(7); notify(0x30,motion,20); assert(mouse_forwarded==2);
    /* A second connection must discover fresh handles and subscribe again. */
    assert(!client.started);
    discover(); assert(client.active && client.started && !client.notification_count);
    report_reference(2,3,1); notify(0x30,motion,20); assert(mouse_forwarded==3);
    puts("PASS: security gate, map, report selection, Protocol Mode, CCCD readback, notifications and disconnect guards");
}
