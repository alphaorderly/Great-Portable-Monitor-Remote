#include "macro.h"
#include <string.h>
static const uint8_t buttons[14]={0x66,0x69,0x6a,0x6b,0x52,0x51,0x50,0x4f,0x28,0xf1,0x4a,0x76,0x80,0x81};
static struct { uint16_t length, revision; uint8_t data[MACRO_MAX_SIZE]; } slots[MACRO_SLOTS];
static struct { bool active; uint8_t slot; uint16_t token, revision, length, received; uint32_t deadline; uint8_t data[MACRO_MAX_SIZE]; } upload;
static uint8_t response[64];
static struct { bool active, release; uint8_t slot, repeats; uint16_t pos, text_end; uint32_t due; unsigned phase; } run;
static uint16_t u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1]<<8); }
static void put16(uint8_t *p,uint16_t n) { p[0]=n; p[1]=n>>8; }
static bool due(uint32_t now,uint32_t deadline) { return (int32_t)(now-deadline)>=0; }
static uint32_t between(uint16_t low,uint16_t high)
{
    uint32_t range=(uint32_t)high-low+1, limit=UINT32_MAX-(UINT32_MAX%range), value;
    do { value=macro_random(); } while(value>=limit);
    return low+value%range;
}
bool macro_ascii(uint8_t c,uint8_t *key,uint8_t *mods)
{
    *mods=0;
    if(c>='a' && c<='z') { *key=4+c-'a';return true; }
    if(c>='A' && c<='Z') { *key=4+c-'A';*mods=2;return true; }
    if(c>='1' && c<='9') { *key=0x1e + c-'1';return true; }
    if(c=='0') { *key=0x27;return true; }
    if(c=='\n') { *key=0x28;return true; }
    if(c=='\t') { *key=0x2b;return true; }
    if(c==' ') { *key=0x2c;return true; }
    const char *plain="-=[]\\;\x27`,./", *shift="_+{}|:\x22~<>?";
    const uint8_t codes[]={0x2d,0x2e,0x2f,0x30,0x31,0x33,0x34,0x35,0x36,0x37,0x38};
    for(unsigned i=0;i<sizeof(codes);++i) {
        if(c==(uint8_t)plain[i] || c==(uint8_t)shift[i]) { *key=codes[i];*mods=c==(uint8_t)shift[i]?2:0;return true; }
    }
    const char *digits="!@#$%^&*()";
    for(unsigned i=0;i<10;++i)if(c==(uint8_t)digits[i]) { *key=i==9?0x27:0x1e + i;*mods=2;return true; }
    return false;
}
bool macro_validate(const uint8_t *p,unsigned n)
{
    if(n<20 || n>MACRO_MAX_SIZE || memcmp(p,"MC\x01\x00",4) || p[4]>1 || !p[5] || p[5]>100 ||
       u16(p+6)<10 || u16(p+6)>500 || u16(p+8)>u16(p+10) || u16(p+10)>5000 ||
       u16(p+12)>u16(p+14) || u16(p+14)>60000 || u16(p+16)!=n-20 || p[18] || p[19])return false;
    bool text=false;
    for(unsigned i=20;i<n;) {
        unsigned op=p[i++];
        if(op==1) {
            if(i+2>n)return false;
            unsigned len=u16(p+i);i+=2;
            if(!len || i+len>n)return false;
            while(len--) { uint8_t key,mods;if(!macro_ascii(p[i++],&key,&mods))return false; }
            text=true;
        } else if(op==2) {
            if(i+4>n || u16(p+i)>u16(p+i+2) || u16(p+i+2)>60000)return false;
            i+=4;
        } else return false;
    }
    return !p[4] || text;
}
int macro_slot(uint8_t button,unsigned mode)
{
    if(mode>1 || button==0x6a || (mode==1 && button==0x4a))return -1;
    for(unsigned i=0;i<14;++i)if(buttons[i]==button)return (int)(mode*14+i);
    return -1;
}
bool macro_enabled(unsigned slot) { return slot<MACRO_SLOTS && slots[slot].data[4]; }
bool macro_active(void) { return run.active || run.release; }
void macro_cancel(void)
{
    upload.active=false; /* Partial uploads must not survive stop/host changes. */
    run.release=run.active || run.release;run.active=false;
}
void macro_start(unsigned slot,uint32_t now)
{
    if(macro_active() || !macro_enabled(slot))return;
    run.active=true;run.slot=slot;run.repeats=slots[slot].data[5];run.pos=20;run.text_end=20;run.due=now;run.phase=0;
}
void macro_tick(uint32_t now)
{
    uint8_t packet[8]={0};
    if(run.release) { if(macro_send_keyboard(packet))run.release=false;return; }
    if(!run.active || !due(now,run.due))return;
    const uint8_t *p=slots[run.slot].data;
    if(run.phase==1) {
        if(!macro_send_keyboard(packet))return;
        run.phase=0;run.due=now+between(u16(p+8),u16(p+10));return;
    }
    if(run.pos<run.text_end) {
        macro_ascii(p[run.pos],&packet[2],&packet[0]);
        if(!macro_send_keyboard(packet))return;
        ++run.pos;run.phase=1;run.due=now+u16(p+6);return;
    }
    if(run.pos==slots[run.slot].length) {
        if(--run.repeats) { run.pos=run.text_end=20;run.due=now+between(u16(p+12),u16(p+14)); }
        else run.active=false;
        return;
    }
    unsigned op=p[run.pos++];
    if(op==1) { unsigned n=u16(p+run.pos);run.pos+=2;run.text_end=run.pos+n; }
    else { run.due=now+between(u16(p+run.pos),u16(p+run.pos+2));run.pos+=4; }
}
void macro_init(void)
{
    memset(&run,0,sizeof(run));memset(&upload,0,sizeof(upload));memset(response,0,sizeof(response));
    for(unsigned i=0;i<MACRO_SLOTS;++i) {
        uint16_t n=MACRO_MAX_SIZE,rev=0;
        if(macro_storage_load(i,slots[i].data,&n,&rev) && macro_validate(slots[i].data,n) &&
           (!slots[i].data[4] || macro_slot(buttons[i%14],i/14)>=0)) {
            slots[i].length=n;slots[i].revision=rev;
        } else {
            memset(&slots[i],0,sizeof(slots[i]));slots[i].length=20;
            memcpy(slots[i].data,"MC\x01\x00",4);slots[i].data[5]=1;
            put16(slots[i].data+6,40);put16(slots[i].data+8,80);put16(slots[i].data+10,180);
        }
    }
}
void macro_response(uint8_t p[64]) { memcpy(p,response,64); }
void macro_request(const uint8_t p[64],uint32_t now)
{
    memcpy(response,p,16);memset(response+16,0,48);response[5]=MACRO_INVALID;response[14]=0;
    if(memcmp(p,"MX\x01",3) || p[5] || p[15])return;
    unsigned cmd=p[3],slot=p[4],offset=u16(p+10),total=u16(p+12),len=p[14];
    uint16_t token=u16(p+6),rev=u16(p+8);
    if(cmd==MACRO_STOP) { macro_cancel();response[5]=MACRO_OK;return; }
    if(cmd==MACRO_STATUS) {
        for(unsigned i=0;i<MACRO_SLOTS;++i)if(macro_enabled(i))response[16+i/8]|=1u<<(i%8);
        response[20]=run.active?run.slot:255;response[14]=5;response[5]=MACRO_OK;return;
    }
    if(slot>=MACRO_SLOTS)return;
    put16(response+8,slots[slot].revision);
    if(cmd==MACRO_READ) {
        unsigned n=slots[slot].length;
        if(offset>n)return;
        unsigned size=n-offset;if(size>48)size=48;
        memcpy(response+16,slots[slot].data+offset,size);response[14]=size;put16(response+12,n);response[5]=MACRO_OK;return;
    }
    if(rev!=slots[slot].revision) { response[5]=MACRO_CONFLICT;return; }
    if(cmd==MACRO_BEGIN) {
        if(total<20 || total>MACRO_MAX_SIZE || offset || len)return;
        upload.active=true;upload.slot=slot;upload.token=token;upload.revision=rev;upload.length=total;
        upload.received=0;upload.deadline=now+10000;response[5]=MACRO_OK;return;
    }
    if(!upload.active || due(now,upload.deadline) || upload.slot!=slot || upload.token!=token ||
       upload.revision!=rev || upload.length!=total)return;
    if(cmd==MACRO_CHUNK) {
        if(!len || len>48 || offset!=upload.received || offset+len>total)return;
        memcpy(upload.data+offset,p+16,len);upload.received+=len;upload.deadline=now+10000;response[5]=MACRO_OK;return;
    }
    if(cmd!=MACRO_COMMIT || len || offset || upload.received!=total || !macro_validate(upload.data,total) ||
       (upload.data[4] && macro_slot(buttons[slot%14],slot/14)<0))return;
    if(macro_active()) { response[5]=MACRO_BUSY;return; }
    uint16_t next=rev+1;
    if(!macro_storage_save(slot,upload.data,total,next)) { response[5]=MACRO_STORAGE_ERROR;return; }
    memcpy(slots[slot].data,upload.data,total);slots[slot].length=total;slots[slot].revision=next;
    upload.active=false;put16(response+8,next);response[5]=MACRO_OK;
}
