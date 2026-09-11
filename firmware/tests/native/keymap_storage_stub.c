#include <string.h>
#include "../../main/keymap.h"
static uint8_t flash_blob[KEYMAP_LENGTH];
static bool saved;
bool test_storage_failure;
bool keymap_storage_load(uint8_t payload[KEYMAP_LENGTH])
{ if (!saved) { return false; } memcpy(payload, flash_blob, KEYMAP_LENGTH); return true; }
bool keymap_storage_save(const uint8_t payload[KEYMAP_LENGTH])
{ if (test_storage_failure) { return false; } memcpy(flash_blob, payload, KEYMAP_LENGTH); saved = true; return true; }
