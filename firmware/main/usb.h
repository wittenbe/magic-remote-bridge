#pragma once
#include <stdbool.h>
#include <stdint.h>

// Composite USB device: CDC-ACM (log output + line commands) and one HID interface with
// keyboard (report 1), mouse (report 2) and consumer control (report 3).

typedef void (*usb_line_cb_t)(const char *line);

void usb_init(usb_line_cb_t on_line);
void usb_hid_start(void);  // called by usb_init()
bool usb_hid_mounted(void);

// HID keyboard usage (0x04..0xE7). 0xE0..0xE7 are the modifiers (Ctrl/Shift/Alt/GUI) and are sent as such.
void usb_key(uint8_t usage, bool down);
// All HID calls only queue and return immediately; a USB task sends the reports.
// Mouse: buttons bit 0 = left, bit 1 = right, bit 2 = middle.
void usb_mouse_buttons(uint8_t buttons);
// Relative motion in (sub-)pixels, spread over the next `smooth` ms (see settings).
void usb_mouse_motion(float dx, float dy, int wheel);
void usb_mouse_move(int dx, int dy, int wheel);
extern volatile uint32_t usb_mouse_reports;
// Consumer control usage (e.g. 0xE9 volume up), sent as press or release.
void usb_consumer(uint16_t usage, bool down);

// Reboot into the ROM download mode (USB-Serial/JTAG), so esptool can flash without the BOOT button.
void usb_reboot_to_download(void);
