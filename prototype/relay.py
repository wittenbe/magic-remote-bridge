"""Step 3: relay between the real MR25GB (we are central) and the fake remote's GATT server (the TV is central).

Used by `fake_remote.py --relay`. Real remote → TV: F9/FD/FE input notifications.
TV → real remote: writes to F9 Output/Feature, HID Control Point and Protocol Mode.
The real remote's characteristics are addressed by their (stable) declaration handles (docs/protocol.md).
Addresses come from the environment: MRB_REMOTE (the real remote), MRB_TV (the TV, only used by --fresh).

BlueZ forgets the TV's CCCD subscriptions when the GATT app is re-registered, and the bonded TV does not
subscribe again on reconnect. So after a restart the user re-pairs via Home+Back+OK: the IR code opens the TV's
pairing window, we grab the real remote, and the TV pairs with the fake. --fresh drops our bond with the remote
first (Home+Back makes the remote forget it anyway). Don't drop the TV bond: the TV then fails to reconnect and
skips this address when registering.

Writes need bluetoothd without the LE-HID plugin (`-P hog`); otherwise BlueZ refuses D-Bus writes to the
remote's HID service (NotAuthorized).
"""
import importlib
import os
import sys
import time

import dbus
from gi.repository import GLib

import pc_mode

from fake_remote import BUS, ADAPTER, CHR_IFACE, PROPS, log, hexs

REMOTE = os.environ.get("MRB_REMOTE", "")
TV = os.environ.get("MRB_TV", "")
if not REMOTE:
    sys.exit("set MRB_REMOTE to the real remote's address (AA:BB:CC:DD:EE:FF)")
REMOTE_PATH = f"{ADAPTER}/dev_{REMOTE.replace(':', '_')}"
DEV_IFACE = "org.bluez.Device1"
# key in fake_remote.build() / label → declaration handle on the real remote
INPUTS = {"F9in": 0x0018, "FDin": 0x0021, "FEin": 0x0026}
OUTPUTS = {"F9out": 0x002b, "F9feat": 0x001d}
OTHER_WRITES = {"HID/ControlPoint": 0x0011, "HID/ProtocolMode": 0x0013}
# A freshly paired remote (pairing mode) expects the LG handshake on F9 Output within ~15 s, otherwise it
# discards its key (F9 In "00", "40 00", 0x803F, disconnect). The TV may connect too late, so we send the
# TV's init sequence ourselves (recorded from the TV, see docs/protocol.md).
HANDSHAKE = [bytes.fromhex(h) for h in (
    "17", "13", "74", "19", "90",
    "62004d0f4f030000005d1bf8000c024d00010000000000000100020f"
    "4f000000000032ffff00000037000000a4831c8bbd852623291948cc"
    "6882442245193495304f7df5d75c40240bdc149249a6d45126a38a94"
    "9249a6d25127238a802000003268")]


def char_path(handle):
    return f"{REMOTE_PATH}/service000e/char{handle:04x}"


def remove_device(bus, addr):
    adapter = dbus.Interface(bus.get_object(BUS, ADAPTER), "org.bluez.Adapter1")
    try:
        adapter.RemoveDevice(f"{ADAPTER}/dev_{addr.replace(':', '_')}")
        log(f"RELAY removed old bond with {addr}")
    except dbus.DBusException:
        pass


class Relay:
    def __init__(self, bus, app, reports, fresh=False, on_remote_ready=lambda: None):
        self.bus = bus
        self.on_remote_ready = on_remote_ready
        self.reports = reports
        self.by_label = {c.label: c for s in app.services for c in s.chars}
        self.last_btn = None
        self.fd_count = 0
        self.router = pc_mode.Router()
        self.need_handshake = False
        self.handshaking = False
        self.connected_at = 0.0
        self.quick_drops = 0
        self.pairing = False
        self.adapter = dbus.Interface(bus.get_object(BUS, ADAPTER), "org.bluez.Adapter1")

        if fresh:
            remove_device(bus, REMOTE)

        # One receiver for all real-remote characteristics, keyed by path, so re-subscribing after a
        # reconnect or re-pair doesn't stack handlers.
        self.inputs_by_path = {char_path(h): key for key, h in INPUTS.items()}
        bus.add_signal_receiver(self.on_real_props, dbus_interface=PROPS, signal_name="PropertiesChanged",
                                path_keyword="path")
        bus.add_signal_receiver(self.on_iface_added, dbus_interface="org.freedesktop.DBus.ObjectManager",
                                signal_name="InterfacesAdded")
        # `bluetoothctl remove <remote>` while running → scan and grab it again, without re-registering the app.
        bus.add_signal_receiver(self.on_iface_removed, dbus_interface="org.freedesktop.DBus.ObjectManager",
                                signal_name="InterfacesRemoved")

        for key, h in OUTPUTS.items():
            self._wire_write(reports[key], h)
        reports["FDin"].on_notify_change = self.on_tv_fd_notify
        self.last_fd = None
        for label, h in OTHER_WRITES.items():
            self._wire_write(self.by_label[label], h)

        self.connecting = False
        if not self._remote_prop("Paired"):
            self.scan()
            return
        on_remote_ready()
        if self._remote_prop("ServicesResolved"):
            self.subscribe()
        else:
            self.reconnect()

    def scan(self):
        self.pairing = False
        self.adapter.SetDiscoveryFilter({"Transport": "le", "DuplicateData": True})
        try:
            self.adapter.StartDiscovery()
        except dbus.DBusException:
            pass
        log(f"RELAY scanning for the real remote {REMOTE}: Home+Back, then OK pointing at the TV")

    def on_iface_removed(self, path, ifaces):
        if path == REMOTE_PATH and DEV_IFACE in ifaces:
            log("RELAY real remote device removed (unbonded or BlueZ dropped the temporary entry)")
            self.scan()

    def reconnect(self):
        """Keep an LE connect to the bonded remote pending while it's disconnected.

        After the TV puts it to sleep, the remote reconnects to its central on the next key press. Normally
        BlueZ's HOG plugin marks it auto-connect (background scan); with `-P hog` nobody would listen, and the
        remote falls back to IR. A pending Connect() is that background scan."""
        if self.connecting or not self._remote_prop("Paired") or self._remote_prop("Connected"):
            return False
        self.connecting = True
        dev = dbus.Interface(self.bus.get_object(BUS, REMOTE_PATH), DEV_IFACE)

        def done(err=None):
            self.connecting = False
            if err is not None:
                GLib.timeout_add(500, self.reconnect)  # connect timed out (remote asleep): try again
        dev.Connect(reply_handler=done, error_handler=done, timeout=120)
        return False

    def _remote_prop(self, name):
        try:
            return bool(dbus.Interface(self.bus.get_object(BUS, REMOTE_PATH), PROPS).Get(DEV_IFACE, name))
        except dbus.DBusException:
            return False

    def handshake(self):
        """Send the TV's init sequence to the real remote so it keeps the new bond (see HANDSHAKE)."""
        self.need_handshake = False
        self.handshaking = True
        real = dbus.Interface(self.bus.get_object(BUS, char_path(OUTPUTS["F9out"])), CHR_IFACE)
        log("RELAY handshake with the real remote")

        def send(i=0):
            if i == len(HANDSHAKE):
                GLib.timeout_add(500, lambda: setattr(self, "handshaking", False) or False)
                return False
            real.WriteValue(dbus.Array(HANDSHAKE[i], signature="y"), {"type": "command"},
                            reply_handler=lambda: None,
                            error_handler=lambda e: log(f"RELAY handshake write failed: {e}"))
            GLib.timeout_add(30, send, i + 1)
            return False
        send()

    def subscribe(self):
        if self.need_handshake:
            GLib.timeout_add(200, lambda: self.need_handshake and self.handshake() or False)
        for path, key in self.inputs_by_path.items():
            dbus.Interface(self.bus.get_object(BUS, path), CHR_IFACE).StartNotify(
                reply_handler=lambda k=key: log(f"RELAY subscribed to real {k}"),
                error_handler=lambda e, k=key: log(f"RELAY StartNotify {k}: {e.get_dbus_message()}"))

    def grab(self):
        if self.pairing:
            return
        self.pairing = True
        log("RELAY real remote seen, pairing...")
        try:
            self.adapter.StopDiscovery()
        except dbus.DBusException:
            pass
        dev = dbus.Interface(self.bus.get_object(BUS, REMOTE_PATH), DEV_IFACE)

        def paired():
            log("RELAY real remote paired")
            self.need_handshake = True
            if self._remote_prop("ServicesResolved"):  # BlueZ often resolves before the Pair() reply arrives
                GLib.timeout_add(200, lambda: self.need_handshake and self.handshake() or False)
            self.on_remote_ready()
            dbus.Interface(self.bus.get_object(BUS, REMOTE_PATH), PROPS).Set(DEV_IFACE, "Trusted", True)
            dev.Connect(reply_handler=lambda: None, error_handler=lambda e: log(f"RELAY connect: {e}"), timeout=30)

        def failed(e):
            log(f"RELAY pairing the real remote failed: {e}")
            self.scan()
        dev.Pair(reply_handler=paired, error_handler=failed, timeout=60)

    def on_iface_added(self, path, ifaces):
        if path == REMOTE_PATH and DEV_IFACE in ifaces and not ifaces[DEV_IFACE].get("Paired", False):
            self.grab()

    def on_real_props(self, iface, changed, invalidated, path=None):
        if path == REMOTE_PATH and iface == DEV_IFACE:
            if changed.get("ServicesResolved"):
                self.subscribe()
            elif changed.get("Connected"):
                self.connected_at = time.monotonic()
            elif changed.get("Connected") is not None and self._remote_prop("Paired"):
                # Drops right after connecting = encryption with our key fails = the remote discarded the bond.
                self.quick_drops = self.quick_drops + 1 if time.monotonic() - self.connected_at < 2 else 0
                if self.quick_drops >= 3:
                    log("RELAY the real remote seems to have discarded our key; dropping the bond and re-scanning")
                    self.quick_drops = 0
                    remove_device(self.bus, REMOTE)
                    return
                log("RELAY real remote disconnected (asleep); waiting for it to reconnect")
                self.reconnect()
            elif "RSSI" in changed and not self._remote_prop("Paired"):
                self.grab()
            return
        key = self.inputs_by_path.get(path)
        if key is None or iface != CHR_IFACE or "Value" not in changed:
            return
        value = bytes(changed["Value"])
        if key == "FDin" and len(value) >= 19:
            out = self.router.on_fd(value)
            if out is not None:
                self.reports[key].notify(out, force=True)
        elif self.handshaking and key == "F9in":
            log(f"RELAY handshake answer F9in = {hexs(value[:12])}{' …' if len(value) > 12 else ''}")
            return
        elif self.router.mode == "TV":
            self.reports[key].notify(value, force=True)
        if key == "FDin" and len(value) >= 19:
            self.last_fd = value
            self.fd_count += 1
            btn = (value[16] << 8) | value[17]
            if btn != self.last_btn or value[18]:
                log(f"RELAY remote→TV FD btn=0x{btn:04x} wheel={value[18] if value[18] < 128 else value[18] - 256}"
                    f" (TV subscribed={self.reports[key].notifying}, {self.fd_count} FD so far)")
                self.last_btn = btn
        elif key != "FDin":
            log(f"RELAY remote→TV {key} = {hexs(value)}")

    def reload(self):
        """SIGUSR1: reload pc_mode.py (mapping, pointer tuning) without re-registering the GATT app."""
        mode = self.router.mode
        self.router.close()
        try:
            importlib.reload(pc_mode)
            self.router = pc_mode.Router(mode)
            log(f"RELAY reloaded pc_mode (mode {mode})")
        except Exception as e:  # keep relaying even if the edited module is broken
            log(f"RELAY reload failed: {e!r}")
            self.router = pc_mode.Router(mode)
        return True

    def on_tv_fd_notify(self, _chrc, on):
        # The real remote can only send its connect event 0x803E once its central subscribed; our remote sent it
        # to us long before the TV subscribed, so replay it now (on top of the last real report).
        if on and self.last_fd:
            self.reports["FDin"].notify(self.last_fd[:16] + b"\x80\x3e\x00", force=True)
            log("RELAY sent connect event 0x803E to the TV")

    def _wire_write(self, fake_chrc, handle):
        path = char_path(handle)

        def forward(_chrc, value, write_type):
            if not self.router.allow_tv_write(value):
                log(f"RELAY TV→remote {fake_chrc.label} = {hexs(value)} blocked ({self.router.mode} mode)")
                return
            log(f"RELAY TV→remote {fake_chrc.label} = {hexs(value)}")
            try:
                real = dbus.Interface(self.bus.get_object(BUS, path), CHR_IFACE)
                real.WriteValue(dbus.Array(value, signature="y"), {"type": write_type or "command"},  # the TV uses Write Command
                                reply_handler=lambda: None,
                                error_handler=lambda e: log(f"RELAY write {fake_chrc.label} failed: {e}"))
            except dbus.DBusException as e:
                log(f"RELAY write {fake_chrc.label}: real remote not available ({e.get_dbus_name()})")
        fake_chrc.on_write = forward
