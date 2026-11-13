# Setup: hardware, toolchain, flashing

## Hardware

- An **ESP32-S3** board whose USB connector is wired to the chip's native USB (not a USB-UART bridge).
  Developed on an ESP32-S3 (QFN56, 8 MB flash, 8 MB PSRAM) board with one USB port and two small buttons,
  B (BOOT) and R (RESET).
- **Antenna:** if the board has a u.FL connector, attach the antenna. Without it the signal from the remote
  was around −99 dBm and connections kept failing; with it −57…−70 dBm.
- The board has to be in Bluetooth range of both the TV and the remote while plugged into the PC.

## ESP-IDF

The firmware needs **ESP-IDF v5.4.x**.

**Windows:** install with the [ESP-IDF Tools Installer](https://dl.espressif.com/dl/esp-idf/) (choose v5.4.x),
then use the "ESP-IDF PowerShell" shortcut for all commands below.

**Linux:**
```sh
mkdir -p ~/esp && cd ~/esp
git clone --depth 1 --branch v5.4.1 --recurse-submodules --shallow-submodules https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
python3 tools/idf_tools.py install cmake ninja   # if your system has no cmake/ninja
. ~/esp/esp-idf/export.sh                         # in every new shell
```
Serial port access needs the `dialout` group: `sudo usermod -aG dialout $USER`, then log in again.

## Build and flash

In an ESP-IDF shell, from the repository root:

```sh
idf.py -C firmware set-target esp32s3   # once
python tools/flash.py                   # build + flash
```

`flash.py` finds the board by its USB ID. If the bridge firmware is running, it reboots it into the ROM
download mode over the console, flashes, and resets it. No buttons needed.

**The first time** (or if the board runs other firmware), put it into download mode by hand:
hold **B**, tap **R**, release **B**, then run `python tools/flash.py`.

USB IDs: `303a:4005` = bridge firmware, `303a:1001` = ROM download mode (or firmware without TinyUSB).

## Console

```sh
python tools/console.py                  # live log, type commands
python tools/console.py -c status -t 2   # one-shot
```

On Windows the console is a normal COM port, so PuTTY or any serial terminal works too (115200 baud, the speed
doesn't matter). See [usage](usage.md#console-commands) for the commands.
