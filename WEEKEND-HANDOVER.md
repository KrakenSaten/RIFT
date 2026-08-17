# Weekend handover — 0.4.0, and where the next session starts

Written to move between machines. **This is a session log, not a reference.**
[`HANDOFF.md`](HANDOFF.md) is the durable working notes and stays authoritative;
if the two ever disagree, believe that one. Delete this file once the open thread
below is closed, so it cannot rot into a second source of truth the way the README
had begun to.

---

## Start here

```bash
git clone https://github.com/KrakenSaten/RIFT.git
```

```bash
git checkout rift-nordic-input
```

| Branch | State |
|---|---|
| `rift-tdeck` | `bd75511d`, tagged **v0.4.0**, released and published |
| `rift-nordic-input` | 3 commits ahead, pushed, **not merged** — deliberately held back |

Nothing on the feature branch is released. The published flasher serves 0.4.0.

Then set the environment up per [`BUILDING.md`](BUILDING.md). On a fresh machine
the three traps in there are the whole job; the rest is `pio`.

**Two local helper scripts are not in the repo** and will need recreating. They
live at `C:\dev\rift-env.ps1` (sets `PLATFORMIO_CORE_DIR`, puts the venv and
`C:\msys64\ucrt64\bin` on PATH) and `C:\dev\rift-flash.ps1` (the split write).
`BUILDING.md` documents what they do, so they are convenience rather than
knowledge — but the split write is not optional, see below.

---

## The one open thread

**Message history persistence works, but the write blocks for 553ms.**

That is the immediate next thing, and it needs a reading before it needs code.

There is no watchdog on the main loop and a blocking call silently starves the LoRa
radio — `HANDOFF.md` calls this the most important constraint in the codebase. Half
a second every time a conversation settles is not acceptable.

553ms with **four** messages in the log, so a few hundred bytes. Far too little
data for the data to be the cause; the file operations are the suspect, and SPIFFS
is known to be slow at creating and deleting files.

**Next action:** SYSTEM shows `MSGLOG` (count and total) and `phases`
(open / write / close / remove+rename). Send a message, wait 20 seconds for the
debounced write, read the four numbers.

- Time in **open** and **swap** → metadata operations. Fix by opening the log once
  at boot, keeping the handle, and rewriting in place with `seek(0)`. That drops
  temp-and-rename, which only ever protected the log file from a torn write — the
  log is not precious, a corrupt one is discarded on load. Cheap thing to trade for
  half a second of radio time.
- Time in **write** → it really is the payload, and the write has to be split
  across loop iterations, or made rarer and smaller.

---

## What the weekend produced

Nine commits, all verified on hardware before being committed. Released as 0.4.0
partway through; three commits sit unreleased on the feature branch.

### The home screen answers a different question

It headlined `hasConnection()` — the USB/BLE companion link — which is honest about
what it measures but not the useful thing. A standalone node with a busy mesh around
it read `STANDBY` forever, and standalone is the case RIFT exists for.

It now reads `ACTIVE` / `IDLE` / `QUIET` / `NO SIGNAL` from how long ago the radio
last decoded anything, with the age and packet count beneath it so the verdict can
be checked against the numbers it came from. `logRxRaw()` is the hook, which
`Dispatcher` calls for every raw reception before parsing — so it counts traffic for
other nodes and traffic that does not decrypt, deliberately: the question is whether
this radio is somewhere with a live network.

`meshcore.io` sits above it, because LoRa handhelds are dominated by Meshtastic and
a T-Deck looks like one.

### Screens gained a lifecycle

`UIScreen` had render/handleInput/handleTouch/poll and no notion of entering,
leaving, modality or overlays, so `setCurrScreen()` answered "is this real
navigation?" twice with its own loop and `newMsg()` answered it a third way, each
reaching into concrete screen classes.

Both bugs `HANDOFF.md` had recorded were the same mistake at two of those sites: a
popup treated as navigation. Once it tore the BT controller down mid-scan and
panicked the device; once it wiped a one-time channel key while it was being read.

`RiftScreen` adds `onEnter`/`onLeave`/`isModal`/`isOverlay` in `ui-rift` rather than
in the shared header, to keep the diff against upstream small. The rule lives in
`riftScreenTransition()` where it is tested. An overlay never becomes `curr` and
never touches `nav_idx`, so dismissing restores nothing and the screen underneath is
never told it was left — that removes the need for the guard rather than
centralising it.

Overlays render after the current screen into the same canvas, which is the
mechanism `showAlert()` already used.

### Messages, popups and alerts

The popup showed one message with ENTER to step through, so clearing N cost N
presses. It lists the six newest now, in the same row idiom COMMS uses; ENTER opens
COMMS for the full history, backspace dismisses and hands back the screen you were
on rather than dropping you home.

The alert box joined the same panel language — `bg` fill, accent border, sized to
its text. It had been filling with `bar`, the title-bar band, which sits two percent
from its own background in both modes and read as a stray outline.

### Emoji are readable

Invisible code points — variation selectors, ZWJ, skin tones — were each drawn as a
block, so a heart with U+FE0F was two squares and a ZWJ family was five. Those are
dropped, consecutive undrawable ones collapse to one, and anything with a genuine
equivalent gets it.

The table is conservative on purpose: mapping the whole U+1F600 block to `:)` would
put a smile where someone sent a sobbing face, which is a claim the device cannot
support. There is a test for it. **Still too many blocks for current traffic** —
that is task #10, and the limit is now fundamental rather than a gap in the table.

### The chrome moved to one edge

The 16px title bar is gone. The wordmark moved into the nav bar — the first tab
reads `RIFT` and takes the accent while active — and the battery moved to the nav
bar's unused right-hand 32px. Per-screen context became a heading in each screen's
own body; three of the fourteen call sites only repeated the nav bar and draw
nothing now.

COMMS was the only screen genuinely short of room and is the one that varies: with a
channel target there is no heading, so the strip sits at the top and the history
takes the reclaimed 16px, which is one more message. With a contact target the
heading is the only place the target appears, so it stays.

### Nordic characters can be typed

Double-tap a base vowel: `a` offers æ å ä, `o` offers ø ö, uppercase follows. Both
taps are inserted as ordinary letters and the picker replaces the pair, so
cancelling leaves what was typed and a false trigger on Haakon or Aage costs one
keypress.

The compose line is UTF-8 aware. It renders by translating first and taking the tail
of the *translated* text, so a cut cannot land inside a sequence; backspace removes
a whole code point; the send path truncates on a code point boundary. All three use
`mesh::validUtf8PrefixLength`, which upstream already has and tests. Two were live
bugs.

Confirmed end to end: æøå ÆØÅ arrive intact at another client, which is the only
check that can tell UTF-8 from CP437 — the panel would look identical either way.

### Documentation

README went from 580 lines to 310, split into `BUILDING.md` and `ARCHITECTURE.md`.
Two of its four jobs had duplicated `HANDOFF.md` nearly word for word and had
already drifted twice. It had also started describing a UI that no longer existed.

`design/handoff.md` was two commits stale and is updated, with the superseded
sections marked rather than rewritten — the renderings in `design/screens/` still
match what they were drawn for, which is why the README now caveats them.

---

## Findings that cost real time

These are all in `HANDOFF.md` now. Repeated here because each one was a day's lesson.

**`SYM` is not a spare key.** It emits nothing alone, but `SYM`+letter emits the
symbol printed on the key as plain ASCII well under 127 — `SYM`+a is 42 `*`, +o is
43 `+`, +e is 50 `2`. It is a working symbol layer, and a picker on it would have
cost `* + / : ; ' " @ #` and the digits.

**There is no key repeat and no key-up, so a long press cannot be detected.** A
whole trigger design was built on a comment in `TDeckKeyboard::poll()` claiming the
opposite, before anyone put the number on screen. Holding `a` types one `a`, however
long you hold. The comment now says what was measured, and the header explicitly
documents the absence of a `heldKey()` so the next person meets the answer instead
of rediscovering it.

**Two byte values cannot be drawn.** `Adafruit_GFX::write()` special-cases `0x0A`
and `0x0D` before `drawChar` sees them, so a glyph mapped to either renders as
nothing. `0x0D` is CP437's eighth note, and music mapped there vanished. Also
`0x01`/`0x02` are spent on the synthesised ø/Ø, which are CP437's two smileys — so
an incoming smiley cannot use them. `riftNoGlyphAt()` states the rule and a test
walks the whole table.

**A single flash write over ~1.2MB fails.** Reproducibly, with the USB device
dropping off the bus. Ruled out: a different cable, `upload_speed` (baud is a no-op
on native USB CDC), and `--no-stub`. Splitting `firmware.bin` at `0xC0000` and
writing the halves to `0x10000` and `0xD0000` completes in under five seconds each
with hashes verified. Cause never established; the first upload of a session has
gone through in one piece, which argues for something state-dependent. **Expect to
need this every time** — the firmware is 1.6MB and growing.

**GitHub rejected the first push.** GH007: the commits carried `andre@jpc.no`,
auto-derived because git identity was never configured, and the account keeps email
private. Nothing was published — the push was refused whole. Identity is now set
globally to `270574703+KrakenSaten@users.noreply.github.com` and the seven commits
were rewritten before pushing.

---

## The queue

Eight done, nine open. Nothing here is started.

| # | Item | Note |
|---|---|---|
| 10 | Hand-drawn glyphs for the emoji still showing as blocks | Limit is 6x8 pixels; the discard list is as much the deliverable as the glyphs |
| 11 | meshcore.io in MeshCore's brand blue | Needs the real hex; site returns 403 to automated fetches |
| 12 | Remove `LIVE` from the RADAR heading | Consider whether `IDLE`/`INITIALISING` stay, since they say something `LIVE` does not |
| 13 | Move the `HEARD` count out of the NODES heading | **Ask where it should go** — the request did not say |
| 14 | Redesign NODES: most nodes land in `3+ HOPS` | Get the real hop distribution first; guessing the buckets is what produced this |
| 15 | Only offer DM to nodes that can receive one | Repeaters cannot; the NODES entry point bypasses the picker's filter |
| 16 | Trackball scrolling should stop at the ends, not change screen | Find out first whether a fast roll emits LEFT/RIGHT, or a screen returns false at its limit |
| 17 | Colour-code channels and own messages in COMMS | The accent is spoken for; count of usable colours falls out of the contrast check |

Also outstanding and not in the queue:

- **Report the I²C bug upstream.** Measurements are in commit `3528d80a`. It costs
  every T-Deck user four minutes of boot, not just us — the largest effect available
  outside this repo. Outward-facing, so it needs an explicit go-ahead.
- **`QUIET` has never been seen on hardware.** Needs a quarter hour of silence.
- **The design renderings show the removed chrome.** Redraw or keep the caveat.

---

## Verifying a change

```bash
pio test -e native -e native_kiss_modem
```

95 cases. Then all five environments, because `MyMesh` and `AbstractUITask` compile
into every companion-radio build:

```bash
pio run -e LilyGo_TDeck_rift -e LilyGo_TDeck_companion_radio_usb -e LilyGo_TDeck_companion_radio_ble -e LilyGo_TDeck_repeater -e LilyGo_TDeck_kiss_modem
```

Then flash and look at it. Nothing this weekend was accepted on the strength of
compiling — and the two times a code comment was trusted instead of a measurement,
it cost a day each.
