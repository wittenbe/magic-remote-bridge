# Notes for Claude

Start with [README.md](README.md) and [docs/](docs/); [docs/protocol.md](docs/protocol.md) and
[docs/troubleshooting.md](docs/troubleshooting.md) hold the hard-won details. This file covers how to work here.

## Working with the hardware

- The user tests physically (TV, remote, PC) and reports back. Give short, concrete test steps; read the bridge's
  log afterwards instead of guessing.
- The ESP32-S3 is plugged into the PC it controls. Its log and command console: `python tools/console.py`
  (`-c status -t 2` for one-shot). Useful commands: `status`, `settings`, `set <name> <value>`, `mode`, `forget`.
- Build + flash: `.\tools\flash.ps1` from any PowerShell (loads ESP-IDF from `~\esp\esp-idf`), or
  `python tools/flash.py` from an ESP-IDF shell. No buttons needed while the bridge firmware runs.
  Manual download mode if needed: hold B, tap R, release B.
- **Windows: a COM port can only be opened by one program.** Stop `console.py` (or any serial terminal) before
  `flash.py`, otherwise it can't send `dl` to the board. On Linux both can run.
- Re-pairing the TV costs the user a ritual (Home+Back until the LED blinks, then OK pointing at the TV). Bonds and
  the TV's subscriptions survive reboots and reflashing, so normal firmware changes don't need it.
- Never write to the remote's OTA service (`d0ff…`); it updates the remote's firmware.

## Repository rules

- The repo is **public**: no Bluetooth/MAC addresses of the user's devices, no btsnoop captures, no logs.
  Use placeholders like `AA:BB:CC:DD:EE:FF` in docs and examples.
- Docs are written as documentation (what it is, how to use it), not as an experiment log.
- Commits: use the repo-local git identity (GitHub noreply address); don't put the user's email in commits.

## Firmware conventions

- ESP-IDF v5.4.x, NimBLE, esp_tinyusb 1.x. Modules: `remote.c` (central), `tv.c` (peripheral), `pc.c` (modes,
  PC input), `usb.c`, `settings.c` (NVS), glue in `main.c`. Keep the comment style: explain *why*, not *what*.
- Keep NimBLE logging at WARNING; INFO over the USB console slows the BLE host enough to drop reports.
- Code that runs per report (100 Hz) must not block (`pc_on_fd`, notify paths).

## Open items

- Test the sleep settings on Windows (`tvsleep`, `pcsleep`; defaults in `settings.c`). Pointer: the user runs
  `accel 0`, double `gain`; smooth enough with the 13.75 ms link + `smooth 15`. Remaining 25-40 ms gaps (missed
  connection events next to the TV link) could be hidden with a ~40 ms jitter buffer, at the cost of latency.
- Why the controller rejects connection intervals below 13.75 ms is unknown (maybe multi-connection scheduling);
  accepting the remote's own update request in `BLE_GAP_EVENT_L2CAP_UPDATE_REQ` rebooted the bridge.
- Configuration page: WebSerial (Chrome/Edge) on top of the console commands: key map, mode-switch key + hold time,
  pointer settings. Key map and switch key are still compiled in (`KEYMAP`, `RED` in `pc.c`) and would move to NVS.
- Advertising flags: 0x06 works; the real remote uses 0x05, never confirmed with the bridge.
- FE report (probably microphone audio) is unexplored.
