#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MACRO_REPORT_ID 6
#define MACRO_PACKET_SIZE 64
#define MACRO_SLOTS 28
#define MACRO_MAX_SIZE 512
#define MACRO_HEADER_SIZE 20
#define MACRO_CHUNK_SIZE 48
enum { MACRO_READ=1, MACRO_BEGIN, MACRO_CHUNK, MACRO_COMMIT, MACRO_STOP, MACRO_STATUS };
enum { MACRO_OK, MACRO_INVALID, MACRO_CONFLICT, MACRO_STORAGE_ERROR, MACRO_BUSY };
void macro_init(void);
bool macro_enabled(unsigned slot);
int macro_slot(uint8_t button, unsigned mode);
bool macro_validate(const uint8_t *data, unsigned length);
bool macro_ascii(uint8_t ch, uint8_t *usage, uint8_t *modifiers);
void macro_request(const uint8_t request[64], uint32_t now);
void macro_response(uint8_t response[64]);
/* Owner-task scheduler: never sleep, advance only after accepted HID reports. */
bool macro_active(void);
void macro_start(unsigned slot, uint32_t now);
void macro_cancel(void);
void macro_tick(uint32_t now);
/* Platform adapters. Keyboard report transmission must wait for an empty USB
 * queue so random intervals are not collapsed into a burst after backpressure. */
bool macro_send_keyboard(const uint8_t report[8]);
uint32_t macro_random(void);
bool macro_storage_load(unsigned slot, uint8_t *data, uint16_t *length, uint16_t *revision);
bool macro_storage_save(unsigned slot, const uint8_t *data, uint16_t length, uint16_t revision);
