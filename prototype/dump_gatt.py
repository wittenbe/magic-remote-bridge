#!/usr/bin/env python3
"""Dump every GATT service/characteristic/descriptor BlueZ exports for a device, reading values (read-only).

Note: BlueZ's HOG plugin claims the HID service (0x1812) and does not export it over D-Bus;
its attributes and values come from the btmon capture of the connect instead.
"""
import sys
import dbus

if len(sys.argv) < 2:
    sys.exit(f"usage: {sys.argv[0]} DEVICE_ADDR")
ADDR = sys.argv[1]
BUS = "org.bluez"


def hexs(v):
    return " ".join(f"{int(b):02x}" for b in v)


def main():
    bus = dbus.SystemBus()
    om = dbus.Interface(bus.get_object(BUS, "/"), "org.freedesktop.DBus.ObjectManager")
    objs = om.GetManagedObjects()
    dev_path = f"/org/bluez/hci0/dev_{ADDR.replace(':', '_')}"
    dev = objs.get(dev_path, {}).get("org.bluez.Device1")
    if not dev:
        sys.exit(f"{ADDR} not known to BlueZ")
    print(f"Device {ADDR}: Name={dev.get('Name')} Paired={dev.get('Paired')} Bonded={dev.get('Bonded')} "
          f"Connected={dev.get('Connected')} ServicesResolved={dev.get('ServicesResolved')}")
    print(f"  UUIDs: {[str(u) for u in dev.get('UUIDs', [])]}")
    print(f"  Appearance: {dev.get('Appearance')}  AddressType: {dev.get('AddressType')}")
    for k, v in dev.get("ManufacturerData", {}).items():
        print(f"  ManufacturerData[0x{int(k):04x}]: {hexs(v)}")
    for k, v in dev.get("ServiceData", {}).items():
        print(f"  ServiceData[{k}]: {hexs(v)}")

    paths = sorted(p for p in objs if p.startswith(dev_path + "/"))
    for p in paths:
        ifaces = objs[p]
        depth = p[len(dev_path):].count("/")
        indent = "  " * depth
        for iface, kind in (("org.bluez.GattService1", "SVC"), ("org.bluez.GattCharacteristic1", "CHR"),
                            ("org.bluez.GattDescriptor1", "DSC")):
            if iface not in ifaces:
                continue
            props = ifaces[iface]
            handle = props.get("Handle")
            hstr = f"0x{int(handle):04x}" if handle is not None else "?"
            flags = ",".join(str(f) for f in props.get("Flags", []))
            print(f"{indent}{kind} {hstr} {props['UUID']} {flags}")
            if kind == "SVC":
                continue
            # Read-only: never write. Reads on the OTA service d0ff are reads too, so they are allowed.
            if kind == "CHR" and "read" not in props.get("Flags", []):
                continue
            try:
                obj = dbus.Interface(bus.get_object(BUS, p), iface)
                val = obj.ReadValue({}, timeout=10)
                print(f"{indent}    = {hexs(val)}  {bytes(val)!r}")
            except dbus.DBusException as e:
                print(f"{indent}    ! {e.get_dbus_name()}: {e.get_dbus_message()}")


if __name__ == "__main__":
    main()
