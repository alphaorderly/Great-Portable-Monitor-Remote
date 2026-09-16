#include "macro.h"
#include <string.h>
static uint8_t stored[MACRO_SLOTS][MACRO_MAX_SIZE];
static uint16_t lengths[MACRO_SLOTS],revisions[MACRO_SLOTS];
bool test_macro_storage_failure;
bool macro_storage_load(unsigned slot,uint8_t *p,uint16_t *n,uint16_t *rev)
{
    if(slot>=MACRO_SLOTS || !lengths[slot] || *n<lengths[slot])return false;
    *n=lengths[slot];*rev=revisions[slot];memcpy(p,stored[slot],*n);return true;
}
bool macro_storage_save(unsigned slot,const uint8_t *p,uint16_t n,uint16_t rev)
{
    if(test_macro_storage_failure || slot>=MACRO_SLOTS || n>MACRO_MAX_SIZE)return false;
    memcpy(stored[slot],p,n);lengths[slot]=n;revisions[slot]=rev;return true;
}
