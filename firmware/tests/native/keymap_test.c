#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/keymap.h"
#include "../../main/input_codec.h"
extern bool test_storage_failure;
static void put(uint8_t *p,unsigned mode,unsigned index,unsigned kind,unsigned key,unsigned mods)
{
    unsigned offset=8+2*(mode*KEYMAP_COUNT+index), v=key|(kind<<7)|(mods<<9);
    p[offset]=v; p[offset+1]=v>>8;
}
int main(int argc,char **argv)
{
    uint8_t original[64],payload[64],actual[64];
    keymap_init(); keymap_read(original);
    if (argc==2 && !strcmp(argv[1],"--defaults")) {
        for (unsigned i=0;i<64;++i) { printf("%02x",original[i]); } puts(""); return 0;
    }
    assert(!memcmp(original,"KM\x03\x0e",4) && original[4]==0 && original[6]==2);
    memcpy(payload,original,64);
    put(payload,0,0,KEYMAP_KEY,4,8); /* Power -> Command+A, normal only. */
    put(payload,1,6,KEYMAP_MOUSE,2,0); /* Cursor LEFT -> right click. */
    assert(keymap_apply(payload,63)==KEYMAP_INVALID);
    assert(keymap_apply(payload,64)==KEYMAP_OK);
    uint8_t input[8]={0,0,0x66},keyboard[8],consumer,mouse;
    input_keyboard_map(input,keyboard,&consumer); assert(keyboard[0]==8 && keyboard[2]==4 && !consumer);
    input_map(input,1,0,keyboard,&consumer,&mouse); assert(keyboard[0]==0 && keyboard[2]==0x6b);
    input[2]=0x50;
    input_map(input,0,0,keyboard,&consumer,&mouse); assert(keyboard[2]==0x50 && !mouse);
    input_map(input,1,0,keyboard,&consumer,&mouse); assert(!keyboard[2] && mouse==2);
    input_map(input,1,1,keyboard,&consumer,&mouse); assert(!keyboard[2] && mouse==3); /* held LEFT plus native OK */
    memset(input,0,8); input_map(input,1,0,keyboard,&consumer,&mouse); assert(!mouse && !keyboard[2]);
    keymap_read(actual); assert(actual[4]==1);
    assert(keymap_apply(payload,64)==KEYMAP_CONFLICT);
    keymap_init(); keymap_read(payload); assert(!memcmp(payload,actual,64));
    put(payload,1,6,KEYMAP_MOUSE,4,0); test_storage_failure=true;
    assert(keymap_apply(payload,64)==KEYMAP_STORAGE_ERROR);
    keymap_read(payload); assert(!memcmp(payload,actual,64));
    test_storage_failure=false;
    const unsigned offsets[]={0,2,3,6,7,8,9,36,37};
    for (unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i) {
        memcpy(payload,actual,64); payload[offsets[i]]=0xff;
        assert(keymap_apply(payload,64)==KEYMAP_INVALID);
        keymap_read(payload); assert(!memcmp(payload,actual,64));
    }
    for (unsigned m=0;m<2;++m) {
        memcpy(payload,actual,64); put(payload,m,2,KEYMAP_KEY,0x6d,8);
        assert(keymap_apply(payload,64)==KEYMAP_INVALID);
        memcpy(payload,actual,64); put(payload,m,6,KEYMAP_MOUSE,3,0);
        assert(keymap_apply(payload,64)==KEYMAP_INVALID);
        memcpy(payload,actual,64); put(payload,m,6,KEYMAP_MOUSE,2,8);
        assert(keymap_apply(payload,64)==KEYMAP_INVALID);
    }
    /* Native OK can also map to a shortcut and coexists with keyboard keys. */
    memcpy(payload,actual,64); put(payload,1,8,KEYMAP_KEY,4,8);
    assert(keymap_apply(payload,64)==KEYMAP_OK);
    input_map(input,1,1,keyboard,&consumer,&mouse); assert(keyboard[0]==8 && keyboard[2]==4 && !mouse);
    input_map(input,1,0,keyboard,&consumer,&mouse); assert(!keyboard[0] && !keyboard[2]);
    /* HID Ctrl/Shift/Alt/GUI are independent on Windows as well as macOS. */
    for (unsigned mods=0;mods<16;++mods) {
        keymap_read(payload); put(payload,0,0,KEYMAP_KEY,0x2b,mods);
        assert(keymap_apply(payload,64)==KEYMAP_OK);
        input[2]=0x66;
        input_map(input,0,0,keyboard,&consumer,&mouse);
        assert(keyboard[0]==mods && keyboard[2]==0x2b && !consumer && !mouse);
        input[2]=0;
        input_map(input,0,0,keyboard,&consumer,&mouse);
        assert(!keyboard[0] && !keyboard[2]);
    }
    /* Read the independent original v1 fixture; migrate custom mappings and revision. */
    FILE *f=fopen("../shared/fixtures/keymap-v1-default.hex","r"); assert(f);
    for(unsigned i=0;i<64;++i) { unsigned value; assert(fscanf(f,"%2x",&value)==1); payload[i]=value; } fclose(f);
    payload[4]=42; payload[10]=5; payload[11]=8;
    payload[17]=KEYMAP_KEY; payload[18]=0x6d; payload[19]=8;
    assert(keymap_storage_save(payload)); keymap_init(); keymap_read(actual);
    assert(actual[2]==3 && actual[4]==42);
    for(unsigned m=0;m<2;++m) {
        assert(keymap_find_mode(0x66,m)->key==5 && keymap_find_mode(0x66,m)->modifiers==8);
        assert(keymap_find_mode(0x6a,m)->kind==KEYMAP_NONE);
    }
    assert(keymap_find_mode(0x28,1)->kind==KEYMAP_MOUSE && keymap_find_mode(0x28,1)->key==1);
    assert(keymap_apply(payload,64)==KEYMAP_INVALID); /* Old GUI cannot overwrite two profiles. */
    assert(keymap_find_mode(0x4a,1)->kind==KEYMAP_NONE);
    assert(keymap_mouse_speed()==KEYMAP_SPEED_DEFAULT);
    /* v2 keeps custom profiles and revision, reserves HOME only in mouse mode. */
    f=fopen("../shared/fixtures/keymap-v2-default.hex","r"); assert(f);
    for(unsigned i=0;i<64;++i) { unsigned value; assert(fscanf(f,"%2x",&value)==1); payload[i]=value; } fclose(f);
    payload[4]=77; put(payload,0,10,KEYMAP_KEY,5,8); put(payload,1,0,KEYMAP_KEY,6,4);
    assert(keymap_storage_save(payload)); keymap_init(); keymap_read(actual);
    assert(actual[2]==3 && actual[4]==77 && actual[7]==KEYMAP_SPEED_DEFAULT);
    assert(keymap_find_mode(0x4a,0)->key==5 && keymap_find_mode(0x4a,0)->modifiers==8);
    assert(keymap_find_mode(0x4a,1)->kind==KEYMAP_NONE);
    assert(keymap_find_mode(0x66,1)->key==6 && keymap_find_mode(0x66,1)->modifiers==4);
    assert(keymap_apply(payload,64)==KEYMAP_INVALID); /* Reject older GUI writes. */
    memcpy(payload,actual,64); put(payload,1,10,KEYMAP_MOUSE,1,0);
    assert(keymap_apply(payload,64)==KEYMAP_INVALID);
    input[2]=0x4a;
    input_map(input,0,0,keyboard,&consumer,&mouse); assert(keyboard[2]==5 && keyboard[0]==8);
    input_map(input,1,0,keyboard,&consumer,&mouse); assert(!keyboard[2] && !keyboard[0] && !mouse && !consumer);
    for(unsigned speed=1;speed<=KEYMAP_SPEED_MAX;++speed) {
        keymap_read(payload); payload[7]=speed;
        assert(keymap_apply(payload,64)==KEYMAP_OK);
        keymap_init(); keymap_read(actual);
        assert(actual[7]==speed && keymap_mouse_speed()==speed);
    }
    keymap_read(payload); payload[7]=0; assert(keymap_apply(payload,64)==KEYMAP_INVALID);
    payload[7]=KEYMAP_SPEED_MAX+1; assert(keymap_apply(payload,64)==KEYMAP_INVALID);
    payload[7]=1; test_storage_failure=true;
    assert(keymap_apply(payload,64)==KEYMAP_STORAGE_ERROR);
    assert(keymap_mouse_speed()==KEYMAP_SPEED_MAX);
    test_storage_failure=false;
    puts("PASS: speed persistence and rollback, reserved HOME, v2 migration");
    puts("PASS: independent profiles, mouse/keyboard overlap and release, atomic save, rollback, revision, v1 migration");
}
