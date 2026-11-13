#!/usr/bin/env python3
"""Build and flash the bridge firmware without pressing any buttons (Linux, Windows, macOS).

Run it from an ESP-IDF shell (Linux: `. $IDF_PATH/export.sh`, Windows: "ESP-IDF PowerShell"), so that this
Python has pyserial and IDF_PATH is set:

    python tools/flash.py            # build + flash
    python tools/flash.py --no-build # flash the existing build

If the bridge firmware is running, it is told to reboot into the ROM download mode ("dl" on its console port).
Otherwise the board has to be put into download mode by hand: hold B (BOOT), tap R (RESET), release B.
"""
import argparse
import os
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A
PID_BRIDGE = 0x4005  # bridge firmware (TinyUSB: console + HID)
PID_ROM = 0x1001     # USB-Serial/JTAG: ROM download mode, or a firmware without TinyUSB

FIRMWARE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "firmware")


def find_port(pid):
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID and p.pid == pid:
            return p.device
    return None


def wait_for_port(pid, timeout):
    end = time.time() + timeout
    while time.time() < end:
        port = find_port(pid)
        if port:
            time.sleep(0.5)  # let the OS finish setting up the port (permissions on Linux)
            return port
        time.sleep(0.2)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--no-build", action="store_true", help="flash the existing build")
    args = ap.parse_args()

    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        sys.exit("IDF_PATH is not set: run this from an ESP-IDF shell")

    if not args.no_build:
        r = subprocess.run([sys.executable, os.path.join(idf_path, "tools", "idf.py"), "build"], cwd=FIRMWARE,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode != 0:
            print("\n".join(l for l in r.stdout.splitlines() if "error" in l.lower())[-3000:])
            sys.exit("build failed, not flashing")
        print("build ok")

    bridge = find_port(PID_BRIDGE)
    if bridge:
        print(f"bridge firmware on {bridge}: rebooting it into download mode")
        try:
            with serial.Serial(bridge, 115200, timeout=0.2) as s:
                s.write(b"\r\ndl\r\n")
                time.sleep(0.3)
        except (serial.SerialException, OSError):
            pass  # the port disappears while we're still writing: expected

    rom = wait_for_port(PID_ROM, 10)
    if not rom:
        sys.exit("ESP not in download mode: hold B (BOOT), tap R (RESET), release B, then run again")

    esptool = os.path.join(idf_path, "components", "esptool_py", "esptool", "esptool.py")
    cmd = [sys.executable, esptool, "--chip", "esp32s3", "-p", rom, "-b", "460800", "--before", "default_reset",
           "--after", "watchdog_reset", "write_flash", "@flash_args"]
    r = subprocess.run(cmd, cwd=os.path.join(FIRMWARE, "build"))
    if r.returncode != 0:
        sys.exit("flashing failed")

    if wait_for_port(PID_BRIDGE, 15):
        print("bridge firmware running")
    else:
        sys.exit("firmware did not come up (unplug and replug the board)")


if __name__ == "__main__":
    main()
