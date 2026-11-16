# LG Magic Remote MR25GB: BLE protocol

Reverse-engineered with BlueZ/btmon and verified by relaying between a real MR25GB and an LG OLED TV (webOS,
2025 model). Everything here was observed on one remote and one TV. The older MR20 is documented in
[sirfragles/lgmagic](https://github.com/sirfragles/lgmagic).

## Overview

- Chip: Realtek (PnP ID vendor 0x005D, product 0x8762), BLE peripheral with a public address.
- HID over GATT, but the report descriptor has **no standard usages**: three vendor-defined byte arrays on the
  Consumer page. No operating system turns it into a keyboard/mouse by itself.
- The pointer is **not** computed in the remote: it streams a raw 6-axis IMU (gyro + accelerometer) at 100 Hz,
  and the TV does the sensor fusion.
- Power is IR only. When the remote has no BLE link, all keys are also sent as IR.

## Advertising

Legacy `ADV_IND` (connectable, scannable), about every 35 ms while advertising:

| Field | Bytes |
|---|---|
| Flags | `02 01 05` (LE Limited Discoverable, BR/EDR not supported) |
| Manufacturer data | `17 ff c4 00` + ASCII `SCD 21.2,BA 35,webOS` (company 0x00C4 = LG Electronics) |
| Scan response | `0b 09` + `LGE MR25GB` (complete local name) |

The remote advertises:
- after **Home+Back** (held until the LED blinks) followed by **OK**: "pairing mode", undirected, for up to ~15 s;
- after a key press while it has no valid bond;
- when bonded and asleep, a key press makes it reconnect to its central (directed; a pending LE connect to its
  address catches it).

## Pairing and security

- The remote sends an SMP Security Request right after connecting and expects encryption.
- Pairing: IO capability NoInputNoOutput, AuthReq Bonding without MITM, **LE Legacy only** (no Secure
  Connections), Just Works. Key distribution: initiator EncKey, responder EncKey + IdKey.
- Connection parameters: it accepts a 30–50 ms interval at connect time (a 10 ms interval in the CONNECT_IND
  made every connection fail with 0x3E), then requests its preferred **10 ms interval, latency 4, 3 s timeout**
  via an L2CAP connection parameter update about 1 s after connecting. It also starts an MTU exchange (247).
- The remote does no GATT discovery on its central; it only acts as a server.

## GATT table

Declaration handles (value handle = declaration + 1). Values are from one unit.

| Handle | UUID | Properties | Value / notes |
|---|---|---|---|
| **0x0001** | **1801 GATT** | | |
| 0x0002 | 2A05 Service Changed | indicate | |
| **0x0005** | **1800 GAP** | | |
| 0x0006 | 2A00 Device Name | read | `LGE MR25GB\0` (NUL-terminated) |
| 0x0008 | 2A01 Appearance | read | `c1 03` (0x03C1, HID keyboard) |
| 0x000a | 2A04 Preferred Connection Parameters | read | `08 00 08 00 04 00 2c 01` |
| 0x000c | 2AA6 Central Address Resolution | read | `00` |
| **0x000e** | **1812 HID** | | |
| 0x000f | 2A4A HID Information | read | `00 00 00 01` (bcdHID 0, flags RemoteWake) |
| 0x0011 | 2A4C HID Control Point | write-no-rsp | |
| 0x0013 | 2A4E Protocol Mode | read, write-no-rsp | `01` (report) |
| 0x0015 | 2A4B Report Map | read | 71 bytes, below; External Report Reference → 0x2A19 |
| 0x0018 | 2A4D Report **F9 Input** | read, notify | read answers ATT error 0x0A |
| 0x001d | 2A4D Report **F9 Feature** | read, write, write-no-rsp | read answers ATT error 0x0A |
| 0x0021 | 2A4D Report **FD Input** | read, write, notify | read returns empty; the main data report |
| 0x0026 | 2A4D Report **FE Input** | read, notify | read answers ATT error 0x0A |
| 0x002b | 2A4D Report **F9 Output** | read, write, write-no-rsp | not in the report map; command channel |
| **0x002f** | **180F Battery** | | 2A19 Battery Level, read/notify |
| **0x0033** | **180A Device Information** | | manufacturer `Realtek BT`, model `Model Nbr 0.9`, other strings `RTKBee…`, System ID `00 01 02 00 00 03 04 05`, PnP ID `01 5d 00 62 87 01 00` |
| **0x0046** | **1813 Scan Parameters** | | 2A4F (write-no-rsp), 2A31 (notify) |
| **0x004c** | **LG vendor** `0000d1ff-3c17-d293-8e48-14fe2e4da212` | | a001 (write-no-rsp, notify), a002 (write) — **never used by the TV** |
| **0x0052** | **Realtek OTA** `0000d0ff-3c17-d293-8e48-14fe2e4da212` | | ffd1…fff2; firmware update service, **do not write** |

Each report characteristic has a Report Reference descriptor (`f9 01`, `f9 03`, `fd 01`, `fe 01`, `f9 02`) and a
User Description (`HID_0xF9_IN_USER_SESCR`, `HID_0xF9_FEATURE_USER_SESCR`, `HID_0xFD_IN_USER_SESCR`,
`HID_0xFE_IN_USER_SESCR`, `HID_0xF9_OUT_USER_SESCR`).

Report map:
```
05 0c 09 01 a1 01 85 fd 95 1e 75 08 15 00 26 ff 00 81 00 c0
05 0c 09 01 a1 01 85 f9 95 1e 75 08 15 00 26 ff 00 81 00 95 c8 75 08 15 00 26 ff 00 b1 00 c0
05 0c 09 01 a1 01 85 fe 95 7c 75 08 15 00 26 ff 00 81 00 c0
```
Three Consumer Control collections: FD input (30 bytes declared), F9 input (30) + feature (200), FE input (124).
The declared sizes don't match the actual FD notifications (19 bytes).

## FD report (input, 19 bytes, 100 Hz while connected)

| Bytes | Content |
|---|---|
| 0 | slowly varying (0xBD–0xE3), unknown |
| 1 | sequence counter, +1 per report |
| 2–3 | status word, unknown |
| 4–9 | gyro X, Y, Z (int16, big-endian) |
| 10–15 | accelerometer X, Y, Z (int16, big-endian) |
| 16–17 | button code (uint16, big-endian), `0x0000` = none; repeated while held |
| 18 | wheel (int8): +1 per notch up, −1 per notch down, one report per notch |

### IMU

- Accelerometer ≈ 4096 LSB/g; lying flat, face up: Z ≈ −4096.
- Gyro ≈ **16.9 LSB per °/s** (±2000 °/s range); at rest bias ±1.5 LSB, noise ≈ 1 LSB.
- Axes: Y along the remote (tip), X across, Z perpendicular to the face. Gyro and accelerometer share one
  frame (checked against each other). For a pointer: with `up = accel / |accel|` (low-pass filtered),
  `yaw = ω·up`, `pitch = ω·(Y × up)`, then `dx = −k·yaw`, `dy = −k·pitch`. This is roll-compensated.
  The sign of `up` was settled empirically (the readings alone fit both conventions, which give mirrored
  directions).

### Button codes

| Key | Code | Key | Code |
|---|---|---|---|
| OK / wheel click | 0x8044 | Vol+ / Vol− | 0x8002 / 0x8003 |
| Up / Down | 0x8040 / 0x8041 | Mute | 0x8009 |
| Left / Right | 0x8007 / 0x8006 | Ch+ / Ch− | 0x8000 / 0x8001 |
| Back | 0x8028 | Digits 0–9 | 0x8010 + n |
| Home | 0x807C | Red | 0x8072 |
| Settings | 0x8043 | Green / Yellow / Blue | 0x8071 / 0x8063 / 0x8061 |
| "…" | 0x8045 | Voice | 0x808B |
| Guide / List | 0x80AB | Input | 0x80A1 |
| Netflix | 0x8056 | Prime Video | 0x805C |
| Disney+ | 0x8031 | Rakuten TV | 0x8049 |
| Google Assistant | 0x8048 | Alexa | 0x800C |

The MR25GB has no Play/Pause key. Long-press functions (printed with ◎ on the remote) have no codes of their
own: the code repeats while the key is held and the TV measures the time.

Pseudo-codes sent by the remote: **0x803E** "active" (after its central subscribes, and when waking),
**0x803F** "idle" (after a sleep command, before it drops the link). Power has no BLE code.

Between 0x803F and the next 0x803E the remote may keep sending FD reports (for key presses) with the **IMU off**:
the gyro and accelerometer fields repeat their last values unchanged. A live gyro always shows ~1 LSB of noise,
so identical readings over several reports mean frozen data, not a remote at rest.

## F9 command channel

The central writes commands to **F9 Output** (Write Command); the remote answers on **F9 Input**. The first byte is
a command ID that the answer echoes.

Initialisation, sent by the TV after subscribing to F9/FD/FE:

| Central → remote | Remote → central |
|---|---|
| `17` | two 125-byte blocks starting `17 01 …` / `17 00 …`, float32 LE values: most likely IMU calibration |
| `13` | `14 18 52 05 38 0c 00 01 08 03 e0 01` (version/info?) |
| `74`, `6d` | `6d 00` |
| `6c 01` | `6c 00` |
| `19` | `19 34 23 ff ff ff ff ff ff ff ff ff` when unconfigured, `19 34 23 ff ff ff 4d 0f 4f ff ff ff` after `62` |
| `90` | `90 00 00` |
| `62 00 4d 0f 4f 03 …` (98 bytes, configuration) | `62 00` (only sent when `19` reports "unconfigured") |

The order varies between sessions. The 125-byte answers need an ATT MTU of at least 128: with the default
MTU of 23 they arrive truncated, buttons still work, but **the TV shows no cursor**.

Runtime commands:

| Command | Effect |
|---|---|
| `01` | pointer active (the TV sends it when the user starts using the remote) |
| `02` (often twice) | idle/sleep: the remote answers FD 0x803F and drops the link about 15 s later unless used again |

A remote paired in pairing mode that receives no initialisation gives up after ~12 s (F9 In `00`, `40 00`,
FD 0x803F, disconnect). It keeps its key and reconnects later.

FE (124-byte input) was never seen; it is probably the microphone channel.

## How the TV pairs a remote

- The TV accepts a new remote only after receiving the **IR "register" code** that the remote emits on
  Home+Back+OK. Advertising alone (IR covered) is ignored. It does not filter by address, name position or flags.
- After the IR code, the TV connects to the first LG-looking advertiser and pairs.
- TV as central: SMP Pairing Request IO KeyboardDisplay, AuthReq Bonding + MITM + **Secure Connections**;
  with a NoInputNoOutput peripheral this becomes LE SC Just Works. It waits for the peripheral's Security
  Request before pairing.
- It does **not** start an MTU exchange; the peripheral has to.
- GATT: reads PnP ID, HID Information, Report Map and all Report References, enables notifications on
  F9/FD/FE (not battery, not the vendor service), then runs the F9 initialisation.
- A bonded TV reconnects immediately when its remote advertises and expects its earlier notification
  subscriptions to still be active (it doesn't re-subscribe).
