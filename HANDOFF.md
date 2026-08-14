# RIFT — working handoff

Written to be loaded into a fresh Claude Code session. The README is the public
front page; this is the working notes. Everything here was learned by doing, and
several items cost hours the first time.

**Repo:** https://github.com/KrakenSaten/RIFT — default branch `rift-tdeck`
**Current:** v0.3.1, HEAD `175ccf7e`
**Hardware:** original LilyGO T-Deck (ESP32-S3, 320×240 ST7789, QWERTY, trackball,
GT911 touch, SX1262 LoRa)

RIFT is a fork of [MeshCore](https://github.com/meshcore-dev/MeshCore) by Scott
Powell and contributors (MIT). The mesh stack, LoRa drivers and protocol are
theirs. RIFT adds a keyboard-driven on-device UI so the T-Deck works standalone,
with no phone app.

---

## 1. Environment — three things that will waste your time otherwise

**PlatformIO in its own venv.** Installing it into a shared Python upgraded
another project's pinned dependencies once already.

```bash
python -m venv C:/dev/RIFT/.venv
C:/dev/RIFT/.venv/Scripts/python.exe -m pip install platformio
```

**`PLATFORMIO_CORE_DIR` must be ASCII-only.** A profile path containing `é`
makes the toolchain fail to find `Arduino.h` / `Stream.h` — a completely
misleading error.

```bash
export PLATFORMIO_CORE_DIR="C:/dev/.platformio"
```

**Unit tests need a host compiler.** PlatformIO does not bring one; without it
`pio test -e native` fails with `'g++' is not recognized` and the suite can only
run in CI. That is how a missing `main()` once reached the branch. On Windows,
MSYS2: install from msys2.org, then in the UCRT64 shell

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc
```

and put `C:\msys64\ucrt64\bin` on PATH. Verified with GCC 16.1.0.

### Commands

```bash
pio test -e native -e native_kiss_modem
```

```bash
pio run -e LilyGo_TDeck_rift
```

```bash
pio run -e LilyGo_TDeck_rift -t upload --upload-port COM5
```

Always build all five T-Deck environments before committing — RIFT edits shared
code, so `_companion_radio_usb`, `_companion_radio_ble`, `_repeater` and
`_kiss_modem` are the regression check:

```bash
pio run -e LilyGo_TDeck_rift -e LilyGo_TDeck_companion_radio_usb -e LilyGo_TDeck_companion_radio_ble -e LilyGo_TDeck_repeater -e LilyGo_TDeck_kiss_modem
```

**Upload says "Could not open COM5, the port doesn't exist" while the port is
clearly listed?** Something holds it. Usually a serial monitor — or the browser
tab with the web flasher, since WebSerial keeps the port until the tab closes.

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
- **Keyboard** is I²C 0x55, polled. The co-processor repeats held keys, so edge
  detection is required or one Enter fires several times.
- **I²C after the RTC probe is broken** until the bus is restarted. A transaction
  to an absent address took ~920 ms, and two callers walk all 112 addresses —
  that was 243 seconds of boot. `radio_init()` now does `Wire.end()` then
  `Wire.begin(18, 8, 100000)`. Why the probe leaves it that way is still not
  established; this treats the symptom.
- **External power** is `HWCDC::isPlugged()`. `(bool) Serial` resolves to
  `isCDC_Connected()`, which needs the host to *open* the port and returns false
  by design.
- **CP437 font** has æÆåÅäÄöÖ but no ø/Ø — those are drawn manually as the base
  letter plus a stroke.
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

`design/handoff.md` is the specification, with 1:1 device coordinates. `design/screens/`
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
rendering; browser flasher; boot in 5.1 s.

**CI** (`.github/workflows/rift-build-check.yml`, `rift-release.yml`): tests gate
the builds, all five environments build, third-party actions pinned to SHAs,
read-only by default with write only on the publish job, and a release is refused
if the tag and `RIFT_VERSION` disagree.

**Warnings are on for RIFT's own code** (`-Wall -Wextra`, `-w` unflagged) and it
compiles clean. `-Werror` is off while ~210 warnings in shared MeshCore code are
not ours to fix.

**60 native tests**, all runnable locally. They cover the three places that have
actually been wrong: base64 key validation, `path_len` decoding, and hash
collision resolution.

### Open items

1. **MESH headline should show real mesh activity.** It currently says
   `COMPANION` and reports the USB/BLE link, which is honest but not the useful
   thing. Needs a last-received timestamp nothing tracks yet.
2. **`UIScreen` lifecycle / overlay refactor.** `onEnter`/`onLeave`/`isModal` as
   real concepts, with the message preview as an overlay rather than a screen
   replacement. Two bugs have already come from its absence. The reviews
   recommend doing it gradually, not as a rewrite.
3. **Message history does not survive reboot.** RAM-only by design; persisting it
   means new code and flash wear.
4. **Nordic character input.** Reading works; the keyboard has no Nordic keys and
   no alt/sym combination produces distinct codes, so entering them needs a
   software compose scheme.
5. **Report the I²C bus issue upstream.** It costs every T-Deck user four minutes
   of boot. Measurements are in commit `3528d80a`.
6. **Bundle ESP Web Tools** instead of loading it from unpkg. Pinning the version
   is only partial — the `?module` form resolves its dependencies from the CDN at
   load time. Real self-hosting needs an npm build step.
7. **`.github/actions/setup-build-environment`** is upstream's and still refers to
   two actions by tag. Pinning there would change their workflows too.

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
