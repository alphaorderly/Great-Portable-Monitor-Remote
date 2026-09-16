#pragma once
#include <stdbool.h>
#include <stdint.h>
void hid_input_remote_ready(bool ready);
void hid_input_keyboard(const uint8_t keyboard[8]);
void hid_input_mouse(const uint8_t mouse[4]);
void hid_input_remote_mouse(const uint8_t mouse[4]);
void hid_input_sensor_mode(bool cursor);


#define REPORT_LENGTH 12
#define REPORT_ID 1
#define FLAG_REMOTE_READY 1
#define FLAG_INPUT_EVENT 2
typedef struct {
    uint8_t id, length, data[8];
    bool dirty;
} native_report_t;
/* Input owns the USB report slots. */
extern native_report_t native[3];
int hid_input_init(void);
int hid_input_start(void);
void hid_input_reset(void);
void hid_input_connected(void);
void hid_input_publish(void);
/* Called on the input owner task when USB makes forward progress. */
void hid_input_output_ready(void);
void hid_input_suspend(bool enabled);
void hid_input_mapping_changed(void);
const uint8_t *hid_input_vendor_report(void);

void hid_input_macro_stop(void);
