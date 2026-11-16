#pragma once
#include <stdbool.h>
#include <stdint.h>

// PC-mode key map: remote button code → USB HID output. Persisted in NVS and edited over the console
// ("keymap", "map", "unmap", "keymap reset"), which is what tools/config.html uses.

typedef enum { OUT_NONE, OUT_KEY, OUT_CONSUMER, OUT_MOUSE } out_type_t;

typedef struct {
    uint16_t code;   // remote button code (0x80xx)
    uint8_t type;    // out_type_t
    uint8_t mods;    // OUT_KEY: modifier bits as in the HID report (0 = Ctrl, 1 = Shift, 2 = Alt, 3 = GUI, 4-7 right)
    uint16_t usage;  // keyboard usage / consumer usage / mouse button bits; OUT_KEY with 0 = modifiers only
} keymap_entry_t;

#define KEYMAP_MAX 64

void keymap_load(void);  // after nvs_flash_init()
// Copies the entry for `code`; false if the button is unmapped.
bool keymap_find(uint16_t code, keymap_entry_t *out);
// Adds or replaces the entry (type OUT_NONE removes it) and saves. 0 on success, -1 if the map is full.
int keymap_set(const keymap_entry_t *e);
void keymap_reset(void);
// Prints the map as "keymap begin", one "map ..." line per entry (same syntax as the command), "keymap end".
void keymap_print(void);
void keymap_print_entry(const keymap_entry_t *e);
