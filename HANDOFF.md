# RIFT — working handoff

Written to be loaded into a fresh Claude Code session. The README is the public
front page; this is the working notes. Everything here was learned by doing, and
several items cost hours the first time.

**Repo:** https://github.com/KrakenSaten/RIFT — default branch `rift-tdeck`
**Current:** v0.5.0
**Hardware:** original LilyGO T-Deck (ESP32-S3, 320×240 ST7789, QWERTY, trackball,
GT911 touch, SX1262 LoRa)

RIFT is a fork of [MeshCore](https://github.com/meshcore-dev/MeshCore) by Scott
Powell and contributors (MIT). The mesh stack, LoRa drivers and protocol are
theirs. RIFT adds a keyboard-driven on-device UI so the T-Deck works standalone,
with no phone app.

---

## 1. Environment

`BUILDING.md` has the setup, the three gotchas that waste an afternoon (PlatformIO
in its own venv, an ASCII-only `PLATFORMIO_CORE_DIR`, and no host compiler for the
tests), the upload failures and what does not fix them. It is not repeated here -
two copies means one of them is wrong and you cannot tell which.

This machine:

```bash
cd /c/dev/RIFT
export PLATFORMIO_CORE_DIR=C:/dev/.platformio
.venv/Scripts/pio.exe run -e LilyGo_TDeck_rift
.venv/Scripts/python.exe tools/rift-flash.py --port COM5
```

Build all five T-Deck environments before committing. RIFT edits shared code, so
`_companion_radio_usb`, `_companion_radio_ble`, `_repeater` and `_kiss_modem` are
the regression check:

```bash
.venv/Scripts/pio.exe run -e LilyGo_TDeck_rift -e LilyGo_TDeck_companion_radio_usb -e LilyGo_TDeck_companion_radio_ble -e LilyGo_TDeck_repeater -e LilyGo_TDeck_kiss_modem
```

There is no host g++ on this box, so `pio test` cannot run here. The suite is built
with the zig compiler from the venv instead, which is how it gets verified before a
push rather than by CI discovering it:

```bash
GT=.pio/libdeps/native/googletest/googletest
.venv/Scripts/python.exe -m ziglang c++ -std=c++17 -w -I "$GT/include" -I "$GT"   test/test_rift_logic/test_rift_logic.cpp "$GT/src/gtest-all.cc" -o /tmp/rl.exe && /tmp/rl.exe
```

---

## 2. Where things are

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
| `design/` | The screen design handoff: spec, PNGs, original archive |
| `flasher/` | Browser flasher page and manifest |
| `test/` | Native googletest suites |

The UI is selected purely by build flags. `ui-new`, `ui-orig`, `ui-tiny` and
`ui-rift` are parallel implementations behind the same `UIScreen` seam.

---

## 2b. The screen coming on is the notification

Stated as a requirement rather than left as an implementation detail, because it
is easy to mistake for one and remove.

**This board has no sounder and no vibration motor.** `PIN_BUZZER` and
`PIN_VIBRATION` are not defined for the T-Deck variant, so `UITask::notify()`
compiles to nothing at all - even though `MyMesh` calls it correctly for both
contact and channel messages. The whole notification path exists in software with
no output device at the end of it.

So a message arriving while the display is dark has exactly one way to announce
itself: `newMsg()` calls `turnOn()`. That line is the notification. It is
suppressed only when a companion app is attached, because then the phone is doing
the notifying.

`MSG WAKE` on SYSTEM shows how many times it has fired and how long ago, so a
report of "it did not notify me" can be checked rather than reasoned about - the
alternative being three plausible explanations and no way to choose between them.

Audio through the ES8311 codec is the only route to an audible alert and is
deliberately deferred: it needs I2S and codec bring-up, which is real work rather
than a flag.

## 3. Hardware facts that cost time

Every one of these was wrong on the first assumption and only settled by putting
the actual value on screen. **When something does not behave, display the value —
do not reason about it.** That pattern resolved four separate problems faster
than any amount of code reading.

- **Trackball pins** are UP=3, DOWN=15, LEFT=1, RIGHT=2. Guessing gave a ball
  that scrolled the wrong way. Sourced from LilyGO's `utilities.h` and Meshtastic.
- **GT911 touch** reports X at byte offset 0–1 and Y at 2–3 — *not* the
  datasheet's track-id-first layout. Calibration: display top-left = raw (228, 8),
  bottom-right = raw (6, 310). Status register 0x814E, bit 7 = data ready, and it
  must be written back to 0 or the controller stops reporting.
- **Keyboard** is I²C 0x55, polled at 20 ms - `KEYBOARD_POLL_MILLIS` in the rift
  environment. The 100 ms in `TDeckKeyboard.h` is only the fallback for a build
  that does not set it; at 100 ms keystrokes were being missed.
- **There is no key repeat and no key-up, so a long press cannot be detected.**
  Measured: holding a key produces exactly one event, and every read after it
  returns 0 for as long as the key stays down. Holding `a` in a text field types
  one `a`, however long you hold. The comment in `TDeckKeyboard::poll()` used to
  claim the opposite — that a held key repeats and edge detection is needed to
  suppress it — and a whole trigger design was built on that before anyone put the
  number on screen. It now says what was measured. Anything needing a second
  gesture has to be built from discrete presses; the Nordic picker uses a double
  tap for exactly this reason.
- **`SYM` emits nothing on its own, and is not a spare key.** `SYM`+letter emits
  the symbol printed on the key, as plain ASCII well under 127, so it already
  reaches the app: `SYM`+a is 42 `*`, +o is 43 `+`, +e is 50 `2`. It is a working
  symbol layer, and repurposing it would cost `* + / : ; ' " @ #` and the digits.
  Settled by putting `TDeckKeyboard::lastSeen()` — the raw byte, recorded before
  the >127 filter — next to the filtered one on SYSTEM. Those two numbers
  differing is how to tell a key being discarded from a key that does not exist.
- **I²C after the RTC probe is broken** until the bus is restarted. A transaction
  to an absent address took ~920 ms, and two callers walk all 112 addresses —
  that was 243 seconds of boot. `radio_init()` now does `Wire.end()` then
  `Wire.begin(18, 8, 100000)`. Why the probe leaves it that way is still not
  established; this treats the symptom.
- **External power** is `HWCDC::isPlugged()`. `(bool) Serial` resolves to
  `isCDC_Connected()`, which needs the host to *open* the port and returns false
  by design.
- **CP437 font** has æÆåÅäÄöÖ but no ø/Ø — those are drawn manually as the base
  letter plus a stroke, keyed on bytes 0x01 and 0x02. Those happen to be CP437's
  two smiley faces, so they are spent: an incoming smiley cannot be mapped there.
- **Two byte values are undrawable.** `Adafruit_GFX::write()` special-cases 0x0A
  as a newline and 0x0D as a carriage return before `drawChar` ever sees them, so
  a glyph mapped to either renders as nothing at all. 0x0D is CP437's eighth
  note, and mapping music there made it vanish. `riftNoGlyphAt()` in
  `RiftLogic.h` states the rule and a test walks the whole mapping table to
  enforce it.
- **No watchdog on the main loop.** `loopTaskWDTEnabled = false`, and the IDF task
  WDT only watches core 0 idle. A blocking call will not panic; it silently
  starves LoRa. `WiFi.scanNetworks()` with default arguments blocks 10 s, and
  `BLEScan::start(duration, bool)` blocks the full duration — only the
  function-pointer form is non-blocking.
- **`BLEDevice::deinit()` panics** after a recent scan. The stack is left up and
  idle instead; it transmits nothing once the scan stops.
- **SPI is shared** between the TFT and the SX1262, so redraws stay coarse.

---

## 4. Protocol details worth knowing

- **`path_len` is not a hop count.** Bits 6–7 hold the hash size minus one, bits
  0–5 the hop count. `Packet::pathHashSize()` / `pathHashCount()` decode it —
  use them, do not re-derive. Reading it raw showed a two-hop route as 66 hops at
  the 2-byte setting.
- **A path hash is a prefix of the repeater's public key** (`Identity::copyHashTo`
  is a straight memcpy from `pub_key`), so a hop can be matched against nodes we
  have heard adverts from. At one byte it collides once in 256 — resolve to
  ambiguous rather than naming the first match.
- **Hashtag channel keys** are the first 16 bytes of `sha256(name)` *including*
  the `#`. Test vector: `sha256("#test")[:16] == 9cd8fcf22a47333b591d96a2b848b73f`.
- **Channel messages get `"<sender>: "` prepended** and are then truncated at
  `MAX_TEXT_LEN` (160). The composer must size against that or the history shows
  text longer than what was sent.
- **`msgcount` in `newMsg`/`msgRead` is `offline_queue_len`** — how far behind a
  connected companion app is. It is *not* the device's unread count, and driving
  UI state from it lets a phone navigate the terminal.

---

## 5. The screen design

`design/DESIGN-HANDOFF.md` is the current reference: measured palette and contrast,
the RGB565 constraint, and the NODES problem stated so it can be acted on.
`design/handoff.md` is the original concept and predates the chrome rework.
holds renderings. Two rules run through it:

- **Never encode data in brightness where shape can carry it.** Reflected light
  outdoors lifts black toward grey and the dark steps of a ramp collapse.
  Freshness in NODES and the FAR band in RADAR are filled versus hollow.
- **De-emphasis inverts with the field.** On white, dim must be *darker*. The
  accent `#FF4100` keeps its value in both modes but changes role: legible as
  text on black (6.0:1), fill-only on white (3.5:1).

Night/day palettes are in `UITask.h` as `RiftPalette`; `riftApplyPalette()` also
assigns the shared `UIColor` statics so nothing half-changes. Toggled from SYSTEM.

Two things in the spec could not be built as drawn, both documented: the splash
progress bar has nothing to animate against (`SPIFFS.begin()` blocks the only
running task and reports no progress), and day mode is a menu item rather than
bound to backlight level, because this panel's backlight is on or off.

---

## 6. State of play

**Done and verified on hardware:** all five screens redesigned; passive Wi-Fi/BLE
RADAR with waterfall; COMMS with channel tabs, DMs and delivery status; NODES as
hop columns with real route drawing; SYSTEM in two columns with diagnostics;
touch, trackball-as-Enter, node renaming, channel creation, Nordic character
rendering; browser flasher; boot in 5.1 s. MESH now headlines mesh receive
activity — `NO SIGNAL` at boot and `IDLE` after a minute both seen on hardware,
with the age and packet count ticking alongside.

Screens now have a lifecycle. `RiftScreen` adds `onEnter`/`onLeave`/`isModal`/
`isOverlay`, the message preview is a real overlay drawn over the screen the user
is on, and dismissing it hands that screen back instead of dropping them on MESH.
Checked on device against both bugs the old arrangement caused: a message
arriving mid-scan no longer disturbs RADAR, and no longer wipes a channel key
being read on SYSTEM.

The popup lists the six newest messages rather than showing one at a time with
ENTER to step through — paging cost N presses to clear N messages and told you
less. ENTER now opens COMMS, which is where the full history and scrolling live.
Rows follow the COMMS idiom so it reads as a shorter COMMS. The alert box uses
the same panel language, `bg` fill and accent border, sized to its text: it used
to fill with `bar`, a band two percent from its own background, and read as a
stray outline.

Incoming emoji no longer arrive as runs of blocks. Invisible code points —
variation selectors, ZWJ, skin tones — are dropped rather than drawn, consecutive
unmappable ones collapse to one block, and the rest map to real glyphs or ASCII
emoticons where a genuine equivalent exists.

The 16px title bar is gone. The wordmark moved into the nav bar — the first tab
reads `RIFT`, and takes the accent while active, as text on black and as a fill
with the letters reversed out on white — and the battery moved to the nav bar's
right-hand slack, so all the chrome is on one edge. Per-screen context became
`renderHeading()`: the same text at the top of the body with no band behind it.
Three of the fourteen old call sites only repeated the nav bar and draw nothing
now; eleven carried real state and kept it. MESH also fronts `meshcore.io` above
the state, which distinguishes the device from Meshtastic and is something the
firmware knows for certain.

COMMS was the only screen genuinely short of room, and it is the one that varies:
with a channel target there is no heading, so the strip sits at the top and the
history takes the reclaimed 16px, which is one more message. With a contact target
the heading is the only place the target appears — the strip holds channels
only — so it stays and the strip drops. Nothing jumps, because the history draws
bottom-up.

**CI** (`.github/workflows/rift-build-check.yml`, `rift-release.yml`): tests gate
the builds, all five environments build, third-party actions pinned to SHAs,
read-only by default with write only on the publish job, and a release is refused
if the tag and `RIFT_VERSION` disagree.

**Warnings are on for RIFT's own code** (`-Wall -Wextra`, `-w` unflagged) and it
compiles clean. `-Werror` is off while ~210 warnings in shared MeshCore code are
not ours to fix.

**95 native tests**, all runnable locally. They cover the places that have
actually been wrong: base64 key validation, `path_len` decoding, hash collision
resolution, the mesh-activity thresholds, screen-transition hooks, the UTF-8 to
CP437 translation, and the Nordic variant table — including an assertion that
every variant is two-byte UTF-8 with a glyph the panel can draw back, since
confusing the wire form with the display form is this feature's sharpest trap.

### Open items

1. **The `HOPS` distribution still has not been read with more than one node.**
   SYSTEM reports `n` nodes / `max` hop count / `3+` beyond the third column, and
   two NODES changes wait on it: moving the `HEARD` count out of the heading, and
   rebucketing the hop columns, since the report is that almost everything lands in
   `3+`. Do both together; separately they shift the same layout twice. The buckets
   were guessed once already, which is why this waits for a number.

   It read `n0` for a long time and that was **not** a bug, though it was diagnosed
   as one twice. `advert_paths`, which every RIFT node list reads, lives in RAM and
   is cleared at startup, so it holds only what has been heard since boot - and
   channel messages do not create contacts. A device receiving plenty of traffic can
   have an empty table, and reflashing restarts it. Send an advert from another node
   and `n` moves immediately.

   **Seeding it from the persisted contacts was tried and reverted** (`2e0f0171`,
   reverted in `bac7f804`). A stored contact with no known route carries
   `out_path_len` `0xFF`, which means "flood, route unknown" and not a hop count, so
   every contact landed in a 63-hop column - `0xFF & 63`. The screen sorts by hops,
   so seeding needs somewhere honest to put "route unknown", and that decision
   belongs to the column redesign rather than ahead of it. Do not retry it as a
   standalone change.

2. **Nordic character input — done, one thing left to confirm.**

   Double-tapping a base vowel in COMMS opens a picker: `a` offers æ å ä, `o`
   offers ø ö, and the uppercase forms follow. Verified on hardware in both cases.
   Both taps are inserted as ordinary letters first and the picker replaces the
   pair, so cancelling leaves exactly what was typed — which is what makes a false
   trigger on a genuine double vowel (Haakon, Aage) cost one keypress.

   A double tap rather than a long press because a long press cannot be detected
   at all — see section 3. That was measured only after building the wrong thing.

   The compose line is UTF-8 aware: it renders by translating first and taking the
   tail of the *translated* text, so a cut cannot land inside a sequence; backspace
   removes a whole code point; and the send path truncates on a code point
   boundary. All three use `mesh::validUtf8PrefixLength`, which upstream already
   has and tests. Two were live bugs — a byte-wise backspace left a dangling lead
   byte, and the send truncation could put invalid UTF-8 on the air once the
   capacity dropped below the composed length (compose a long DM, switch the target
   to a channel).

   The character counter counts bytes, deliberately: MeshCore truncates at 160
   bytes, so a Nordic character really does cost two.

   **Still unconfirmed:** that a Nordic character survives the trip to another
   client. Nothing local can check it — the panel would look identical whether the
   wire carried UTF-8 or CP437. Send one to a phone and read it there.

   Sharpest trap, still: `ø` on the air must be UTF-8 `0xC3 0xB8`. `0x01` is a
   display-side placeholder only, and putting it in outgoing text sends a C0
   control byte that other clients may mangle or truncate.
3. **Bundle ESP Web Tools** instead of loading it from unpkg. Pinning the version
   is only partial — the `?module` form resolves its dependencies from the CDN at
   load time. Real self-hosting needs an npm build step.
4. **`.github/actions/setup-build-environment`** is upstream's and still refers to
   two actions by tag. Pinning there would change their workflows too.
5. **`QUIET` has never been seen on hardware.** It needs a quarter of an hour of
   silence, so only the threshold is covered, by native test. `NO SIGNAL` and
   `IDLE` were both confirmed on device.
6. **Report the I²C bug upstream.** Measurements are in commit `3528d80a`. The
   fix took this device's boot from 243 s to 5.1 s; every other T-Deck user on
   MeshCore is still paying the four minutes and does not know why. It is the
   largest effect available outside this repo. Outward-facing, so it needs an
   explicit go-ahead — asked for and deferred as of 0.5.0, not declined.
7. **Why a long flash write fails at all.** Known to be the transfer length rather
   than the address, but not *why* the transport gives up there. The threshold moves
   with the image: four parts was fine at 1.54MB and fails on the third chunk at
   1.55MB, where eight parts goes through. `tools/rift-flash.py` now derives the
   count from a 192KB target and retries a failed chunk four times, so nothing has
   to be remembered - but the cause is still unknown, and a partly written image is
   a device that will not boot until the tool is run again.
8. **Emoji still show as blocks past the mapped set**, and the user reports that
   as too many for current message traffic. The limit is now fundamental rather
   than a gap in the table: CP437 has no emoji, and what remains has no honest
   ASCII synonym — mapping it anyway would put a claim on screen the sender did
   not make. The route is custom glyphs, drawn through the same `print()`
   interception that synthesises `ø`. Bounded, but the cell is 6x8 with no
   antialiasing, so the list has to be chosen for shapes that survive that size.
   An unrecognisable custom glyph is worse than a block: a block admits it cannot
   draw the thing, a vague shape looks like it means something specific.

---

## 7. How this project has worked

Worth keeping, because it is what actually produced results.

**Measure, do not reason.** Four hardware problems and one CI failure were all
resolved by displaying or logging the real value after reasoning had failed —
sometimes twice. The SYSTEM screen's diagnostics (last key, I²C scan, free heap,
reset reason, touch coordinates, boot timings) were never on any requirements
list and are the most valuable thing in the firmware.

**Verify a review's claims before acting on them.** Both external reviews were
substantially correct, but each had one item where the mechanism was wrong even
though the conclusion held, and acting on the stated mechanism would have fixed
the wrong thing.

**Fixes introduce bugs.** Three findings in the second review were regressions
from the first round's fixes. Re-check the blast radius of a fix, especially when
it adds a hook that fires on a shared code path.

**Say what was not verified.** Several things here are workarounds for causes
that are still unknown, and they are labelled as such in the code and commits.

---

## 8. Starting a session at home

```bash
git clone https://github.com/KrakenSaten/RIFT.git
cd RIFT
git checkout rift-tdeck
```

Then set up the venv and `PLATFORMIO_CORE_DIR` per section 1, install the host
compiler, and confirm the baseline before changing anything:

```bash
pio test -e native -e native_kiss_modem && pio run -e LilyGo_TDeck_rift
```

60 tests and a clean build means you are where this handoff left off.
