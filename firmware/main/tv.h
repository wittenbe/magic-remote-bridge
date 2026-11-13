#pragma once
#include <stdbool.h>
#include <stdint.h>

// Peripheral side: we pretend to be the MR25GB toward the TV (GATT mirror + identical advertising).

enum tv_report { TV_F9_IN, TV_FD_IN, TV_FE_IN };

// TV wrote to one of the remote's writable report characteristics; forward it (remote handle given).
typedef void (*tv_write_cb_t)(uint16_t remote_handle, const uint8_t *data, uint16_t len);
// TV (re)subscribed to FD: the relay replays the connect event 0x803E.
typedef void (*tv_fd_subscribed_cb_t)(void);

void tv_init(tv_write_cb_t on_write, tv_fd_subscribed_cb_t on_fd_subscribed);  // before nimble host start
void tv_start_advertising(void);  // after sync, once the real remote is bonded
void tv_stop_advertising(void);   // the real remote is gone for good (forgotten)
void tv_notify(enum tv_report r, const uint8_t *data, uint16_t len);
bool tv_connected(void);
void tv_status(void);
