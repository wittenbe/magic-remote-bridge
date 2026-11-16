# Usage

## First-time pairing

1. Flash the firmware and plug the board in (see [setup](setup.md)). With no remote paired it scans for one
   and advertises the TV-facing copy.
2. On the remote: hold **Home + Back** until the LED blinks, then press **OK** while pointing at the TV.
3. The TV shows "Registering…" and then "registration successful". Cursor and keys now go through the bridge.

If the TV says "registration failed", press OK again (the TV offers this). If the remote is already paired
with the bridge and should be paired fresh, run `forget` on the console first.

After pairing, both bonds are stored on the board; reboots and firmware updates need no new pairing.

**Giving the remote back to the TV:** unplug the bridge, then Home+Back+OK pointing at the TV.

## Switching between TV and PC

Hold **red** (leftmost colour key) for about a second. A short press of red still works on the TV. The switch
key and the hold time can be changed (`switchkey`, `switchhold`, or the [configuration page](#configuration-page)).

In PC mode the TV ignores the remote (its cursor disappears) and the remote acts as keyboard and mouse. The
default key map:

| Remote | PC |
|---|---|
| Pointer (move the remote) | mouse pointer |
| OK / wheel click | left click |
| Settings | right click |
| Wheel | scroll |
| Arrows | arrow keys |
| Back | Esc |
| Home | Windows key |
| Vol+ / Vol− / Mute | volume up / down / mute |
| Ch+ / Ch− | Page Up / Page Down |
| 0–9 | digits |
| Green / Yellow / Blue | F2 / F3 / F4 |

Guide, Input, "…", Voice and the app keys (Netflix, Prime Video, Disney+, Rakuten TV, Google Assistant, Alexa)
do nothing in PC mode by default. The power key is IR only and always controls the TV. The pointer and the scroll
wheel are fixed; every button can be remapped.

## Configuration page

`tools/config.html` edits the key map and the settings in the browser. It talks to the bridge's console over
WebSerial, so it needs **Chrome or Edge** on the PC the bridge is plugged into. Open the file directly (no server
needed), click **Connect** and pick the bridge. Close `console.py` first: on Windows only one program can open
the port.

- Click a button on the drawing, or press it on the remote, to edit it: nothing, a key (with Ctrl/Shift/Alt/Win,
  e.g. Win + H for dictation), a media key or a mouse button. **Record from keyboard** takes a key or shortcut
  pressed on the PC keyboard; combinations Windows keeps for itself (Alt+Tab, Ctrl+Alt+Del) are picked from the
  list instead.
- Any button can be the mode-switch key. A short press of it goes to the TV in TV mode and does its mapping in
  PC mode.
- Changes are stored on the board at once. The page reconnects by itself after a reset or re-plugging.

Mouse-button keys (OK by default) don't select themselves when pressed, since they are what clicks in the page.

## Settings

Stored on the board, changed with `set <name> <value>` on the console (see below):

| Name | Default | Meaning |
|---|---|---|
| `gain` | 0.016 | pointer base speed (px per gyro unit per report; ~0.016 ≈ 27 px per degree) |
| `accel` | 1.0 | pointer acceleration: fast movements up to 1 + 4·accel times faster; 0 = off |
| `tvsleep` | 0 | TV mode: seconds of idle before the TV's sleep command reaches the remote; 0 = immediately, like without the bridge |
| `pcsleep` | 10 | PC mode: seconds the remote has to lie still (put down, not hand-held) before the bridge puts it to sleep; 0 = never (drains the battery) |
| `dead` | 12 | PC mode: dead zone for very slow rotation, in gyro units (~17 per °/s); stops the cursor creeping while the remote is held still. Subtracted from all speeds, so slow aiming stays continuous |
| `drag` | 30 | PC mode: px the pointer must travel while a mouse button is held before it moves, so a click with a slightly moving hand stays a click; a real drag then catches up. 0 = off |
| `smooth` | 15 | PC mode: each pointer report's motion is spread over this many ms at the 1 kHz USB rate; 0 = send at once (choppier, 10 ms less latency) |
| `switchkey` | 0x8072 | button code that switches TV/PC mode when held (red); codes in [protocol](protocol.md#button-codes) |
| `switchhold` | 800 | ms the switch key must be held (up to 5000); 0 = switch on press, the key then does nothing else on the TV or PC |

A sleeping remote wakes on any key press; the first press is delivered.

## Console commands

| Command | |
|---|---|
| `status` | connections, subscriptions, mode, settings |
| `settings` / `set <name> <value>` | show / change settings |
| `mode` | toggle TV/PC mode (same as holding red) |
| `keymap` / `keymap reset` | list the PC-mode key map / restore the defaults |
| `map <code> key\|consumer\|mouse <usage> [mods]` | map a button: HID keyboard usage (+ modifier bits 1 Ctrl, 2 Shift, 4 Alt, 8 Win), consumer usage, or mouse buttons (1 left, 2 right, 4 middle, 8 back, 16 forward). Example: `map 0x80a1 key 0x0b 0x08` = Input → Win+H |
| `unmap <code>` | the button does nothing in PC mode |
| `timing` | toggle a once-per-second log of pointer report timing (arrival gaps, lost reports, USB reports sent) |
| `link <min> <max> <latency>` | request new connection parameters for the remote link (units of 1.25 ms) |
| `forget` | forget the paired remote and scan for a new one |
| `setremote AA:BB:CC:DD:EE:FF` | tell the bridge which bonded device is the remote (only needed after upgrading from firmware that didn't record it) |
| `dl` | reboot into download mode (used by `flash.py`) |
| `key <usage>` / `move <dx> <dy> [wheel]` | send a test keystroke / mouse movement over USB |
