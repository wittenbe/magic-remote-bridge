"""Mode switching and PC mode for the relay (hot-reloadable: `kill -USR1 <relay pid>`).

Router.on_fd() gets every FD report from the real remote and returns the report to forward to the TV
(possibly modified), or None. Router.allow_tv_write() decides whether a TV write reaches the remote.

TV mode: forward everything, except the red key, which is held back until release: a short press is then
replayed to the TV, a long press (>= LONG_PRESS_S) toggles the mode instead.
PC mode: nothing goes to the TV and TV writes don't reach the remote (so the TV can't put it to sleep);
keys, wheel and pointer go to a uinput keyboard+mouse.

FD report layout (19 bytes): [0] ?, [1] seq, [2:4] status, [4:10] gyro x/y/z, [10:16] accel x/y/z (int16 BE),
[16:18] button code (u16 BE, 0 = none), [18] wheel (int8).
IMU frame (measured): X = right, Y = forward (tip), Z = up; the accelerometer reports the gravity direction
(flat, face up: z ≈ -4096 = -1 g). The gyro is right-handed in the same frame (+gx = tip up, +gy = roll about
the tip axis, checked against the accelerometer); scale ≈ 16.9 LSB per °/s.
"""
import fcntl
import math
import os
import struct
import time

from fake_remote import log

LONG_PRESS_S = 0.8
RED = 0x8072
CONNECT, DISCONNECT = 0x803E, 0x803F
RED_REPLAY_FRAMES = 5  # a short red press is replayed to the TV for this many reports (10 ms each)

# Pointer tuning
GYRO_GAIN = 0.012       # pixels per gyro LSB per report; measured 16.9 LSB per °/s → ~20 px per degree
STILL_LSB = 40          # all axes within this of the bias → "still" sample
STILL_SAMPLES = 50      # 0.5 s of stillness → re-estimate the bias
ACC_ALPHA = 0.05        # low-pass for the gravity direction
CLICK_FREEZE_S = 0.15   # no pointer motion right after OK press/release, for steady clicks

# linux/input-event-codes.h
EV_SYN, EV_KEY, EV_REL = 0, 1, 2
SYN_REPORT = 0
REL_X, REL_Y, REL_WHEEL = 0, 1, 8
BTN_LEFT, BTN_RIGHT = 0x110, 0x111
KEY = dict(ESC=1, ENTER=28, UP=103, PAGEUP=104, LEFT=105, RIGHT=106, DOWN=108, PAGEDOWN=109, MUTE=113,
           VOLUMEDOWN=114, VOLUMEUP=115, LEFTMETA=125, BACK=158, PLAYPAUSE=164, F1=59, F2=60, F3=61, F4=62)

KEYMAP = {
    0x8040: KEY["UP"], 0x8041: KEY["DOWN"], 0x8007: KEY["LEFT"], 0x8006: KEY["RIGHT"],
    0x8044: BTN_LEFT,                       # OK / wheel click
    0x8028: KEY["BACK"],                    # Back → browser/app back
    0x807C: KEY["LEFTMETA"],                # Home → Super (launcher)
    0x8043: BTN_RIGHT,                      # Settings → right click (code from the MR20 driver, untested)
    0x8002: KEY["VOLUMEUP"], 0x8003: KEY["VOLUMEDOWN"], 0x8009: KEY["MUTE"],
    0x8000: KEY["PAGEUP"], 0x8001: KEY["PAGEDOWN"],   # Ch+ / Ch-
    0x80B0: KEY["PLAYPAUSE"], 0x80BA: KEY["PLAYPAUSE"],  # Play / Pause (MR20 codes, untested)
    0x8071: KEY["F2"], 0x8063: KEY["F3"], 0x8061: KEY["F4"],  # Green / Yellow / Blue (MR20 codes)
}
KEYMAP.update({0x8010 + n: (11 if n == 0 else 1 + n) for n in range(10)})  # digits → KEY_0..KEY_9

# linux/uinput.h
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_RELBIT = 0x40045564, 0x40045565, 0x40045566
UI_DEV_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405C5503, 0x5501, 0x5502
BUS_BLUETOOTH = 0x05


class UInput:
    """Minimal uinput keyboard+mouse (no python-evdev dependency)."""

    def __init__(self, name="LG Magic Remote (PC mode)"):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        for ev in (EV_KEY, EV_REL):
            fcntl.ioctl(self.fd, UI_SET_EVBIT, ev)
        for code in set(KEYMAP.values()) | {BTN_LEFT, BTN_RIGHT}:
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        for code in (REL_X, REL_Y, REL_WHEEL):
            fcntl.ioctl(self.fd, UI_SET_RELBIT, code)
        setup = struct.pack("<HHHH80sI", BUS_BLUETOOTH, 0x005D, 0x8762, 1, name.encode(), 0)
        fcntl.ioctl(self.fd, UI_DEV_SETUP, setup)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)

    def emit(self, events):
        if not events:
            return
        data = b"".join(struct.pack("llHHi", 0, 0, t, c, v) for t, c, v in events)
        os.write(self.fd, data + struct.pack("llHHi", 0, 0, EV_SYN, SYN_REPORT, 0))

    def close(self):
        try:
            fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        finally:
            os.close(self.fd)


class Pointer:
    """Gyro → cursor deltas, roll-compensated with the gravity direction from the accelerometer."""

    def __init__(self):
        self.bias = [0.0, 0.0, 0.0]
        self.still = []
        self.grav = None
        self.acc_x = self.acc_y = 0.0
        self.freeze_until = 0.0

    def update(self, gyro, accel, now):
        if self.grav is None:
            self.grav = list(accel)
        self.grav = [g + ACC_ALPHA * (a - g) for g, a in zip(self.grav, accel)]

        if all(abs(g - b) < STILL_LSB for g, b in zip(gyro, self.bias)):
            self.still.append(gyro)
            if len(self.still) >= STILL_SAMPLES:
                self.bias = [sum(s[i] for s in self.still) / len(self.still) for i in range(3)]
                self.still = []
        else:
            self.still = []

        w = [g - b for g, b in zip(gyro, self.bias)]
        n = math.sqrt(sum(g * g for g in self.grav)) or 1.0
        up = [g / n for g in self.grav]                   # accel reports specific force (up), IMU mounted upside down
        fwd = (0.0, 1.0, 0.0)
        right = (fwd[1] * up[2] - fwd[2] * up[1], fwd[2] * up[0] - fwd[0] * up[2], fwd[0] * up[1] - fwd[1] * up[0])
        rn = math.sqrt(sum(r * r for r in right)) or 1.0
        right = [r / rn for r in right]
        yaw = sum(a * b for a, b in zip(w, up))           # + = turning left (counter-clockwise from above)
        pitch = sum(a * b for a, b in zip(w, right))      # + = tip going up
        if now < self.freeze_until:
            return 0, 0
        self.acc_x += -yaw * GYRO_GAIN
        self.acc_y += -pitch * GYRO_GAIN
        dx, dy = int(self.acc_x), int(self.acc_y)
        self.acc_x -= dx
        self.acc_y -= dy
        return dx, dy


class Router:
    def __init__(self, mode="TV"):
        self.mode = mode
        self.red_down_at = None
        self.red_consumed = False
        self.red_replay = 0
        self.last_btn = 0
        self.pointer = Pointer()
        self.ui = None
        self.dbg = {"dx": 0, "dy": 0, "wheel": 0, "since": time.monotonic()}
        if mode == "PC":
            self._open_ui()

    def _open_ui(self):
        try:
            self.ui = UInput()
        except OSError as e:
            log(f"MODE PC: can't create uinput device: {e}")

    def close(self):
        if self.ui:
            self.ui.close()
            self.ui = None

    def toggle(self):
        self.mode = "PC" if self.mode == "TV" else "TV"
        log(f"MODE → {self.mode}")
        if self.mode == "PC" and self.ui is None:
            self._open_ui()
        self._release_pc_key()

    def _release_pc_key(self):
        if self.ui and self.last_btn in KEYMAP:
            self.ui.emit([(EV_KEY, KEYMAP[self.last_btn], 0)])

    def allow_tv_write(self, value):
        return self.mode == "TV"

    def on_fd(self, value):
        now = time.monotonic()
        btn = (value[16] << 8) | value[17]
        wheel = value[18] - 256 if value[18] >= 128 else value[18]

        # Red: held back in both modes; long press toggles, short press is replayed to the TV in TV mode.
        if btn == RED:
            if self.red_down_at is None:
                self.red_down_at, self.red_consumed = now, False
            elif not self.red_consumed and now - self.red_down_at >= LONG_PRESS_S:
                self.red_consumed = True
                self.toggle()
            value = value[:16] + b"\x00\x00" + value[18:]
            btn = 0
        elif self.red_down_at is not None:
            if not self.red_consumed and self.mode == "TV":
                self.red_replay = RED_REPLAY_FRAMES
            self.red_down_at = None

        if self.mode == "TV":
            self.last_btn = btn
            if self.red_replay and btn == 0:
                self.red_replay -= 1
                value = value[:16] + struct.pack(">H", RED) + value[18:]
            return value

        self._pc(value, btn, wheel, now)
        return None

    def _pc(self, value, btn, wheel, now):
        events = []
        if btn in (CONNECT, DISCONNECT):
            btn = self.last_btn
        if btn != self.last_btn:
            if self.last_btn in KEYMAP:
                events.append((EV_KEY, KEYMAP[self.last_btn], 0))
            if btn in KEYMAP:
                events.append((EV_KEY, KEYMAP[btn], 1))
            elif btn:
                log(f"PC: unmapped button 0x{btn:04x}")
            if BTN_LEFT in (KEYMAP.get(btn), KEYMAP.get(self.last_btn)):
                self.pointer.freeze_until = now + CLICK_FREEZE_S
            self.last_btn = btn
        if wheel:
            events.append((EV_REL, REL_WHEEL, wheel))
        gyro = struct.unpack(">3h", value[4:10])
        accel = struct.unpack(">3h", value[10:16])
        dx, dy = self.pointer.update(gyro, accel, now)
        if dx:
            events.append((EV_REL, REL_X, dx))
        if dy:
            events.append((EV_REL, REL_Y, dy))
        if self.ui:
            self.ui.emit(events)
        self._debug(events, now)

    def _debug(self, events, now):
        """Log PC output (there's no screen on this box to watch it): keys immediately, motion summed per 0.5 s."""
        names = {v: k for k, v in KEY.items()} | {BTN_LEFT: "BTN_LEFT", BTN_RIGHT: "BTN_RIGHT"}
        for t, c, v in events:
            if t == EV_KEY:
                log(f"PC key {names.get(c, c)} {'down' if v else 'up'}")
            elif t == EV_REL:
                self.dbg[{REL_X: "dx", REL_Y: "dy", REL_WHEEL: "wheel"}[c]] += v
        if now - self.dbg["since"] >= 0.5:
            if self.dbg["dx"] or self.dbg["dy"] or self.dbg["wheel"]:
                log(f"PC motion dx={self.dbg['dx']:+5d} dy={self.dbg['dy']:+5d} wheel={self.dbg['wheel']:+d}")
            self.dbg.update(dx=0, dy=0, wheel=0, since=now)
