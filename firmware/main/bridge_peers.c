#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "host/ble_store.h"
#include "bridge_peers.h"

bool bridge_peer_equal(const ble_addr_t *a, const ble_addr_t *b)
{
    return (a->type & 1) == (b->type & 1) && !memcmp(a->val, b->val, 6);
}

bool bridge_peer_load(const char *role, ble_addr_t *peer)
{
    nvs_handle_t handle;
    if (nvs_open("bridge_peers", NVS_READONLY, &handle) != ESP_OK) { return false; }
    uint8_t data[7]; size_t size = sizeof(data);
    esp_err_t rc = nvs_get_blob(handle, role, data, &size);
    nvs_close(handle);
    if (rc != ESP_OK || size != sizeof(data) || data[0] > BLE_ADDR_RANDOM) { return false; }
    peer->type = data[0]; memcpy(peer->val, data + 1, 6);
    return true;
}

bool bridge_peer_save(const char *role, const ble_addr_t *peer)
{
    ble_addr_t previous;
    if (bridge_peer_load(role, &previous) && bridge_peer_equal(&previous, peer)) { return true; }
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("bridge_peers", NVS_READWRITE, &handle);
    if (rc == ESP_OK) {
        uint8_t data[7] = {peer->type & 1}; memcpy(data + 1, peer->val, 6);
        rc = nvs_set_blob(handle, role, data, sizeof(data));
        if (rc == ESP_OK) { rc = nvs_commit(handle); }
        nvs_close(handle);
    }
    if (rc != ESP_OK) { ESP_LOGW("REMOTE_STATUS", "peer metadata save failed: role=%s rc=%d", role, rc); }
    return rc == ESP_OK;
}

bool bridge_peer_bonded(const ble_addr_t *peer, bool central)
{
    struct ble_store_key_sec key = {.peer_addr = *peer};
    key.peer_addr.type &= 1;
    struct ble_store_value_sec value = {0};
    int rc = central ? ble_store_read_peer_sec(&key, &value) : ble_store_read_our_sec(&key, &value);
    return rc == 0 && value.ltk_present;
}

bool bridge_peer_forget(const char *role, const ble_addr_t *peer)
{
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("bridge_peers", NVS_READWRITE, &handle);
    if (rc != ESP_OK) { return false; }
    uint8_t data[7]; size_t size = sizeof(data);
    rc = nvs_get_blob(handle, role, data, &size);
    if (rc == ESP_ERR_NVS_NOT_FOUND) { rc = ESP_OK; }
    else if (rc == ESP_OK) {
        if (size != sizeof(data)) { rc = ESP_ERR_INVALID_SIZE; }
        else if ((data[0] & 1) == (peer->type & 1) && !memcmp(data + 1, peer->val, 6)) {
            rc = nvs_erase_key(handle, role);
            if (rc == ESP_OK) { rc = nvs_commit(handle); }
        }
    }
    nvs_close(handle);
    if (rc != ESP_OK) { ESP_LOGW("REMOTE_STATUS", "peer metadata removal failed: role=%s rc=%d", role, rc); }
    return rc == ESP_OK;
}
