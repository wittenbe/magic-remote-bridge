#!/usr/bin/env python3
"""Wait for the MR25GB to advertise, then pair (Just Works), trust and connect it via BlueZ D-Bus."""
import sys
import dbus
import dbus.service
import dbus.mainloop.glib
from gi.repository import GLib

if len(sys.argv) < 2:
    sys.exit(f"usage: {sys.argv[0]} REMOTE_ADDR [TIMEOUT_S]")
ADDR = sys.argv[1]
TIMEOUT_S = int(sys.argv[2]) if len(sys.argv) > 2 else 180
BUS = "org.bluez"
AGENT_PATH = "/rc/agent"


class Agent(dbus.service.Object):
    """NoInputNoOutput agent: accept everything (Just Works)."""

    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Release(self):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="")
    def RequestAuthorization(self, device):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="ou", out_signature="")
    def RequestConfirmation(self, device, passkey):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="os", out_signature="")
    def AuthorizeService(self, device, uuid):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Cancel(self):
        pass


def log(msg):
    print(msg, flush=True)


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    loop = GLib.MainLoop()

    Agent(bus, AGENT_PATH)
    mgr = dbus.Interface(bus.get_object(BUS, "/org/bluez"), "org.bluez.AgentManager1")
    mgr.RegisterAgent(AGENT_PATH, "NoInputNoOutput")
    mgr.RequestDefaultAgent(AGENT_PATH)

    adapter_path = "/org/bluez/hci0"
    adapter = dbus.Interface(bus.get_object(BUS, adapter_path), "org.bluez.Adapter1")
    dev_path = f"{adapter_path}/dev_{ADDR.replace(':', '_')}"
    state = {"busy": False}

    def stop_scan():
        try:
            adapter.StopDiscovery()
        except dbus.DBusException:
            pass

    def on_done(ok, msg):
        log(msg)
        stop_scan()
        loop.quit()
        if not ok:
            sys.exit(1)

    def pair():
        if state["busy"]:
            return
        state["busy"] = True
        log(f"Seen {ADDR}, pairing...")
        stop_scan()
        dev = dbus.Interface(bus.get_object(BUS, dev_path), "org.bluez.Device1")
        props = dbus.Interface(bus.get_object(BUS, dev_path), "org.freedesktop.DBus.Properties")

        def paired():
            log("Paired. Trusting + connecting...")
            props.Set("org.bluez.Device1", "Trusted", True)
            dev.Connect(reply_handler=lambda: on_done(True, "Connected."),
                        error_handler=lambda e: on_done(True, f"Paired, connect failed: {e}"),
                        timeout=30)

        dev.Pair(reply_handler=paired,
                 error_handler=lambda e: on_done(False, f"Pair failed: {e}"),
                 timeout=60)

    def on_iface_added(path, ifaces):
        if path == dev_path and "org.bluez.Device1" in ifaces:
            pair()

    def on_props(iface, changed, invalidated, path=None):
        if path == dev_path and ("RSSI" in changed or "ManufacturerData" in changed):
            pair()

    bus.add_signal_receiver(on_iface_added, dbus_interface="org.freedesktop.DBus.ObjectManager",
                            signal_name="InterfacesAdded")
    bus.add_signal_receiver(on_props, dbus_interface="org.freedesktop.DBus.Properties",
                            signal_name="PropertiesChanged", path_keyword="path")

    adapter.SetDiscoveryFilter({"Transport": "le", "DuplicateData": True})
    adapter.StartDiscovery()
    log(f"Scanning for {ADDR} (up to {TIMEOUT_S}s). Press a key on the remote now.")
    GLib.timeout_add_seconds(TIMEOUT_S, lambda: on_done(False, "Timed out, remote not seen."))
    loop.run()


if __name__ == "__main__":
    main()
