# Architecture

```
                       BLE (bridge = central)                BLE (bridge = peripheral, "LGE MR25GB")
  Magic Remote  ◄──────────────────────────────►  ESP32-S3  ◄──────────────────────────────────►  LG TV
   (MR25GB)                                          │
                                                     └── USB (TinyUSB) ──► PC: keyboard + mouse + media keys
                                                                               + serial console
```

The bridge sits between the remote and the TV. Toward the remote it is the central (like the TV would be);
toward the TV it pretends to be the remote. Because it relays the real remote's answers, nothing about the
LG-specific protocol has to be reimplemented: the TV's initialisation and commands go to the real remote, its
calibration data and reports go back to the TV.

## Modes

| | TV mode (default) | PC mode |
|---|---|---|
| Remote → TV (FD, F9, FE) | forwarded | not forwarded |
| TV → remote (F9 Output writes) | forwarded | blocked |
| USB HID | idle | keys, wheel, gyro pointer |
| Sleep command `02` | from the TV (optionally held back) | sent by the bridge after idle |

Holding **red** for 0.8 s switches modes. The red key itself is never forwarded directly: a short press is
replayed to the TV on release, so red keeps its TV function. On entering PC mode the TV receives the remote's
"idle" event (0x803F) and hides its cursor; on returning it receives "active" (0x803E).

## Firmware modules (`firmware/main`)

| File | Role |
|---|---|
| `main.c` | startup, glue between the modules, console commands, queue for TV writes |
| `remote.c` | central: scan for a remote in pairing mode, pair, remember its address, subscribe to F9/FD/FE, fallback initialisation, keep a connect pending while the remote sleeps, detect lost keys |
| `tv.c` | peripheral: GATT copy of the remote, advertising, Security Request, MTU exchange, accepts re-pairing |
| `pc.c` | mode switch, red-key handling, PC mode (key map, roll-compensated pointer with acceleration), idle sleep |
| `usb.c` | TinyUSB composite device: CDC console + HID (keyboard, mouse, consumer control); reboot into download mode |
| `settings.c` | user settings in NVS |

Stack: ESP-IDF 5.4, NimBLE (dual role, bonds and CCCDs persisted in NVS), esp_tinyusb.

## Registration with the TV

The TV only pairs after the IR "register" code (Home+Back+OK) and then takes the first LG advertiser it sees.
The bridge therefore advertises its copy **whenever the TV isn't connected**, also during registration:

1. User presses Home+Back (LED blinks), then OK pointing at the TV.
2. The remote starts advertising in pairing mode and sends the IR code; the TV opens its pairing window.
3. The TV finds the bridge's advertising and pairs with it. Meanwhile the bridge connects to the real
   remote, which nobody else is competing for.
4. TV writes that arrive before the remote is ready are queued and forwarded once it is.
5. If the TV's initialisation hasn't reached the remote within 8 s, the bridge sends a recorded copy
   so the remote doesn't give up.

After that, both bonds live in NVS. A TV reconnecting after a reboot or firmware update gets its old
subscriptions back (NimBLE restores them), so no new registration is needed.

## Sleep

The remote streams 100 reports/s while connected. Normally the TV sends `02` when it's idle; the remote then
drops the link and reconnects on the next key press (the bridge keeps an LE connect pending for that).

- TV mode: the TV's `02` is forwarded immediately, or held back until the remote has been idle for `tvsleep`
  seconds (any activity cancels it; the TV asks again later).
- PC mode: TV commands are blocked, so the bridge sends `02` itself after `pcsleep` seconds of idle.

## Pointer (PC mode)

Gyro bias is re-estimated whenever the remote is still for 0.5 s. The rotation is projected onto the vertical
(from the low-pass-filtered accelerometer) and onto the horizontal axis across the pointing direction, so the
cursor moves correctly even when the remote is held tilted. Speed is `gain` px per gyro LSB per report; above
~18 °/s the factor grows with the rotation rate (`accel`), up to 1 + 4·accel. The pointer freezes for 150 ms
around clicks.
