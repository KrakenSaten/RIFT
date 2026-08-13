# RIFT — Radio Intelligence & Field Terminal

A custom firmware UI for the **original LilyGO T-Deck**, built on upstream
[MeshCore](https://github.com/meshcore-dev/MeshCore).

RIFT turns the T-Deck into a standalone mesh terminal: read and write MeshCore
messages using the physical keyboard, with no phone or companion app involved,
plus passive Wi-Fi/BLE situational awareness and a view of the mesh topology.

Upstream's on-device UI is a status display — it shows an unread count and
previews incoming messages, but cannot compose. The companion phone app is what
normally does the typing. RIFT replaces that UI with one designed for a device
that has its own keyboard.

---

## Hardware

Original LilyGO T-Deck — **not** T-Deck Pro:

- ESP32-S3, 16 MB flash, 8 MB PSRAM
- SX1262 LoRa
- 320×240 ST7789 colour LCD
- Physical QWERTY keyboard (I²C co-processor at `0x55`)
- Trackball (4 directional lines + centre click)
- GT911 capacitive touchscreen at I²C `0x14`
- No onboard GPS

Note: the design document this project started from stated the original T-Deck
has no touchscreen. That is wrong - an I²C scan from the device found the GT911.
Worth remembering when trusting other claims in that document.

---

## Building

Two environment gotchas cost real time during development. Both are worth
getting right before the first build.

**1. PlatformIO must live in its own virtual environment.** Installing it into
a shared/global Python can upgrade packages other tools depend on.

```bash
python -m venv C:/dev/RIFT/.venv
C:/dev/RIFT/.venv/Scripts/python.exe -m pip install platformio
```

**2. `PLATFORMIO_CORE_DIR` must be an ASCII-only path.** The Windows toolchain
fails to find headers if the core directory contains non-ASCII characters — for
example a user profile like `C:\Users\AndréWågen\.platformio`. The failure looks
like missing `Arduino.h`/`Stream.h`, which is misleading.

```bash
export PLATFORMIO_CORE_DIR="C:/dev/.platformio"
```

Then build and flash:

```bash
pio run -e LilyGo_TDeck_rift
```

```bash
pio run -e LilyGo_TDeck_rift -t upload --upload-port COM5
```

If upload reports *"Could not open COM5, the port doesn't exist"* while the port
is clearly listed, a leftover `pio device monitor` process is holding it. Kill
it before retrying.

**First boot after switching firmware can take one to two minutes** while SPIFFS
reformats. The screen sits on `Loading...`. This is not a hang — wait it out.

### Other environments

`LilyGo_TDeck_rift` is additive. The four upstream T-Deck environments
(`_companion_radio_usb`, `_companion_radio_ble`, `_repeater`, `_kiss_modem`)
are unchanged and still build; that is worth re-checking after any edit to
shared code, since `MyMesh` and `AbstractUITask` are compiled into every
companion-radio build on every board.

---

## Screens and controls

**Trackball click is Enter** — it selects, activates and sends, the same as the
keyboard's Enter. Screen changes are covered by **rolling the trackball
left/right**, **double-click** (previous screen), or **tapping a nav-bar tab**.
**Backspace** goes one level back out of any sub-view.

| Screen | What it shows | Screen-specific controls |
|---|---|---|
| **MESH** | Dashboard: connection state, battery, node count, link RSSI/SNR, radio config, radar ping animation | — |
| **NODES** | Mesh topology — nodes placed by real hop distance, branched by the repeater they arrived through | trackball up/down selects a node, or tap one |
| **RADAR** | Passive Wi-Fi + BLE scatter and strongest-signal list | **Enter** toggles waterfall view; up/down scrolls the list |
| **COMMS** | MeshCore text terminal — a channel strip, message history, and a compose line | **tap a channel** in the strip to switch to it; **Enter** sends, or opens the target picker when the line is empty; **backspace** deletes; up/down scrolls history |
| **SYSTEM** | Action menu plus diagnostics: node name, keyboard status and last key code, I²C bus, free heap, external-power state, last reset reason, touch coordinates | up/down or tap selects; **Enter** activates. Actions: send advert, edit node name, add channel |

**Long-press the trackball within 8 seconds of boot** to enter MeshCore's CLI
rescue mode (upstream behaviour, preserved).

### Sending your first direct message

A recipient cannot decrypt a direct message from a node it has never heard an
advert from — it looks the sender up in its contacts and silently drops the
packet. So **send an advert from SYSTEM first**, and confirm the T-Deck appears
in the other node's contact list. Public-channel messages work without this,
because channels use a shared key.

Configured channels appear as a strip across the top of COMMS — tap one to switch
to it, with the active channel filled. Contacts are not in the strip (there can
be many); press Enter on an empty line for the full picker, which lists every
channel (marked `channel`) followed by contacts, most recently heard first
(marked `direct`). Repeaters and sensors are filtered out — nobody is reading
them. When a contact is the target, the strip shows `DM` rather than a
misleading channel selection.

Direct messages show delivery state (`...` → `ACK 1.2s`, or `no ack` on
timeout). Channel messages show none: they are always flooded and carry no
acknowledgement, so there is nothing truthful to display.

---

## Architecture

RIFT is a parallel UI implementation, selected entirely by build flags. It sits
alongside upstream's `ui-new`, `ui-orig` and `ui-tiny` rather than replacing
anything.

```
examples/companion_radio/ui-rift/     RIFT UI (all screens)
src/helpers/ui/ST7789NativeDisplay.*  native 320x240 display driver
src/helpers/ui/TDeckKeyboard.*        I2C keyboard driver
src/helpers/ui/TDeckTrackball.*       trackball directional driver
src/helpers/ui/TDeckTouch.*           GT911 touchscreen driver
variants/lilygo_tdeck/                +RIFT_* build flags and globals
```

**The protocol and radio layers are untouched.** MeshCore's mesh protocol,
RadioLib integration and SX1262 handling are used as-is.

Upstream's `ST7789LCDDisplay` treats the colour LCD as a scaled-up 128×64 OLED,
multiplying every draw call by ~2.5×/3.75×. `ST7789NativeDisplay` reports the
true 320×240 and draws 1:1. It is a separate driver because the original is
shared with two other boards.

Two shared files were extended, both additively:

- `AbstractUITask` gained `msgDelivered()` — **non-pure with an empty default**,
  so the other three UI implementations need no changes.
- `MyMesh` gained `sendTextTo()`, which registers the expected ACK in a table
  that is private to the class, and `processAck()` now notifies the UI as well
  as the serial link. It also gained channel-creation methods, because
  `saveChannels()` is private.
- `UIScreen` gained `handleTouch()`, also non-pure with an empty default.

`BaseChatMesh::addChannel()` is deliberately **not** used to add channels. It
writes at `num_channels`, which only counts channels added through that method
and stays 0 for channels restored from storage — so it would silently overwrite
an existing channel, including Public. The RIFT methods find a genuinely free
slot instead. This is a trap waiting for the next person.

Feature flags: `RIFT_DISPLAY`, `RIFT_INPUT_KEYBOARD`, `RIFT_INPUT_TRACKBALL`,
`RIFT_INPUT_TOUCH`, `RIFT_RADAR`. Each guards its own code, so features can be disabled
independently when bisecting a problem.

### Not blocking the radio

There is **no watchdog on the main loop** (`loopTaskWDTEnabled` is false and the
IDF task watchdog only monitors core 0 idle, while `loopTask` runs on core 1).
A blocking call does not panic — it silently starves the LoRa radio. That makes
this the most important constraint in the codebase.

Both scan APIs have a blocking overload that is easy to reach by accident:

- `WiFi.scanNetworks()` with default arguments blocks up to **10 seconds**.
- `BLEScan::start(duration, bool)` blocks for the full duration —
  `start(5, false)` silently resolves to it. The only non-blocking form takes a
  function pointer.

RADAR therefore runs as a non-blocking state machine, and Wi-Fi and BLE
alternate rather than overlap: they share one 2.4 GHz PHY and antenna, and the
coexistence arbiter halves both if run concurrently.

Scanning is **passive** — `passive=true` for Wi-Fi and `setActiveScan(false)`
for BLE, so nothing is transmitted. RIFT observes; it does not probe, inject or
deauthenticate.

---

## Known limitations and workarounds

Honest list. Each of these is a deliberate trade-off, not an oversight.

**BLE is never de-initialised.** `BLEDevice::deinit()` panics in this ESP32
Arduino core when a scan has recently been active, and no grace period made it
reliable. After the first visit to RADAR the BLE stack stays initialised and
idle until reboot, holding its heap. It transmits nothing in that state. To
compensate, `OFFLINE_QUEUE_SIZE` is reduced from upstream's 256 to 32 — that
queue exists to buffer messages for a disconnected companion app, which a
standalone device with its own message log barely needs, and at 177 bytes per
slot it was ~45 KB of static RAM doing very little.

**Message history is RAM-only** and does not survive a reboot. MeshCore stores
no messages itself, so this would mean new persistence code and flash wear.

**RADAR data is cleared when you leave the screen.** Stale channel occupancy
presented as current would be actively misleading.

**The waterfall is not a spectrum analyser.** The ESP32 gives no access to raw
RF. What is plotted is observed 802.11 activity — the strongest signal seen per
channel, one row per sweep.

**No per-node signal strength in NODES.** Adverts are cached without RSSI, so
brightness encodes recency instead of link quality.

**Nordic characters can be read but not typed.** Incoming UTF-8 is mapped to
CP437, which carries æ Æ å Å ä Ä ö Ö; ø and Ø are absent from the font entirely
and are drawn by the display driver as the base letter plus a stroke. Input is a
different problem: the keyboard is a US QWERTY with no Nordic keys, and no
alt/sym combination produces distinct codes for them, so entering them would
need a software compose scheme (a dead key, or a cycle key). Not implemented —
reading was the part that actually hurt.

**Keyboard arrow keys are unreachable.** `TDeckKeyboard` discards bytes above
127, which is where the arrow codes live. The trackball provides directions
instead, so this has not mattered in practice.

### An upstream bug worked around here

`variants/lilygo_tdeck/target.cpp` runs the RTC auto-discovery I²C probe
*before* `Wire.begin(18, 8)` on the following line, so the bus is touched
uninitialised. The symptom is an apparently empty I²C bus — the keyboard is not
found at all. `TDeckKeyboard::begin()` detects an empty bus and re-initialises
it on the known pins. **The real fix belongs upstream**, in the ordering.

Similarly, `DisplayDriver::printWordWrap()` is only a default that forwards to
`print()` and is not overridden by the native driver, so Adafruit GFX wraps to
`x=0` rather than the left margin. RIFT does its own wrapping. `ui-new` on other
colour-LCD boards likely has the same latent issue.

---

## Upstream MeshCore

RIFT is a fork of [MeshCore](https://github.com/meshcore-dev/MeshCore) by Scott
Powell / rippleradios.com, MIT licensed (see `license.txt`). The mesh protocol,
RadioLib integration and radio drivers are upstream's work, used unmodified.

The rest of this repository is upstream's tree: other boards, the repeater and
room-server examples, the companion protocol, and so on. Useful references:

- [`docs/MeshCore-README.md`](docs/MeshCore-README.md) — upstream's own README:
  prebuilt firmware, the flasher, client apps, and their roadmap
- [`docs/companion_protocol.md`](docs/companion_protocol.md) — frame protocol,
  and the channel key definitions RIFT follows
- [`docs/faq.md`](docs/faq.md) — MeshCore questions unrelated to RIFT

If you want plain MeshCore rather than RIFT, get it from upstream — this fork
only adds a T-Deck UI and does not track their releases.

---

## Status

Every screen from the original design concept is implemented and verified on
physical hardware: MESH, CONSTELLATION (NODES), RADAR, RF WATERFALL, COMMS.

Resource use: ~49 % of internal static RAM, ~24 % of the 6.5 MB app partition.

Touch, channel creation, node renaming and Nordic character rendering were added
afterwards from field use.

Possible next steps, roughly by value:

- Persist message history across reboots
- A compose scheme for typing Nordic characters (see limitations)
- Report the `Wire.begin()` ordering bug upstream
- Per-contact detail view (paths, keys, last-heard history)
