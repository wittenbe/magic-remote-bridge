#!/usr/bin/env python3
"""Console of the bridge firmware (log output + commands), Linux/Windows/macOS. Needs pyserial.

    python tools/console.py                      # interactive: shows the log, type commands (status, help, ...)
    python tools/console.py -c status -t 2       # send commands, print output for 2 s, exit
    python tools/console.py --log bridge.log     # also append everything to a file

The port is found by USB ID; it reconnects automatically when the board reboots (e.g. after flashing).
"""
import argparse
import sys
import threading
import time

import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A
PID_BRIDGE = 0x4005


def find_port():
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID and p.pid == PID_BRIDGE:
            return p.device
    return None


class Console:
    def __init__(self, log_path=None):
        self.ser = None
        self.log = open(log_path, "a", encoding="utf-8") if log_path else None
        self.lock = threading.Lock()

    def connect(self, wait=True):
        announced = False
        while True:
            port = find_port()
            if port:
                try:
                    self.ser = serial.Serial(port, 115200, timeout=0.1)
                    self._out(f"--- connected to {port} ---\n")
                    return True
                except (serial.SerialException, OSError):
                    pass  # e.g. permissions not set yet right after enumeration
            if not wait:
                return False
            if not announced:
                self._out("--- waiting for the bridge (USB 303a:4005) ---\n")
                announced = True
            time.sleep(0.5)

    def _out(self, text):
        sys.stdout.write(text)
        sys.stdout.flush()
        if self.log:
            self.log.write(text)
            self.log.flush()

    def send(self, line):
        with self.lock:
            if self.ser:
                try:
                    self.ser.write(line.encode() + b"\r\n")
                except (serial.SerialException, OSError):
                    pass

    def read_forever(self, stop):
        while not stop.is_set():
            try:
                data = self.ser.read(4096)
            except (serial.SerialException, OSError):
                self._out("\n--- disconnected ---\n")
                with self.lock:
                    self.ser = None
                self.connect()
                continue
            if data:
                self._out(data.decode(errors="replace"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-c", "--command", action="append", default=[], help="command to send (repeatable)")
    ap.add_argument("-t", "--time", type=float, default=None, help="exit after this many seconds")
    ap.add_argument("--log", help="append output to this file")
    args = ap.parse_args()

    con = Console(args.log)
    if not con.connect(wait=args.time is None):
        sys.exit("bridge not found (USB 303a:4005)")
    stop = threading.Event()
    reader = threading.Thread(target=con.read_forever, args=(stop,), daemon=True)
    reader.start()

    con.send("")  # flush any partial line in the firmware's input buffer
    for c in args.command:
        con.send(c)
        time.sleep(0.2)

    if args.time is not None:
        time.sleep(args.time)
        stop.set()
        return
    try:
        for line in sys.stdin:
            con.send(line.rstrip("\r\n"))
    except KeyboardInterrupt:
        pass
    stop.set()


if __name__ == "__main__":
    main()
