#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "macro.h"
static bool ready=true;
static uint8_t packets[2048][8];
static unsigned count;
extern bool test_macro_storage_failure;
static uint32_t random_value;
uint32_t macro_random(void) { return random_value; }
bool macro_send_keyboard(const uint8_t p[8])
{ if(!ready)return false;assert(count<2048);memcpy(packets[count++],p,8);return true; }
static void put16(uint8_t *p,unsigned n) { p[0]=n;p[1]=n>>8; }
static uint8_t request[64],reply[64];
static void command(unsigned cmd,unsigned slot,unsigned token,unsigned rev,unsigned offset,unsigned total,const uint8_t *data,unsigned n)
{
    memset(request,0,64);memcpy(request,"MX\x01",3);request[3]=cmd;request[4]=slot;
    put16(request+6,token);put16(request+8,rev);put16(request+10,offset);put16(request+12,total);request[14]=n;
    if(n)memcpy(request+16,data,n);
    macro_request(request,100);macro_response(reply);
}
static void save(unsigned slot,unsigned rev,const uint8_t *p,unsigned n)
{
    command(MACRO_BEGIN,slot,123,rev,0,n,NULL,0);assert(!reply[5]);
    for(unsigned i=0;i<n;i+=48) { unsigned size=n-i;if(size>48)size=48;
        command(MACRO_CHUNK,slot,123,rev,i,n,p+i,size);assert(!reply[5]); }
    command(MACRO_COMMIT,slot,123,rev,0,n,NULL,0);
}
int main(void)
{
    macro_init();assert(!macro_active());
    uint8_t key,mods;
    for(unsigned c=32;c<=126;++c)assert(macro_ascii(c,&key,&mods));
    assert(!macro_ascii(0,&key,&mods) && !macro_ascii(127,&key,&mods));
    assert(macro_ascii('A',&key,&mods) && key==4 && mods==2);
    assert(macro_ascii('?',&key,&mods) && key==0x38 && mods==2);
    assert(macro_ascii('"',&key,&mods) && key==0x34 && mods==2);
    assert(macro_ascii('\n',&key,&mods) && key==0x28 && !mods);
    assert(macro_ascii('\t',&key,&mods) && key==0x2b && !mods);
    uint8_t blob[64]={'M','C',1,0,1,2,40,0,80,0,180,0,100,0,100,0,12,0,0,0,
                      1,1,0,'A',2,50,0,50,0,1,0,0};
    blob[30]=1;blob[32]='a';put16(blob+16,13);
    assert(macro_validate(blob,33));
    for(unsigned n=0;n<33;++n)assert(!macro_validate(blob,n));
    save(8,0,blob,33);assert(!reply[5] && reply[8]==1);
    macro_init();assert(macro_enabled(8));
    command(MACRO_STATUS,0,9,0,0,0,NULL,0);assert(reply[14]==5 && reply[17]==1 && reply[20]==255);
    command(MACRO_BEGIN,8,123,0,0,33,NULL,0);assert(reply[5]==MACRO_CONFLICT);
    command(MACRO_BEGIN,8,123,1,0,33,NULL,0);assert(!reply[5]);
    command(MACRO_CHUNK,8,124,1,0,33,blob,33);assert(reply[5]==MACRO_INVALID);
    command(MACRO_COMMIT,8,123,1,0,33,NULL,0);assert(reply[5]==MACRO_INVALID);
    command(MACRO_CHUNK,8,123,1,1,33,blob,32);assert(reply[5]==MACRO_INVALID);
    command(MACRO_CHUNK,8,123,1,0,33,blob,33);assert(!reply[5]);
    test_macro_storage_failure=true;
    command(MACRO_COMMIT,8,123,1,0,33,NULL,0);assert(reply[5]==MACRO_STORAGE_ERROR);
    test_macro_storage_failure=false;
    command(MACRO_READ,8,7,0,0,0,NULL,0);assert(reply[8]==1 && !memcmp(reply+16,blob,33));
    macro_start(8,1000);macro_tick(1000); /* parse text */
    ready=false;macro_tick(2000);assert(count==0); /* blocked press cannot advance */
    ready=true;macro_tick(2100);assert(count==1 && packets[0][2]==4 && packets[0][0]==2);
    macro_tick(2139);assert(count==1);macro_tick(2140);assert(count==2 && !packets[1][0]);
    macro_tick(2219);assert(count==2);macro_tick(2220); /* wait 50 */
    macro_tick(2269);assert(count==2);macro_tick(2270); /* parse text */
    macro_tick(2271);assert(count==3 && packets[2][2]==4 && !packets[2][0]);
    macro_cancel();ready=false;macro_tick(2300);assert(macro_active());
    ready=true;macro_tick(2301);assert(!macro_active() && count==4 && !packets[3][2]);
    /* Repeated letters have distinct release reports; repeat is bounded. */
    count=0;macro_start(8,0);
    for(unsigned now=0;now<5000;++now)macro_tick(now);
    assert(!macro_active() && count==8);
    for(unsigned i=0;i<count;++i)assert((i%2)?packets[i][2]==0:packets[i][2]==4);
    save(2,0,blob,33);assert(reply[5]==MACRO_INVALID); /* reserved cursor */
    save(24,0,blob,33);assert(reply[5]==MACRO_INVALID); /* mouse HOME */
    /* Wraparound deadlines work across the 32-bit millisecond boundary. */
    count=0;macro_start(8,UINT32_MAX-20);macro_tick(UINT32_MAX-20);macro_tick(UINT32_MAX-19);
    assert(count==1);macro_tick(19);assert(count==1);macro_tick(20);assert(count==2);
    macro_cancel();macro_tick(21);assert(!macro_active());
    uint8_t pair[25]={'M','C',1,0,1,1,10,0,80,0,180,0,0,0,0,0,5,0,0,0,1,2,0,'a','a'};
    save(0,0,pair,25);assert(!reply[5]);
    random_value=100;count=0;macro_start(0,1000);macro_tick(1000);macro_tick(1001);
    macro_tick(1011);assert(count==2);macro_tick(1190);assert(count==2);
    macro_tick(1191);assert(count==3); /* sampled upper endpoint: 180ms */
    macro_cancel();macro_tick(1192);
    command(MACRO_BEGIN,0,123,1,0,25,NULL,0);assert(!reply[5]);
    command(MACRO_CHUNK,0,123,1,0,25,pair,25);assert(!reply[5]);
    macro_cancel(); /* host switch/stop invalidates a fully staged upload */
    command(MACRO_COMMIT,0,123,1,0,25,NULL,0);assert(reply[5]==MACRO_INVALID);
    command(MACRO_BEGIN,0,123,1,0,25,NULL,0);assert(!reply[5]);
    command(MACRO_CHUNK,0,123,1,0,25,pair,25);assert(!reply[5]);
    request[3]=MACRO_COMMIT;request[14]=0;
    macro_request(request,10100);macro_response(reply);assert(reply[5]==MACRO_INVALID);
    puts("PASS: ASCII/Shift, validation, atomic macro commit, conflict/corruption/storage failure, bounded repeats, timing/backpressure/cancellation/wraparound");
    return 0;
}
