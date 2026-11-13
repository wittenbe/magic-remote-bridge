# Troubleshooting and implementation notes

## Symptoms

| Symptom | Cause / fix |
|---|---|
| TV: "registration failed" | The bridge didn't get the remote or the TV didn't pair. Press OK again; check the console (`status`). Make sure the antenna is attached. |
| Keys work on the TV, but no cursor | The TV link runs with ATT MTU 23, so the remote's calibration answers arrive truncated. The firmware requests MTU 247; `status` shows the current value. |
| Only IR works (TV reacts only with a clear line of sight, no cursor) | The remote isn't connected to the bridge. Wake it with a key press; if it doesn't reconnect, the remote may have lost its key: `forget`, then pair again. |
| First seconds after a firmware update feel odd | The TV may have reconnected while the remote was asleep and run its initialisation later. Settles within seconds. |
| Pointer moves in the wrong direction | The sign convention is set in `pc.c` (`up` in `pointer()`), see [protocol](protocol.md#imu). |
| Remote battery drains quickly | Something keeps it from sleeping: check `pcsleep`/`tvsleep`, or whether the TV's `02` reaches it. |
| `flash.py`: "not in download mode" | Hold B, tap R, release B, run again. |

## Implementation notes

These are the non-obvious things that cost time during development.

- **Connection parameters toward the remote:** request 30–50 ms at connect time; the remote switches to 10 ms
  itself. A 10 ms interval in the connect request fails with 0x3E (connection failed to be established).
  Use a long supervision timeout (3 s).
- **NimBLE (ESP-IDF 5.4):** call `ble_gap_security_initiate()` on `BLE_GAP_EVENT_LINK_ESTAB`, not on `CONNECT`;
  on CONNECT it is silently dropped, and the TV never starts pairing. Copy `event->disc.addr` before calling
  `ble_gap_disc_cancel()`, the cancel reuses the event buffer.
- **The TV doesn't negotiate the MTU.** The peripheral must start the exchange.
- **Racing the TV for the remote** during registration fails often. Advertising the fake before the remote is
  grabbed lets the TV pick the fake.
- **Without the TV's sleep command** the remote streams indefinitely; in PC mode the bridge must send `02`.
- **TinyUSB and download mode:** the USB PHY stays routed to USB-OTG across `esp_restart()`; switch it back to
  USB-Serial/JTAG before forcing download boot (see `usb_reboot_to_download()`).
- **Logging:** NimBLE INFO logging over the USB console slows the host task enough to drop reports
  (100 → 70 reports/s). Keep it at WARNING.

## Linux prototype (BlueZ)

See [prototype/README.md](../prototype/README.md). BlueZ-specific pitfalls: the HID plugin (`hog`) blocks D-Bus
writes to the remote's HID service; `JustWorksRepairing = never` (the default) silently stalls a re-pairing TV;
the TV's CCCD state is lost when the GATT application is re-registered.
