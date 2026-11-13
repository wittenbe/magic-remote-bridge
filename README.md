# Magic Remote Bridge

Use one **LG Magic Remote (MR25GB)** for both an **LG TV** and a **PC**, and switch between them with a button.

An ESP32-S3 sits between the remote and the TV over Bluetooth LE and is plugged into the PC over USB. In TV mode
it relays everything, so the TV works exactly as before (pointer, keys, wheel). Hold the red key and the remote
becomes the PC's mouse and keyboard; hold it again to go back to the TV.

```
Magic Remote ◄──BLE──► ESP32-S3 ◄──BLE──► LG TV
                           │
                           └──USB──► PC (keyboard, mouse, media keys)
```

## Status

Working with an MR25GB and a 2025 LG OLED (webOS): TV relay including pointer, PC mode on Windows, sleep/wake.

## Documentation

- [Setup](docs/setup.md): hardware, ESP-IDF, building and flashing
- [Usage](docs/usage.md): pairing, switching modes, key map, settings, console commands
- [Architecture](docs/architecture.md): how the bridge works
- [Protocol](docs/protocol.md): the MR25GB's BLE protocol (GATT, reports, IMU, command channel) and how the TV pairs
- [Troubleshooting](docs/troubleshooting.md)

## Layout

| Path | |
|---|---|
| `firmware/` | ESP-IDF project (ESP32-S3) |
| `tools/flash.py` | build and flash without pressing buttons |
| `tools/console.py` | the bridge's serial console (log + commands) |
| `prototype/` | earlier Linux/BlueZ implementation, kept for reference |

## Planned

- Configuration page (WebSerial, runs in Chrome/Edge): key map, mode-switch key, pointer settings.

## License

[MIT](LICENSE)
