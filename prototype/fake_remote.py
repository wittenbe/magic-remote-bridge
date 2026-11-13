#!/usr/bin/env python3
"""Step 2: pretend to be the LG Magic Remote MR25GB toward the TV and log everything the TV does.

Registers a GATT server that mirrors the real remote's layout/values (see docs/protocol.md) plus an
LE advertisement with the same advertising data and scan response. Every read, write and notify
subscription by the TV is logged. Run btmon alongside to capture SMP and ATT on the wire.

Usage: python3 fake_remote.py [--no-imu] [--relay [--fresh]]
  --no-imu   don't send the 100 Hz idle FD reports after the TV enables notifications
  --relay    relay to/from the real remote; implies --no-imu. Grabs the remote when it advertises.
  --fresh    with --relay: drop our bond with the remote first, then re-pair it (Home+Back+OK)
"""
import sys
import time
import dbus
import dbus.exceptions
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

BUS = "org.bluez"
ADAPTER = "/org/bluez/hci0"
APP = "/rc/app"
ADV = "/rc/adv"
AGENT = "/rc/agent"
GATT_MGR = "org.bluez.GattManager1"
ADV_MGR = "org.bluez.LEAdvertisingManager1"
SVC_IFACE = "org.bluez.GattService1"
CHR_IFACE = "org.bluez.GattCharacteristic1"
DSC_IFACE = "org.bluez.GattDescriptor1"
PROPS = "org.freedesktop.DBus.Properties"

NAME = "LGE MR25GB"
LG_COMPANY = 0x00C4
LG_MFR = b"SCD 21.2,BA 35,webOS"
VND = "-3c17-d293-8e48-14fe2e4da212"
REPORT_MAP = bytes.fromhex(
    "050c0901a10185fd951e75081500 26ff008100c0"
    "050c0901a10185f9951e7508150026ff0081009 5c87508150026ff00b100c0"
    "050c0901a10185fe957c7508150026ff008100c0".replace(" ", ""))

T0 = time.monotonic()


def log(msg):
    print(f"[{time.monotonic() - T0:8.3f}] {msg}", flush=True)


def hexs(b):
    return " ".join(f"{x:02x}" for x in bytes(b))


def u(short):
    return f"0000{short}-0000-1000-8000-00805f9b34fb" if len(short) == 4 else short


class NotSupported(dbus.exceptions.DBusException):
    _dbus_error_name = "org.bluez.Error.NotSupported"


class Failed(dbus.exceptions.DBusException):
    _dbus_error_name = "org.bluez.Error.Failed"


class Application(dbus.service.Object):
    def __init__(self, bus):
        self.path = APP
        self.services = []
        super().__init__(bus, self.path)

    @dbus.service.method("org.freedesktop.DBus.ObjectManager", out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        out = {}
        for s in self.services:
            out[s.path] = s.props()
            for c in s.chars:
                out[c.path] = c.props()
                for d in c.descs:
                    out[d.path] = d.props()
        return out


class Service(dbus.service.Object):
    def __init__(self, bus, index, uuid, label):
        self.path = f"{APP}/s{index:02d}"
        self.uuid = u(uuid)
        self.label = label
        self.chars = []
        self.bus = bus
        super().__init__(bus, self.path)

    def props(self):
        return {SVC_IFACE: {"UUID": self.uuid, "Primary": True}}

    def add(self, uuid, flags, value=None, label=None, read_error=False):
        c = Char(self.bus, len(self.chars), self, uuid, flags, value, label, read_error)
        self.chars.append(c)
        return c

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return self.props()[SVC_IFACE]


class Char(dbus.service.Object):
    def __init__(self, bus, index, svc, uuid, flags, value, label, read_error):
        self.path = f"{svc.path}/c{index:02d}"
        self.svc = svc
        self.uuid = u(uuid)
        self.flags = flags
        self.value = bytes(value or b"")
        self.label = label or f"{svc.label}/{uuid}"
        self.read_error = read_error
        self.descs = []
        self.notifying = False
        self.bus = bus
        super().__init__(bus, self.path)

    def props(self):
        return {CHR_IFACE: {"Service": dbus.ObjectPath(self.svc.path), "UUID": self.uuid,
                            "Flags": dbus.Array(self.flags, signature="s")}}

    def add_desc(self, uuid, value, label=None):
        d = Desc(self.bus, len(self.descs), self, uuid, value, label)
        self.descs.append(d)
        return d

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return self.props()[CHR_IFACE]

    @dbus.service.method(CHR_IFACE, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        off = int(options.get("offset", 0))
        log(f"READ  {self.label} off={off} from {options.get('device', '?')}")
        if self.read_error:
            # The real remote answers ATT 0x0A here; BlueZ can't send that code, so the nearest is an error.
            raise NotSupported("read not supported")
        return dbus.Array(self.value[off:], signature="y")

    @dbus.service.method(CHR_IFACE, in_signature="aya{sv}")
    def WriteValue(self, value, options):
        log(f"WRITE {self.label} type={options.get('type', '?')} = {hexs(value)}")
        if "read" in self.flags and not self.read_error:
            self.value = bytes(value)
        if self.on_write:
            # BlueZ 5.82 doesn't pass "type" for writes from the peer (logged as "?"), hence "" when unknown.
            self.on_write(self, bytes(value), str(options.get("type", "")))

    on_write = None

    @dbus.service.method(CHR_IFACE)
    def StartNotify(self):
        log(f"NOTIFY ON  {self.label}")
        self.notifying = True
        if self.on_notify_change:
            self.on_notify_change(self, True)

    @dbus.service.method(CHR_IFACE)
    def StopNotify(self):
        log(f"NOTIFY OFF {self.label}")
        self.notifying = False
        if self.on_notify_change:
            self.on_notify_change(self, False)

    on_notify_change = None

    def notify(self, value, force=False):
        # force: send anyway; BlueZ only forwards to devices whose CCCD (restored for bonded peers) is on.
        if not (self.notifying or force):
            return
        self.value = bytes(value)
        self.PropertiesChanged(CHR_IFACE, {"Value": dbus.Array(self.value, signature="y")}, [])

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, iface, changed, invalidated):
        pass


class Desc(dbus.service.Object):
    def __init__(self, bus, index, chrc, uuid, value, label):
        self.path = f"{chrc.path}/d{index:02d}"
        self.chrc = chrc
        self.uuid = u(uuid)
        self.value = bytes(value)
        self.label = label or f"{chrc.label}/{uuid}"
        super().__init__(bus, self.path)

    def props(self):
        return {DSC_IFACE: {"Characteristic": dbus.ObjectPath(self.chrc.path), "UUID": self.uuid,
                            "Flags": dbus.Array(["read"], signature="s")}}

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return self.props()[DSC_IFACE]

    @dbus.service.method(DSC_IFACE, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        log(f"READ  {self.label}")
        return dbus.Array(self.value, signature="y")

    @dbus.service.method(DSC_IFACE, in_signature="aya{sv}")
    def WriteValue(self, value, options):
        log(f"WRITE {self.label} = {hexs(value)}")


class Advertisement(dbus.service.Object):
    def __init__(self, bus):
        self.path = ADV
        super().__init__(bus, self.path)

    def props(self):
        return {
            "Type": "peripheral",
            "LocalName": NAME,  # BlueZ puts the name into the scan response, like the real remote
            "ManufacturerData": dbus.Dictionary({dbus.UInt16(LG_COMPANY): dbus.Array(LG_MFR, signature="y")},
                                                signature="qv"),
            # The real remote uses flags 0x05 (Limited). BlueZ rejects raw flags in "Data", so this gives General.
            "Discoverable": True,
            # The real remote advertises every ~35 ms; BlueZ's default is 1.28 s (values in ms).
            "MinInterval": dbus.UInt32(20),
            "MaxInterval": dbus.UInt32(30),
        }

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return self.props()

    @dbus.service.method("org.bluez.LEAdvertisement1")
    def Release(self):
        log("Advertisement released")


class Agent(dbus.service.Object):
    @dbus.service.method("org.bluez.Agent1")
    def Release(self):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="o")
    def RequestAuthorization(self, device):
        log(f"AGENT RequestAuthorization {device}")

    @dbus.service.method("org.bluez.Agent1", in_signature="ou")
    def RequestConfirmation(self, device, passkey):
        log(f"AGENT RequestConfirmation {device} {passkey:06d}")

    @dbus.service.method("org.bluez.Agent1", in_signature="os")
    def AuthorizeService(self, device, uuid):
        log(f"AGENT AuthorizeService {device} {uuid}")

    @dbus.service.method("org.bluez.Agent1")
    def Cancel(self):
        log("AGENT Cancel")


def build(bus, app):
    s = []

    hid = Service(bus, 0, "1812", "HID")
    hid.add("2a4a", ["read"], bytes.fromhex("00000001"), "HID/Info")
    hid.add("2a4c", ["write-without-response"], None, "HID/ControlPoint")
    hid.add("2a4e", ["read", "write-without-response"], b"\x01", "HID/ProtocolMode")
    rm = hid.add("2a4b", ["read"], REPORT_MAP, "HID/ReportMap")
    rm.add_desc("2907", bytes.fromhex("192a"))
    reports = {}
    for key, flags, rr, desc, err in (
            ("F9in", ["read", "notify"], "f901", "HID_0xF9_IN_USER_SESCR", True),
            ("F9feat", ["read", "write-without-response", "write"], "f903", "HID_0xF9_FEATURE_USER_SESCR", True),
            ("FDin", ["read", "write", "notify"], "fd01", "HID_0xFD_IN_USER_SESCR", False),
            ("FEin", ["read", "notify"], "fe01", "HID_0xFE_IN_USER_SESCR", True),
            ("F9out", ["read", "write-without-response", "write"], "f902", "HID_0xF9_OUT_USER_SESCR", True)):
        c = hid.add("2a4d", flags, b"", f"HID/Report{key}", read_error=err)
        c.add_desc("2908", bytes.fromhex(rr))
        c.add_desc("2901", desc.encode())
        reports[key] = c
    s.append(hid)

    bat = Service(bus, 1, "180f", "Battery")
    bat.add("2a19", ["read", "notify"], b"\x27", "Battery/Level")
    s.append(bat)

    dis = Service(bus, 2, "180a", "DIS")
    for uuid, val in (("2a29", b"Realtek BT"), ("2a24", b"Model Nbr 0.9"), ("2a25", b"RTKBeeSerialNum"),
                      ("2a27", b"RTKBeeHardwareRev"), ("2a26", b"RTKBeeFirmwareRev"),
                      ("2a28", b"RTKBeeSoftwareRev"), ("2a23", bytes.fromhex("0001020000030405")),
                      ("2a2a", b"RTKBeeIEEEDatalist"), ("2a50", bytes.fromhex("015d0062870100"))):
        dis.add(uuid, ["read"], val, f"DIS/{uuid}")
    s.append(dis)

    scp = Service(bus, 3, "1813", "ScanParams")
    scp.add("2a4f", ["write-without-response"], None, "ScanParams/IntervalWindow")
    scp.add("2a31", ["notify"], None, "ScanParams/Refresh")
    s.append(scp)

    lg = Service(bus, 4, "0000d1ff" + VND, "LG-d1ff")
    lg.add("a001", ["write-without-response", "notify"], None, "LG/a001")
    lg.add("a002", ["write-without-response", "write"], None, "LG/a002")
    s.append(lg)

    ota = Service(bus, 5, "0000d0ff" + VND, "OTA-d0ff")
    for uuid, flags, val in (("ffd1", ["write-without-response"], None),
                             ("ffd2", ["read"], bytes(6)),  # the remote's BD address; never read by the TV
                             ("ffd3", ["read"], bytes.fromhex("01000c38")),
                             ("ffd4", ["read"], bytes.fromhex("61a00b00")),
                             ("ffd5", ["read"], b""),
                             ("ffd8", ["write-without-response"], None),
                             ("fff1", ["read"], bytes.fromhex("0c010016dc075d00f00f0003")),
                             ("fff2", ["write"], None),
                             ("ffe0", ["read"], bytes.fromhex("0300000111000000 01000c3861a00b0001e00308".replace(" ", ""))),
                             ("ffe1", ["read"], b""),
                             ("ffe2", ["read"], b"")):
        ota.add(uuid, flags, val, f"OTA/{uuid}")
    s.append(ota)

    app.services = s
    return reports


def main():
    relay = "--relay" in sys.argv
    send_imu = "--no-imu" not in sys.argv and not relay
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    loop = GLib.MainLoop()

    adapter_props = dbus.Interface(bus.get_object(BUS, ADAPTER), PROPS)
    old_alias = str(adapter_props.Get("org.bluez.Adapter1", "Alias"))
    adapter_props.Set("org.bluez.Adapter1", "Alias", NAME)  # GAP Device Name
    adapter_props.Set("org.bluez.Adapter1", "Pairable", True)

    Agent(bus, AGENT)
    agent_mgr = dbus.Interface(bus.get_object(BUS, "/org/bluez"), "org.bluez.AgentManager1")
    agent_mgr.RegisterAgent(AGENT, "NoInputNoOutput")
    agent_mgr.RequestDefaultAgent(AGENT)

    app = Application(bus)
    reports = build(bus, app)
    fd = reports["FDin"]
    state = {"seq": 0, "timer": None}

    def fd_tick():
        # Idle report: flat on a table (accel Z ≈ -1 g), no button, no wheel.
        state["seq"] = (state["seq"] + 1) & 0xFF
        payload = bytes([0xd0, state["seq"], 0x9d, 0x03]) + bytes(6) + bytes.fromhex("0000 0000 f009".replace(" ", "")) + bytes(3)
        fd.notify(payload)
        return fd.notifying

    def on_fd_notify(chrc, on):
        if on and not relay:
            # The real remote sends the connect event 0x803E once right after connecting.
            fd.notify(bytes([0xd0, 0, 0x9d, 0x03]) + bytes(12) + bytes([0x80, 0x3e, 0x00]))
            if send_imu and state["timer"] is None:
                state["timer"] = GLib.timeout_add(10, lambda: fd_tick() or state.update(timer=None))
    fd.on_notify_change = on_fd_notify

    gatt = dbus.Interface(bus.get_object(BUS, ADAPTER), GATT_MGR)
    advm = dbus.Interface(bus.get_object(BUS, ADAPTER), ADV_MGR)
    adv = Advertisement(bus)
    advertising = []

    def advertise():
        if advertising:
            return
        advertising.append(True)
        advm.RegisterAdvertisement(ADV, {}, reply_handler=lambda: log("Advertising as " + NAME),
                                   error_handler=fail("RegisterAdvertisement"))

    def fail(what):
        def h(e):
            log(f"{what} failed: {e}")
            loop.quit()
        return h

    gatt.RegisterApplication(APP, {}, reply_handler=lambda: log("GATT application registered"),
                             error_handler=fail("RegisterApplication"))
    if relay:
        # The relay starts advertising once it holds the real remote, so the TV can't register an empty fake.
        from relay import Relay
        r = Relay(bus, app, reports, fresh="--fresh" in sys.argv, on_remote_ready=advertise)
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 10, r.reload)  # SIGUSR1
    else:
        advertise()

    def on_props(iface, changed, invalidated, path=None):
        if iface == "org.bluez.Device1" and path and path.startswith(ADAPTER + "/dev_"):
            interesting = {k: v for k, v in changed.items() if k in
                           ("Connected", "Paired", "Bonded", "ServicesResolved", "Name", "Trusted")}
            if interesting:
                log(f"DEVICE {path.rsplit('/', 1)[-1]} {dict((k, str(v)) for k, v in interesting.items())}")

    bus.add_signal_receiver(on_props, dbus_interface=PROPS, signal_name="PropertiesChanged",
                            path_keyword="path")

    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 15, lambda: loop.quit() or False)  # SIGTERM: clean up too
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    finally:
        for f in (lambda: advm.UnregisterAdvertisement(ADV), lambda: gatt.UnregisterApplication(APP),
                  lambda: adapter_props.Set("org.bluez.Adapter1", "Alias", old_alias)):
            try:
                f()
            except dbus.DBusException:
                pass
        log(f"Stopped, alias restored to {old_alias!r}")


if __name__ == "__main__":
    main()
