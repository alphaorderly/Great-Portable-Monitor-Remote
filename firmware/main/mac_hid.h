#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Called only on the NimBLE host task, except init before starting that task. */
int mac_hid_init(void);
int mac_hid_advertise(uint8_t own_addr_type);
void mac_hid_on_reset(void);
void mac_hid_remote_ready(bool ready);
void mac_hid_keyboard(const uint8_t keyboard[8]);
void mac_hid_mouse(const uint8_t mouse[4]);
void mac_hid_remote_mouse(const uint8_t mouse[4]);
void mac_hid_sensor_mode(bool cursor);
