#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/input_codec.h"

int main(void)
{
    assert(input_sensor_mode((const uint8_t *)"nanosic sensor start",20)==1);
    assert(input_sensor_mode((const uint8_t *)"nanosic sensor stop ",20)==0);
    assert(input_sensor_mode((const uint8_t *)"nanosic sensor start",19)==-1);
    assert(input_sensor_mode((const uint8_t *)"nanosic sensor other",20)==-1);
    const uint8_t inputs[] = {0x4f,0x50,0x51,0x52,0x28,0xf1,0x4a,0x76,0x66,0x69,0x6a,0x6b};
    const uint8_t outputs[] = {0x4f,0x50,0x51,0x52,0x28,0x29,0x4a,0x2b,0x6b,0x6c,0,0x6e};
    uint8_t keyboard[8], volume;
    for (unsigned i=0; i<sizeof(inputs); ++i) {
        uint8_t raw[8]={0,0,inputs[i]};
        input_keyboard_map(raw,keyboard,&volume);
        assert(keyboard[2]==outputs[i] && keyboard[0]==(inputs[i]==0x76?8:0) && !volume);
        memset(raw,0,8); input_keyboard_map(raw,keyboard,&volume);
        assert(!memcmp(raw,keyboard,8) && !volume);
    }
    uint8_t cursor_combo[8]={0,0,0x52,0x6a};
    input_keyboard_map(cursor_combo,keyboard,&volume);
    assert(keyboard[2]==0x52 && !keyboard[3] && !keyboard[0] && !volume);
    uint8_t combo[8]={0,0,0x80,0x81,0x50,0x50,0xab,0x76};
    input_keyboard_map(combo,keyboard,&volume);
    assert(volume==3 && keyboard[0]==8 && keyboard[2]==0x50 && keyboard[3]==0x2b && !keyboard[4]);
    uint8_t motion[20]={5,0xff,0x7f,0x81}, decoded[4];
    assert(input_mouse_decode(motion,20,decoded) && !memcmp(motion,decoded,4));
    assert(input_mouse_decode(motion,4,decoded));
    for (unsigned n=0; n<24; ++n) {
        if (n!=4 && n!=20) { assert(!input_mouse_decode(motion,n,decoded)); }
    }
    for (unsigned n=4; n<20; ++n) {
        motion[n]=1; assert(!input_mouse_decode(motion,20,decoded)); motion[n]=0;
    }
    motion[0]=8; assert(!input_mouse_decode(motion,20,decoded)); motion[0]=0;
    motion[1]=0x80; assert(!input_mouse_decode(motion,20,decoded));
    puts("PASS: native keyboard/consumer mapping, releases, mouse signs, padding and invalid formats");
}
