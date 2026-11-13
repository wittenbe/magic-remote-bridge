#pragma once
#include <stdbool.h>
#include <stdint.h>

// Central side: the real LG Magic Remote (MR25GB). Handles are the remote's fixed GATT value handles
// (see docs/protocol.md).
#define REMOTE_H_F9_IN   0x0019
#define REMOTE_H_F9_FEAT 0x001e
#define REMOTE_H_FD_IN   0x0022
#define REMOTE_H_FE_IN   0x0027
#define REMOTE_H_F9_OUT  0x002c

typedef void (*remote_notify_cb_t)(uint16_t handle, const uint8_t *data, uint16_t len);
typedef void (*remote_state_cb_t)(bool ready);  // ready = bonded + encrypted + subscribed

void remote_init(remote_notify_cb_t on_notify, remote_state_cb_t on_state);
void remote_start(void);  // call from the NimBLE sync callback
bool remote_ready(void);
bool remote_bonded(void);
// Forward a write (from the TV) to the remote; Write Command, like the TV does.
int remote_write(uint16_t handle, const uint8_t *data, uint16_t len);
void remote_forget(void);          // drop the bond and scan for a remote in pairing mode
void remote_handshake_seen(void);  // the TV's own handshake reached the remote: skip our fallback
// Tell the bridge which bonded peer is the remote ("AA:BB:CC:DD:EE:FF"); normally recorded when it pairs.
int remote_set_addr(const char *addr);
void remote_status(void);
