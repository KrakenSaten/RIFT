# RIFT — working handoff

Written to be loaded into a fresh Claude Code session. The README is the public
front page; this is the working notes. Everything here was learned by doing, and
several items cost hours the first time.

**Repo:** https://github.com/KrakenSaten/RIFT — default branch `rift-tdeck`
**Current:** v0.7.0
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

**Whether `pio test` runs depends on the machine**, so check rather than believing
either recipe. The suite needs a host compiler that PlatformIO does not ship.

With MSYS2 on PATH (`C:\msys64\ucrt64\bin`, GCC 16.2.0 on the machine this was last
verified from) the normal command works, and is the one to prefer:

```bash
.venv/Scripts/pio.exe test -e native -e native_kiss_modem
```

On a box without one, the zig compiler in the venv builds the suite directly, which
beats letting CI discover the failure:

```bash
GT=.pio/libdeps/native/googletest/googletest
.venv/Scripts/python.exe -m ziglang c++ -std=c++17 -w -I "$GT/include" -I "$GT"   test/test_rift_logic/test_rift_logic.cpp "$GT/src/gtest-all.cc" -o /tmp/rl.exe && /tmp/rl.exe
```

---

## 2. Hardware facts that cost time

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
  established; this treats the symptom. **Reporting it upstream was considered and
  declined** — boot is seconds here and the item is closed for this project. Do not
  re-open it as a task; the note stays because it explains why `radio_init()` does
  something that otherwise looks arbitrary.
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

## 3. Protocol details worth knowing

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

## 4. State of play

All five screens work and are verified on hardware: MESH headlines mesh receive
activity, NODES draws hop columns with real routes, RADAR does passive Wi-Fi/BLE
with a waterfall and a proximity watch, COMMS shows one
conversation at a time with a list to move between them, SYSTEM has actions, diagnostics and a 128-line event log. Boot is
5.1 s. Screens have a lifecycle, so an arriving message no longer disturbs a scan
or wipes a key being read.

The home screen also has one action: **discover 0-hop repeaters**. It broadcasts a
`DISCOVER_REQ` control packet with the type filter set to repeaters, and every
repeater in direct range answers. The response carries two SNR readings — the one
the repeater measured on our request, and the one our radio measured on the reply —
so this is the only thing in the firmware that can say whether they hear *us*.
Confirmed on hardware: the two values do differ. The responder side lives in
`examples/simple_repeater`, and the byte layout here is copied from it rather than
from `docs/payloads.md` so the two cannot drift. Zero-hop, and the label says so.

What changed in each release is on the
[releases page](https://github.com/KrakenSaten/RIFT/releases); the reasoning is in
the commit messages. Neither is repeated here.

### Open items

0. **Room servers in COMMS — the only part of the redesign not built.** The rest
   shipped in two commits, and `design/comms-redesign.md` records the direction. What
   remains is the third conversation kind, and it is blocked on something real rather
   than on effort.

   A room has to be **logged in** to over the radio before you can post, so a
   connect/connected status is exactly the right thing to want. But
   `checkConnections()`, which sends the keep-alives, is commented out in
   `MyMesh.cpp` with `TODO - deprecate the 'Connections' stuff`. So
   `startConnection()` would fill the table and `hasConnectionTo()` would answer yes
   while the server at the other end timed the session out — a status reading
   "connected" when it is not, which is worse than none. And upstream means to remove
   the mechanism it would rest on.

   `RIFT_CONV_ROOM` is reserved and unimplemented, so the structure takes it without
   rework. Before building it, one of these has to be true: upstream says what
   replaces Connections, or RIFT owns the keep-alive deliberately and accepts the
   divergence, or rooms work with a login per action and no persistent status —
   honest, but probably not what a room is for.

   **Queued, 2026-08-23: ask upstream rather than guess.** The blocker is not a
   missing feature, it is not knowing what a `TODO - deprecate` means in practice —
   whether Connections is being replaced, dropped, or simply left where it is. That is
   a question, and an issue asking it costs nothing and unblocks everything after it.

   A pull request is the weaker opening move here, and worth saying why before anyone
   spends an evening on one: uncommenting `checkConnections()` would be a patch that
   revives a mechanism its own maintainers marked for removal, which invites a no on
   grounds that have nothing to do with whether the patch is correct. Ask first, then
   offer the patch if the answer leaves room for it. If the answer is that rooms need
   a keep-alive and nothing replaces it yet, that is also the strongest possible case
   for the patch, and it will have been made by them rather than by us.

   A second thing to settle first: a room password is a secret on screen, the same
   class as the one-time channel key, which had to be wiped on leaving SYSTEM because
   coming back redisplayed it. Any password field inherits that requirement.

   Two smaller things the redesign left standing, both deliberate. Unread is
   session-only, so a dot does not survive a reboot. And a channel scrolled out of
   the four-tab strip shows no dot of its own — the nav bar still says something is
   unread somewhere, and the conversation list shows which.

1. **The NODES redesign, now measured.** `design/DESIGN-HANDOFF.md` §6 has the
   direction — summary buckets, a scrollable list, one selected route — with four
   constraints that are not negotiable. The readings that were missing are in:

   - **13 of 16 cached nodes sit beyond 2 hops, with a maximum of 7.** The four
     fixed columns therefore spend three quarters of their width on three nodes.
     The buckets are `riftHopBucket()` in `RiftLogic.h`, tested, with fixed ranges
     `DIRECT | 1-2 | 3-5 | 6+` — fixed on purpose, per §6: a column whose meaning
     moves with the network is one you cannot read.
   - **The path cache read `16/16, 64 lost`, so it was thrashing.** Raised from 16
     to 96 slots (+9.3 KB static; RIFT went 53.3% → 56.1% RAM, and NODES keeps its
     own heap copy so the real cost is about double). Below that, the list had a
     churning sample to scroll and the bucket counts described the sample rather
     than the mesh. **Re-read `PATH CACHE` on SYSTEM after this** — if it still
     evicts, 96 is not enough either.

   Its cache is RAM-only and cleared at boot, and channel messages create no
   contacts, so an empty list after a restart is normal and was twice mistaken for
   a bug. Seeding it from stored contacts was tried and reverted (`2e0f0171` /
   `bac7f804`): a contact with no learned route carries `out_path_len` `0xFF` —
   "flood, route unknown", not 63 hops — and this screen sorts by hops. Ask
   `riftHopsUnknown()` before decoding; `riftHopBucket()` has a bucket for it.

2. **Nordic input works, confirmed end to end.** Double-tap a vowel in COMMS for a
   picker. A Nordic character was sent to another client and arrived intact, which
   is the only check that distinguishes UTF-8 from CP437 — the panel looks identical
   either way. Kept here for the trap, not as an open task: `ø` on the air must be
   UTF-8 `0xC3 0xB8`; `0x01` is a display-side placeholder and putting it in
   outgoing text sends a C0 control byte.

3. **Bundle ESP Web Tools** instead of loading it from unpkg. Pinning is partial —
   the `?module` form resolves dependencies from the CDN at load time. Needs an npm
   build step.

4. **`QUIET` has never been seen on hardware.** Needs a quarter hour of silence;
   only the threshold is covered, by native test.

5. **Why a long flash write fails.** Known to be the transfer length, not the
   address, and the threshold moves with the image size. `tools/rift-flash.py`
   handles it; the cause is still unknown.

6. **Emoji show as blocks past the mapped set.** CP437 has none, and what remains
   has no honest ASCII equivalent. The route is hand-drawn glyphs in a 6×8 cell,
   which makes the discard list as much the deliverable as the glyphs: an
   unrecognisable glyph is worse than a block, because a block admits it cannot
   draw the thing.

## 5. How this has worked

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
