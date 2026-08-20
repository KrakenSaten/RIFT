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

## 6. State of play

All five screens work and are verified on hardware: MESH headlines mesh receive
activity, NODES draws hop columns with real routes, RADAR does passive Wi-Fi/BLE
with a waterfall and a proximity watch, COMMS has channel tabs with colours and
delivery status, SYSTEM has actions, diagnostics and a 128-line event log. Boot is
5.1 s. Screens have a lifecycle, so an arriving message no longer disturbs a scan
or wipes a key being read.

What changed in each release is on the
[releases page](https://github.com/KrakenSaten/RIFT/releases); the reasoning is in
the commit messages. Neither is repeated here.

### Open items

1. **`HOPS` has not been read with more than one node.** Two NODES changes wait on
   it: moving `HEARD` out of the heading, and rebucketing the hop columns, since
   almost everything lands in `3+`. Do both together — separately they shift the
   same layout twice. The buckets were guessed once already.

   Its cache is RAM-only and cleared at boot, and channel messages create no
   contacts, so an empty list after a restart is normal and was twice mistaken for
   a bug. Seeding it from stored contacts was tried and reverted (`2e0f0171` /
   `bac7f804`): a contact with no learned route carries `out_path_len` `0xFF` —
   "flood, route unknown", not 63 hops — and this screen sorts by hops. Do not
   retry it as a standalone change; it belongs to the column redesign.

2. **Nordic input works; one thing unconfirmed.** Double-tap a vowel in COMMS for a
   picker. Nothing local can check that a Nordic character survives the trip — the
   panel looks identical either way. Send one to a phone and read it there.
   `ø` on the air must be UTF-8 `0xC3 0xB8`; `0x01` is a display-side placeholder
   and putting it in outgoing text sends a C0 control byte.

3. **Bundle ESP Web Tools** instead of loading it from unpkg. Pinning is partial —
   the `?module` form resolves dependencies from the CDN at load time. Needs an npm
   build step.

4. **`QUIET` has never been seen on hardware.** Needs a quarter hour of silence;
   only the threshold is covered, by native test.

5. **Report the I²C bug upstream.** Measurements in `3528d80a`. The fix took boot
   from 243 s to 5.1 s and every other T-Deck user is still paying it. Outward
   facing, so it needs a go-ahead — asked for and deferred, not declined.

6. **Why a long flash write fails.** Known to be the transfer length, not the
   address, and the threshold moves with the image size. `tools/rift-flash.py`
   handles it; the cause is still unknown.

7. **Emoji show as blocks past the mapped set.** CP437 has none, and what remains
   has no honest ASCII equivalent. The route is hand-drawn glyphs in a 6×8 cell,
   which makes the discard list as much the deliverable as the glyphs: an
   unrecognisable glyph is worse than a block, because a block admits it cannot
   draw the thing.

8. **The NODES redesign.** `design/DESIGN-HANDOFF.md` §6 states the problem and the
   direction — summary buckets, a scrollable list, one selected route — with the
   four constraints that are not negotiable.

## 7. How this has worked

- **Measure, do not reason.** Every hardware problem here was resolved by putting
  the real value on screen after reasoning had failed - several of them twice. The
  SYSTEM diagnostics were on no requirements list and are the most useful thing in
  the firmware.
- **Verify a review's claims first.** Every external review was substantially right
  and most had one item where the stated mechanism was wrong, which would have
  fixed the wrong thing.
- **Fixes introduce bugs.** Several review findings were regressions from earlier
  fixes. Re-check the blast radius, especially for a hook on a shared path.
- **Say what was not verified.** Some of this is a workaround for a cause still
  unknown, and it is labelled as such in the code.
