#include <string.h>
#include "input_codec.h"
#include "keymap.h"

void input_keyboard_map(const uint8_t remote[8], uint8_t keyboard[8], uint8_t *consumer)
{
    uint8_t mouse;
    input_map(remote, 0, 0, keyboard, consumer, &mouse);
}

void input_map(const uint8_t remote[8], unsigned mode, uint8_t native_buttons,
               uint8_t keyboard[8], uint8_t *consumer, uint8_t *mouse_buttons)
{
    memset(keyboard, 0, 8);
    *consumer = 0;
    *mouse_buttons = native_buttons & 6;
    unsigned count = 2;
    for (unsigned i = 2; i < 9; ++i) {
        uint8_t button = i < 8 ? remote[i] : (native_buttons & 1 ? 0x28 : 0);
        /* The remote also inserts 6A alongside arrows when restarting its sensor.
         * Never emit a host key for it, including previously saved mappings. */
        if (button == 0x6a) { continue; }
        const keymap_entry_t *entry = keymap_find_mode(button, mode);
        if (!entry || entry->kind == KEYMAP_NONE) { continue; }
        if (entry->kind == KEYMAP_VOLUME) { *consumer |= entry->key; continue; }
        if (entry->kind == KEYMAP_MOUSE) { *mouse_buttons |= entry->key; continue; }
        const uint8_t usage = entry->key;
        keyboard[0] |= entry->modifiers;
        bool duplicate = false;
        for (unsigned j = 2; j < count; ++j) { duplicate |= keyboard[j] == usage; }
        if (!duplicate && count < 8) { keyboard[count++] = usage; }
    }
}

int input_sensor_mode(const uint8_t *remote, size_t length)
{
    if (length != 20) { return -1; }
    if (!memcmp(remote, "nanosic sensor start", 20)) { return 1; }
    if (!memcmp(remote, "nanosic sensor stop ", 20)) { return 0; }
    return -1;
}

bool input_mouse_decode(const uint8_t *remote, size_t length, uint8_t mouse[4])
{
    /* The captured ID 3 has a four-byte descriptor and sixteen zero padding bytes. */
    if (length != 4 && length != 20) { return false; }
    if (remote[0] & 0xf8) { return false; }
    for (size_t i = 4; i < length; ++i) { if (remote[i]) { return false; } }
    /* Descriptor axes range from -127 to 127. */
    for (unsigned i = 1; i < 4; ++i) { if (remote[i] == 0x80) { return false; } }
    memcpy(mouse, remote, 4);
    return true;
}
