#pragma once
#include <stdbool.h>
#include <stdint.h>

// Mode switching (red long press) and PC mode (USB HID), same logic as prototype/pc_mode.py.

typedef enum { MODE_TV, MODE_PC } bridge_mode_t;

typedef struct {
    void (*to_tv)(const uint8_t *report, uint16_t len);  // send an FD report to the TV
    void (*to_remote)(const uint8_t *data, uint16_t len);  // write F9 Output on the real remote
} pc_io_t;

void pc_init(const pc_io_t *io);
// Every FD report from the remote. In TV mode it is forwarded (red key held back / replayed), in PC mode it
// becomes USB HID input.
void pc_on_fd(const uint8_t *report, uint16_t len);
bridge_mode_t pc_mode(void);
void pc_toggle(void);
// The TV wrote its sleep command (02). Returns true if it should be forwarded now; otherwise pc holds it back
// until the remote has been idle for settings.tv_sleep seconds.
bool pc_tv_sleep_request(void);
void pc_status(void);
