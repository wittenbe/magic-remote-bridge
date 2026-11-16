#pragma once
#include <stdint.h>

// User settings, persisted in NVS. Changed via the console: "set <name> <value>", shown with "settings".
typedef struct {
    float gain;         // pointer base speed, px per gyro LSB per report (~16.9 LSB per °/s)
    float accel;        // pointer acceleration strength (0 = off)
    uint32_t tv_sleep;  // TV mode: hold back the TV's sleep command until idle this many s (0 = pass it on)
    uint32_t pc_sleep;  // PC mode: send the sleep command after this many s idle (0 = never)
    uint32_t smooth;    // PC mode: spread each report's pointer motion over this many ms (0 = send at once)
    float dead;         // PC mode: soft dead zone on the rotation rate, gyro LSB (~16.9 per °/s)
                        // New fields go at the end: settings_load() keeps defaults for fields an older blob lacks.
} settings_t;

extern settings_t settings;

void settings_load(void);  // after nvs_flash_init()
// Returns 0 if the name is known and the value was stored.
int settings_set(const char *name, float value);
void settings_print(void);
