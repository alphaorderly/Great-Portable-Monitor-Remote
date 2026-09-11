#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEYMAP_COUNT 14
#define KEYMAP_MODES 2
#define KEYMAP_LENGTH 64
#define KEYMAP_REPORT_ID 5
/* Quarter-speed units: 1 = 0.25x, 4 = 1x, 12 = 3x. */
#define KEYMAP_SPEED_DEFAULT 4
#define KEYMAP_SPEED_MAX 12
enum { KEYMAP_NONE = 0, KEYMAP_KEY = 1, KEYMAP_VOLUME = 2, KEYMAP_MOUSE = 3 };
typedef struct { uint8_t button, kind, key, modifiers; } keymap_entry_t;
enum { KEYMAP_OK = 0, KEYMAP_INVALID, KEYMAP_CONFLICT, KEYMAP_STORAGE_ERROR };

void keymap_init(void);
const keymap_entry_t *keymap_find(uint8_t button);
const keymap_entry_t *keymap_find_mode(uint8_t button, unsigned mode);
void keymap_read(uint8_t payload[KEYMAP_LENGTH]);
uint8_t keymap_mouse_speed(void);
int keymap_apply(const uint8_t *payload, size_t length);
/* Storage adapters keep the protocol testable independently of flash. */
bool keymap_storage_load(uint8_t payload[KEYMAP_LENGTH]);
bool keymap_storage_save(const uint8_t payload[KEYMAP_LENGTH]);
