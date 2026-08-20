# RIFT architecture

Moved out of the README. The reasoning here is worth keeping — it is what shows
the choices were deliberate — but it is not front page.

See also [`HANDOFF.md`](HANDOFF.md) for the working notes, and
[`design/handoff.md`](design/handoff.md) for the screen design at 1:1 device
coordinates.

---

## A parallel UI, selected by build flags

RIFT sits alongside upstream's `ui-new`, `ui-orig` and `ui-tiny` rather than
replacing anything.

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

### Shared files, extended additively

- `AbstractUITask` gained `msgDelivered()` — non-pure with an empty default, so
  the other three UI implementations need no changes.
- `MyMesh` gained `sendTextTo()`, which registers the expected ACK in a table
  private to the class, and `processAck()` now notifies the UI as well as the
  serial link. It also gained channel-creation methods, because `saveChannels()`
  is private, and mesh receive-activity accessors (`hasHeardMesh()`,
  `getLastRxMillis()`, `getRxCount()`) because nothing tracked that at all.
- `UIScreen` gained `handleTouch()`, also non-pure with an empty default.

`BaseChatMesh::addChannel()` is deliberately **not** used to add channels. It
writes at `num_channels`, which only counts channels added through that method and
stays 0 for channels restored from storage — so it would silently overwrite an
existing channel, including Public. The RIFT methods find a genuinely free slot
instead. This is a trap waiting for the next person.

### Screen lifecycle

`UIScreen` has `render`, `handleInput`, `handleTouch` and `poll`, and no notion of
entering, leaving, modality or overlays. RIFT adds those in a local `RiftScreen`
base class rather than in the shared header, to keep the diff against upstream
small — every screen `UITask` holds is a `Rift*` class, so nothing outside
`ui-rift` is affected.

- `onEnter` / `onLeave` fire only on real navigation. The rule lives in
  `riftScreenTransition()` in `RiftLogic.h`, where it is tested, because it has
  been wrong in shipped firmware twice — both times the same mistake, a transient
  popup treated as navigation. Once it tore the BT controller down mid-scan and
  panicked the device; once it wiped a one-time channel key while it was being
  read.
- `isModal()` is a screen saying it holds something a popup would destroy:
  half-typed text, or a secret on screen.
- `isOverlay()` marks a popup. An overlay never becomes the current screen and
  never touches `nav_idx`, so dismissing it restores nothing and the screen
  underneath is never told it was left. That removes the need for the guard that
  caused both bugs rather than centralising it.

Overlays render after the current screen into the same canvas, which is the
mechanism `showAlert()` already used — one bulk transfer either way.

---

## The display driver

Upstream's `ST7789LCDDisplay` treats the colour LCD as a scaled-up 128x64 OLED,
multiplying every draw call by ~2.5x/3.75x. `ST7789NativeDisplay` reports the true
320x240 and draws 1:1. It is a separate driver because the original is shared with
two other boards.

It also double-buffers. Drawing straight to the panel meant every repaint cleared
the screen and rebuilt it, which showed as a black flash — badly while typing,
since each keystroke forces a redraw. Frames are composed in a 150KB `GFXcanvas16`
(which lands in PSRAM) and blitted once in `endFrame()`. This costs *less* SPI time
than before, because the old path wrote a full frame of background and then
overwrote most of it. If the buffer cannot be allocated the driver falls back to
drawing directly, flicker and all.

### Two glyphs are synthesised, and two byte values are unusable

CP437 has æÆåÅäÄöÖ but no ø/Ø. Those are drawn as the base letter plus a stroke,
keyed on bytes `0x01` and `0x02` intercepted in `print()`. Those two happen to be
CP437's smiley faces, so they are spent: an incoming smiley cannot be mapped there.

`Adafruit_GFX::write()` special-cases `0x0A` as a newline and `0x0D` as a carriage
return before `drawChar` ever sees them, so a glyph mapped to either renders as
nothing. `0x0D` is CP437's eighth note, and mapping music there made it vanish.
`riftNoGlyphAt()` in `RiftLogic.h` states the rule, and a test walks the whole
mapping table to enforce it.

`DisplayDriver::printWordWrap()` is only a default that forwards to `print()` and
is not overridden by the native driver, so Adafruit GFX wraps to `x=0` rather than
the left margin. RIFT does its own wrapping. `ui-new` on other colour-LCD boards
likely has the same latent issue.

---

## Not blocking the radio

There is **no watchdog on the main loop** (`loopTaskWDTEnabled` is false, and the
IDF task watchdog only monitors core 0 idle while `loopTask` runs on core 1). A
blocking call does not panic — it silently starves the LoRa radio. That makes this
the most important constraint in the codebase.

Both scan APIs have a blocking overload that is easy to reach by accident:

- `WiFi.scanNetworks()` with default arguments blocks up to **10 seconds**.
- `BLEScan::start(duration, bool)` blocks for the full duration — `start(5, false)`
  silently resolves to it. The only non-blocking form takes a function pointer.

RADAR therefore runs as a non-blocking state machine, and Wi-Fi and BLE alternate
rather than overlap: they share one 2.4 GHz PHY and antenna, and the coexistence
arbiter halves both if run concurrently.

Scanning is **passive** — `passive=true` for Wi-Fi and `setActiveScan(false)` for
BLE, so nothing is transmitted. RIFT observes; it does not probe, inject or
deauthenticate.

**BLE is never de-initialised.** `BLEDevice::deinit()` panics in this ESP32 Arduino
core when a scan has recently been active, and no grace period made it reliable.
After the first visit to RADAR the BLE stack stays initialised and idle until
reboot, holding its heap. It transmits nothing in that state. To compensate,
`OFFLINE_QUEUE_SIZE` is reduced from upstream's 256 to 32 — that queue exists to
buffer messages for a disconnected companion app, which a standalone device with
its own message log barely needs, and at 177 bytes per slot it was ~45 KB of static
RAM doing very little.

---

## Private keys stay on the device

Upstream enables `ENABLE_PRIVATE_KEY_IMPORT` and `ENABLE_PRIVATE_KEY_EXPORT` by
default, with a comment in `platformio.ini` noting they should be off for more
secure firmware. With them on, `CMD_EXPORT_PRIVATE_KEY` writes the 64-byte private
identity straight out over serial and the matching import command replaces it. That
is a reasonable default for a radio you configure from a phone app; it is not one
for a standalone terminal that also enables `ENABLE_USB_INTERFACE`, where anything
with USB access could clone or hijack the node identity.

The RIFT environment strips both flags via `build_unflags`, and both commands
answer with MeshCore's existing "disabled" response. Nothing else changes — the
stock T-Deck environments keep upstream's behaviour.

A consequence worth stating: since export is disabled, there is no way to back the
private key up or recover it. A full chip erase makes the device a new node,
permanently.

---

## An upstream bug worked around here

**The I²C bus is left unusable by the RTC probe, and it cost four minutes of
boot.** In `variants/lilygo_tdeck/target.cpp`, `rtc_clock.begin(Wire)` runs the RTC
auto-discovery probe. Afterwards, a transaction to an address nothing answers on
takes about **920 ms**, where a NACK should cost microseconds — measured on
hardware, and eighteen times the Arduino default timeout of 50 ms, so it is not the
timeout doing it.

Two callers walk all 112 addresses: `EnvironmentSensorManager::begin()` and
`TDeckKeyboard::begin()`. That was 113 and 125 seconds. Boot took **243 seconds**,
and none of it was visible — `setup()` prints nothing to the display, so it simply
looked like a hang.

Ending and restarting the bus restores normal timing, so `radio_init()` now does
`Wire.end()` then `Wire.begin(18, 8, 100000)` immediately after the probe. Boot went
from 243 seconds to **5.1 seconds**, of which 2.5 is the splash screen and 1.0 is
the GPS detection delay. `TDeckKeyboard::begin()` already did exactly this recovery
to find its co-processor at all; doing it centrally means every user of the bus gets
a working one. That code stays as a safety net.

This is a workaround, not a diagnosis: **why** the probe leaves the peripheral in
that state is not established, and the honest fix belongs upstream. All four stock
T-Deck environments have the same bug and get the same benefit, since the variant
file is shared. Boot-phase timings are on the SYSTEM screen — `boot:` and
`slowest:` — so a regression here is visible rather than mysterious.

---

## Where things are

| Path | What |
|---|---|
| `examples/companion_radio/ui-rift/UITask.cpp` | Every RIFT screen. ~2900 lines. The bulk of the work |
| `examples/companion_radio/ui-rift/UITask.h` | Palette, boot marks, nav constants |
| `examples/companion_radio/ui-rift/RiftLogic.h` | Pure functions extracted so they can be tested |
| `examples/companion_radio/MyMesh.cpp` | Channel creation, message send/receive, ack tracking |
| `examples/companion_radio/main.cpp` | `setup()`; boot screen and SPIFFS probe live here |
| `src/helpers/ui/ST7789NativeDisplay.*` | Native 320×240 driver, double-buffered via `GFXcanvas16` |
| `src/helpers/ui/TDeckKeyboard/Trackball/Touch.*` | Input drivers |
| `variants/lilygo_tdeck/platformio.ini` | The `LilyGo_TDeck_rift` environment |
| `variants/lilygo_tdeck/target.cpp` | Board init — the I²C fix lives here |
| `design/` | `DESIGN-HANDOFF.md` is current; `handoff.md` is the original concept |
| `flasher/` | Browser flasher page and manifest |
| `test/` | Native googletest suites |

The UI is selected purely by build flags. `ui-new`, `ui-orig`, `ui-tiny` and
`ui-rift` are parallel implementations behind the same `UIScreen` seam.

---

## The screen coming on is the notification

`PIN_BUZZER` and `PIN_VIBRATION` are undefined for this variant, so
`UITask::notify()` compiles to nothing - even though `MyMesh` calls it correctly.
The whole notification path exists in software with no output device at the end.

So `newMsg()` calling `turnOn()` **is** the notification, and removing that line
removes the feature. It is suppressed only when a companion app is attached, because
then the phone is notifying. `MSG WAKE` on SYSTEM counts how often it has fired, so
"it did not notify me" can be checked rather than argued about.

Audio through the ES8311 codec is the only route to a sound and is deliberately
deferred: I2S and codec bring-up is work, not a flag.

## The screen design

The five on-device screens follow a design handoff (`design/`, with the original
archive kept alongside it), written against this repo's actual constraints: the 6x8
Adafruit GFX cell with only whole-integer scaling, `DisplayDriver`'s primitives,
and the SPI bus shared with the SX1262. Every coordinate in it is a real device
coordinate.

It carries a night and a day palette — the same geometry with a swapped colour
table, toggled from SYSTEM. Two rules run through both:

- **Nothing encodes data in brightness where shape can carry it**, because a grey
  ramp is the first thing to go once reflected light lifts black toward grey
  outdoors. Freshness in NODES and the FAR band in RADAR are filled versus hollow
  instead.
- **De-emphasis inverts with the field** — on white, dim has to be darker, not
  lighter. The accent `#FF4100` keeps its value in both modes but changes role: at
  6.0:1 on black it may be text, at 3.5:1 on white it may only be a fill with white
  reversed out of it.

The design review also found three real bugs, all confirmed in the source before
anything was changed: nodes placed by `branch & 15`, so two nodes behind the same
repeater landed on the same pixel; a "link line" that was `fillRect(cx, cy, 1, 1)`,
a single pixel at the midpoint of the line it was meant to be; and radar blips
placed by array index, which moved whenever an unrelated device aged out of the
table. Implementing the spec precisely turned up a fourth: `AdvertPath::path_len`
is Packet's raw encoding, where bits 6-7 hold the hash size and bits 0-5 the hop
count, and reading it as a plain count made a two-hop route display as 66 hops at
the 2-byte path hash setting.

Two things in the spec could not be built as drawn. The splash progress bar has
nothing to animate against — `SPIFFS.begin()` blocks the only running task and
reports no progress — so the screen says what is happening instead. And day mode is
a menu item rather than being bound to backlight level, because this panel's
backlight is on or off with no level to read.

The chrome has since moved on from the spec: the 16px title bar was removed, its
wordmark and battery moved into the nav bar, and per-screen context became a
heading in each screen's own body. `design/DESIGN-HANDOFF.md` is the current
reference; `design/handoff.md` is the original concept.
