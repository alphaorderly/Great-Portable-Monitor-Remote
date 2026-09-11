#include <string.h>
#include "keymap.h"

#define DEFAULT_ENTRIES { \
    {0x66,1,0x6b,0}, {0x69,1,0x6c,0}, {0x6a,0,0,0}, {0x6b,1,0x6e,0}, \
    {0x52,1,0x52,0}, {0x51,1,0x51,0}, {0x50,1,0x50,0}, {0x4f,1,0x4f,0}, \
    {0x28,1,0x28,0}, {0xf1,1,0x29,0}, {0x4a,1,0x4a,0}, {0x76,1,0x2b,8}, \
    {0x80,2,1,0}, {0x81,2,2,0} }
static const keymap_entry_t defaults[KEYMAP_COUNT] = DEFAULT_ENTRIES;
static keymap_entry_t entries[KEYMAP_MODES][KEYMAP_COUNT] = {DEFAULT_ENTRIES, DEFAULT_ENTRIES};
static uint16_t revision;
_Static_assert(8 + KEYMAP_MODES * KEYMAP_COUNT * 2 == KEYMAP_LENGTH, "compact wire layout");

static bool valid_entry(const keymap_entry_t *e)
{
    switch (e->kind) {
    case KEYMAP_NONE: return !e->key && !e->modifiers;
    case KEYMAP_KEY: return e->key >= 4 && e->key <= 0x73 && !(e->modifiers & 0xf0);
    case KEYMAP_VOLUME: return (e->key == 1 || e->key == 2) && !e->modifiers;
    case KEYMAP_MOUSE: return (e->key == 1 || e->key == 2 || e->key == 4) && !e->modifiers;
    default: return false;
    }
}

static bool decode(const uint8_t *p, size_t length, keymap_entry_t out[KEYMAP_MODES][KEYMAP_COUNT])
{
    if (length != KEYMAP_LENGTH || memcmp(p, "KM\x02\x0e", 4) || p[6] != KEYMAP_MODES || p[7]) { return false; }
    for (unsigned m = 0; m < KEYMAP_MODES; ++m) {
        for (unsigned i = 0; i < KEYMAP_COUNT; ++i) {
            unsigned offset = 8 + 2 * (m * KEYMAP_COUNT + i);
            uint16_t v = p[offset] | (p[offset + 1] << 8);
            keymap_entry_t e = {defaults[i].button, (v >> 7) & 3, v & 0x7f, (v >> 9) & 15};
            if ((v & 0xe000) || !valid_entry(&e) || (e.button == 0x6a && v)) { return false; }
            out[m][i] = e;
        }
    }
    return true;
}

void keymap_init(void)
{
    for (unsigned m = 0; m < KEYMAP_MODES; ++m) { memcpy(entries[m], defaults, sizeof(defaults)); }
    entries[1][6] = (keymap_entry_t){0x50, KEYMAP_MOUSE, 2, 0};
    entries[1][8] = (keymap_entry_t){0x28, KEYMAP_MOUSE, 1, 0};
    revision = 0;
    uint8_t p[KEYMAP_LENGTH];
    keymap_entry_t loaded[KEYMAP_MODES][KEYMAP_COUNT];
    if (!keymap_storage_load(p)) { return; }
    if (decode(p, sizeof(p), loaded)) {
        memcpy(entries, loaded, sizeof(entries));
    } else if (!memcmp(p, "KM\x01\x0e", 4) && !p[6] && !p[7]) {
        /* Migrate existing assignments in RAM; persist atomically on next save.
         * Cursor-mode OK was a native left click before profiles existed. */
        for (unsigned i = 0; i < KEYMAP_COUNT; ++i) {
            keymap_entry_t e; memcpy(&e, p + 8 + i * 4, 4);
            if (e.button != defaults[i].button || e.kind > KEYMAP_VOLUME || !valid_entry(&e)) { return; }
            loaded[0][i] = loaded[1][i] = e.button == 0x6a ? defaults[i] : e;
        }
        loaded[1][8] = (keymap_entry_t){0x28, KEYMAP_MOUSE, 1, 0};
        memcpy(entries, loaded, sizeof(entries));
    } else { return; }
    revision = p[4] | (p[5] << 8);
}

const keymap_entry_t *keymap_find_mode(uint8_t button, unsigned mode)
{
    if (mode >= KEYMAP_MODES) { return NULL; }
    for (unsigned i = 0; i < KEYMAP_COUNT; ++i) { if (entries[mode][i].button == button) { return &entries[mode][i]; } }
    return NULL;
}

const keymap_entry_t *keymap_find(uint8_t button) { return keymap_find_mode(button, 0); }

void keymap_read(uint8_t p[KEYMAP_LENGTH])
{
    memcpy(p, "KM\x02\x0e", 4);
    p[4] = revision & 0xff; p[5] = revision >> 8; p[6] = KEYMAP_MODES; p[7] = 0;
    for (unsigned m = 0; m < KEYMAP_MODES; ++m) {
        for (unsigned i = 0; i < KEYMAP_COUNT; ++i) {
            const keymap_entry_t *e = &entries[m][i];
            uint16_t v = e->key | (e->kind << 7) | (e->modifiers << 9);
            unsigned offset = 8 + 2 * (m * KEYMAP_COUNT + i);
            p[offset] = v & 0xff; p[offset + 1] = v >> 8;
        }
    }
}

int keymap_apply(const uint8_t *p, size_t length)
{
    keymap_entry_t loaded[KEYMAP_MODES][KEYMAP_COUNT];
    if (!decode(p, length, loaded)) { return KEYMAP_INVALID; }
    if ((uint16_t)(p[4] | (p[5] << 8)) != revision) { return KEYMAP_CONFLICT; }
    uint8_t saved[KEYMAP_LENGTH]; memcpy(saved, p, sizeof(saved));
    uint16_t next = revision + 1;
    saved[4] = next & 0xff; saved[5] = next >> 8;
    if (!keymap_storage_save(saved)) { return KEYMAP_STORAGE_ERROR; }
    memcpy(entries, loaded, sizeof(entries)); revision = next;
    return KEYMAP_OK;
}
