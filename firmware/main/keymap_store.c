#include "nvs.h"
#include "esp_log.h"
#include "keymap.h"

bool keymap_storage_load(uint8_t payload[KEYMAP_LENGTH])
{
    nvs_handle_t handle;
    if (nvs_open("remote_keymap", NVS_READONLY, &handle) != ESP_OK) { return false; }
    size_t length = KEYMAP_LENGTH;
    esp_err_t rc = nvs_get_blob(handle, "map_v3", payload, &length);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        length = KEYMAP_LENGTH;
        rc = nvs_get_blob(handle, "map_v2", payload, &length);
    }
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        length = KEYMAP_LENGTH;
        rc = nvs_get_blob(handle, "map_v1", payload, &length);
    }
    nvs_close(handle);
    return rc == ESP_OK && length == KEYMAP_LENGTH;
}

bool keymap_storage_save(const uint8_t payload[KEYMAP_LENGTH])
{
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("remote_keymap", NVS_READWRITE, &handle);
    if (rc == ESP_OK) {
        rc = nvs_set_blob(handle, "map_v3", payload, KEYMAP_LENGTH);
        if (rc == ESP_OK) { rc = nvs_commit(handle); }
        nvs_close(handle);
    }
    if (rc != ESP_OK) { ESP_LOGE("MAC_HID", "keymap storage failed: rc=%d", rc); }
    return rc == ESP_OK;
}
