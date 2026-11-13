# Linux prototype (BlueZ)

The first version of the bridge, used to work out the protocol before porting it to the ESP32-S3. It runs on a
Linux PC with BlueZ (tested: 5.82, Intel AX211) and Python 3 with `dbus-python` and PyGObject. Kept for
reference; the [firmware](../firmware) is the maintained implementation.

| Script | Purpose |
|---|---|
| `pair_remote.py ADDR` | wait for the remote to advertise, then pair, trust and connect (Just Works) |
| `dump_gatt.py ADDR` | dump all services, characteristics and descriptors with values (read-only) |
| `fake_remote.py` | GATT server + advertisement copying the remote; logs everything a TV reads/writes |
| `fake_remote.py --relay [--fresh]` | the bridge: relays between the real remote and the TV (`relay.py`), PC mode via uinput (`pc_mode.py`, reloadable with `kill -USR1`) |
| `snoop_att.py FILE` | list ATT PDUs from a `btmon -w` capture |

## Requirements

- **bluetoothd without the LE HID plugin:** `bluetoothd -P hog` (e.g. a systemd drop-in overriding `ExecStart`).
  Otherwise BlueZ refuses D-Bus writes to the remote's HID service, which the relay needs for the TV's
  commands. BLE keyboards/mice don't work on that PC while this is active.
- `/dev/uinput` writable for PC mode.
- Environment for `--relay`: `MRB_REMOTE=AA:BB:CC:DD:EE:FF` (the remote).

## Known limitations

- BlueZ forgets the TV's notification subscriptions when the GATT application is re-registered, and the bonded
  TV doesn't subscribe again: after restarting the relay, pair again (Home+Back+OK).
- BlueZ's default `JustWorksRepairing = never` stalls pairing when the TV re-registers while BlueZ still has its
  bond: remove the TV with `bluetoothctl remove` while it is disconnected.
- Without the hog plugin BlueZ doesn't auto-connect the sleeping remote; the relay keeps a `Connect()` pending.
