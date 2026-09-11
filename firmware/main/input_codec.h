#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Consumer bits: bit 0 volume up, bit 1 volume down. */
void input_keyboard_map(const uint8_t remote[8], uint8_t keyboard[8], uint8_t *consumer);
void input_map(const uint8_t remote[8], unsigned mode, uint8_t native_buttons,
               uint8_t keyboard[8], uint8_t *consumer, uint8_t *mouse_buttons);
/* Exact captured vendor ID 91 status; -1 means unknown, 0 normal, 1 cursor. */
int input_sensor_mode(const uint8_t *remote, size_t length);
bool input_mouse_decode(const uint8_t *remote, size_t length, uint8_t mouse[4]);
