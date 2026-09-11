#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "input_log.h"

typedef struct {
    unsigned sequence, length, offset, rx_ms;
    int id;
    uint16_t handle;
    uint8_t data[32], count;
    bool indication;
} log_entry_t;
static QueueHandle_t queue;
static unsigned dropped;
static bool have_mouse_buttons;
static uint8_t last_mouse_buttons;
static unsigned last_mouse_log_ms;

static void logger(void *arg)
{
    (void)arg;
    log_entry_t entry;
    for (;;) {
        if (xQueueReceive(queue, &entry, portMAX_DELAY) != pdTRUE) { continue; }
        unsigned lost = __atomic_exchange_n(&dropped, 0, __ATOMIC_RELAXED);
        if (lost) { ESP_LOGW("REMOTE_INPUT", "Serial log queue overflow: dropped_lines=%u; BLE input continues", lost); }
        char hex[97] = {0};
        for (unsigned i = 0; i < entry.count; ++i) {
            snprintf(hex + 3 * i, sizeof(hex) - 3 * i, i + 1 == entry.count ? "%02X" : "%02X ", entry.data[i]);
        }
        ESP_LOGI("REMOTE_INPUT", "#%04u id=%d h=%04X len=%u off=%u ind=%u rx=%u | %s",
                 entry.sequence, entry.id, entry.handle, entry.length, entry.offset,
                 entry.indication, entry.rx_ms, hex);
    }
}

bool input_log_init(void)
{
    queue = xQueueCreate(64, sizeof(log_entry_t));
    if (!queue) { return false; }
    if (xTaskCreate(logger, "input_log", 3072, NULL, 2, NULL) != pdPASS) {
        vQueueDelete(queue); queue = NULL; return false;
    }
    return true;
}

void input_log_submit(unsigned sequence, int id, uint16_t handle, unsigned length,
                      unsigned offset, bool indication, const uint8_t *data, unsigned count)
{
    unsigned now = esp_log_timestamp();
    /* Keep button edges and every keyboard/sensor report. Sample continuous
     * motion at 10 Hz so 115200-baud UART logging cannot swamp the log queue. */
    if (id == 3 && offset == 0 && count >= 4) {
        bool changed = !have_mouse_buttons || last_mouse_buttons != data[0];
        have_mouse_buttons = true;
        last_mouse_buttons = data[0];
        if (!changed && (unsigned)(now - last_mouse_log_ms) < 100) { return; }
        last_mouse_log_ms = now;
    }
    log_entry_t entry = {.sequence = sequence, .id = id, .handle = handle, .length = length,
                         .offset = offset, .indication = indication, .rx_ms = now};
    if (!queue || count > sizeof(entry.data)) { __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED); return; }
    entry.count = count;
    memcpy(entry.data, data, count);
    /* Never wait for UART while running a NimBLE callback. */
    if (xQueueSend(queue, &entry, 0) != pdTRUE) { __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED); }
}
