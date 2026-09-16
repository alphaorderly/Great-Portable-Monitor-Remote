#include "macro.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>
bool macro_storage_load(unsigned slot,uint8_t *data,uint16_t *length,uint16_t *revision)
{
    nvs_handle_t h;
    if(nvs_open("remote_macro",NVS_READONLY,&h)!=ESP_OK)return false;
    char key[12];snprintf(key,sizeof(key),"slot%u",slot);
    uint8_t blob[MACRO_MAX_SIZE+2];size_t n=sizeof(blob);
    esp_err_t rc=nvs_get_blob(h,key,blob,&n);nvs_close(h);
    if(rc!=ESP_OK || n<2 || n-2>*length)return false;
    *revision=blob[0]|((uint16_t)blob[1]<<8);*length=n-2;memcpy(data,blob+2,*length);return true;
}
bool macro_storage_save(unsigned slot,const uint8_t *data,uint16_t length,uint16_t revision)
{
    if(length>MACRO_MAX_SIZE)return false;
    nvs_handle_t h;
    if(nvs_open("remote_macro",NVS_READWRITE,&h)!=ESP_OK)return false;
    char key[12];snprintf(key,sizeof(key),"slot%u",slot);
    uint8_t blob[MACRO_MAX_SIZE+2];blob[0]=revision;blob[1]=revision>>8;memcpy(blob+2,data,length);
    esp_err_t rc=nvs_set_blob(h,key,blob,length+2);
    if(rc==ESP_OK)rc=nvs_commit(h);
    nvs_close(h);return rc==ESP_OK;
}
