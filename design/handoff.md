# Handoff: RIFT on-device UI

## Overview

A redesign of the five on-device screens in RIFT — the MeshCore companion firmware for the
original LilyGO T-Deck (ESP32-S3, 320x240 ST7789, physical QWERTY, trackball, GT911 touch).

Two problems drove the work, both reported by the maintainer and both confirmed in the source:

1. **NODES** placed nodes with `branch & 15` — 16 fixed directions, so two nodes reached through
   the same repeater always landed on the same point. Its links to centre were
   `fillRect(cx, cy, 1, 1)` (a single pixel at the midpoint, not a line), and its hop rings were
   `drawRect` — distance drawn as squares.
2. **RADAR** derived each blip's angle from the array index (`i & 15`), and the array is re-sorted
   by signal strength on every render, so blips jumped between sweeps with nothing having moved.
   Neither screen labelled its axes.

The redesign fixes both, and applies one consistent visual language across all five screens.
It also adds a day mode, because grey-as-data does not survive sunlight (see *Colour*).

## About the design files

`RIFT skjermdesign.dc.html` is a **design reference written in HTML**, not code to port. Every
on-device screen in it is drawn at 1:1 320x240 and shown at 2x scale, using only constructs the
firmware can actually draw. The implementation target is C++ against the existing
`DisplayDriver` / `ST7789NativeDisplay` API in this repo — **not** a web framework.

Read the HTML as a pixel spec: every coordinate in it is a real device coordinate. This README
carries the same numbers so you do not have to measure them.

## Fidelity

**High fidelity, hardware-checked.** Rounds 3, 4 and 5 are drawn on the real 6x8 glyph grid with
the real chrome geometry, and are the canonical spec. Rounds 1 and 2 are earlier concept
explorations kept for context — their status bar, nav bar and font sizes are wrong (they use 6px
and 7px text, which does not exist on the device). **Implement rounds 3-5. Ignore 1-2.**

Section ids in the file: `3a` NODES + RADAR, `4a` MESH + SYSTEM, `5a` day mode, `5b` night mode,
`2a` wordmark, `2b` splash + COMMS, `2c` waterfall.

---

## Hard constraints

These are not preferences. They are what the panel and the driver permit.

### Text

Adafruit GFX has one built-in bitmap font in a **6x8 cell**, and `setTextSize(n)` multiplies it by
whole integers only. The only available sizes are:

| setTextSize | Cell | Chars per 320px row |
|---|---|---|
| 1 | 6x8 | 53 |
| 2 | 12x16 | 26 |
| 3 | 18x24 | 17 |

Nothing exists between them. There is no 7px text, no antialiasing, and no sub-pixel positioning.
Row pitch is `RIFT_LINE_H` = **12px** (8px glyph + 4px leading) — keep using it.

Every screen in the spec uses size 1 throughout, with **at most one** size-3 value per screen for
the single number or word that screen exists to answer. Never two.

Minus signs are ASCII `-`. The glyph table has no U+2212.

### Colour

`ColorVal` is `uint16_t`, so any RGB565 value is settable and a grey ramp is technically possible.
It is still the wrong encoding channel outdoors: reflected ambient light lays a uniform veil over
the panel, black lifts toward grey, and the dark steps collapse into each other. Consequences,
applied throughout the spec:

- **Never encode data in brightness where shape can carry it.** Freshness in NODES is a *filled*
  vs *hollow* rect, both at full contrast — not four grey levels.
- **Floor of #6E6E6E on black.** Anything darker is unreadable at 6x8 with no antialiasing.
- **De-emphasis inverts with the field.** On black, dim = lighter grey. On white, dim = *darker*
  grey. A light grey on white is invisible.
- **The accent changes role, not value.** #FF4100 on black is 6.0:1 and may be text. On white it
  is 3.5:1 — below the floor — so on the light field red is a **fill with white text reversed out
  of it**, never text colour. Accent *text* on white becomes #202020.
- Where brightness genuinely *is* the quantity (the waterfall), keep the ramp in the upper half:
  black = silent, then four steps from mid-grey to white. Four steps, not twelve.

### Draw budget

The SPI bus is shared with the SX1262. Frames are composed into a `GFXcanvas16` in PSRAM
(150KB for full screen) and blitted once. NODES redraws every 2000ms, RADAR every 700ms.

The redesign is *cheaper* than what it replaces: NODES drops all inter-node connector lines and
draws the route for the selected node only — 4 line segments instead of ~24.

`drawCircle` is not available in the driver. MESH's radar stays three nested `drawRect`.

### Data ceilings

`RIFT_CONST_MAX` = 16 nodes. Message log holds 48. ~49% of internal static RAM is already in use;
the BLE stack is never released after the first RADAR visit. Do not add per-node allocations.

---

## Palettes

Both modes are the same geometry with a swapped colour table. RGB565 values given for direct use.

| Role | Night | RGB565 | Day | RGB565 |
|---|---|---|---|---|
| `bg` background | #000000 | 0x0000 | #FFFFFF | 0xFFFF |
| `bar` title bar fill | #1A1A1A | 0x18C3 | #E8E8E8 | 0xEF5D |
| `fg` primary text | #FFFFFF | 0xFFFF | #000000 | 0x0000 |
| `mid` labels, secondary | #9A9A9A | 0x9CD3 | #5A5A5A | 0x5ACB |
| `dim` inactive nav | #8A8A8A | 0x8C51 | #6A6A6A | 0x6B4D |
| `rule` hairlines | #707070 | 0x738E | #8A8A8A | 0x8C51 |
| accent | #FF4100 | 0xFA00 | #FF4100 | 0xFA00 |
| accent text on field | #FF4100 | 0xFA00 | #202020 | 0x2104 |
| status OK | #39C800 | 0x3E40 | #39C800 | 0x3E40 |

Note: in night mode `mid` and `dim` are nearly equal by design — inactive nav tabs are
distinguished by the active tab's full-contrast label plus its red underline, not by a grey step.

**Recommendation:** bind mode to the backlight level you already set, not to a menu item users
forget to switch.

#FF4100 (Rød) is JPC's sanctioned accent. Do not introduce a second accent or a darkened red.

---

## Shared chrome

Traced from `renderTitleBar` / `renderNavBar` in `examples/companion_radio/ui-rift/UITask.cpp`.

### Title bar — y 0..15 (16px)

- Filled band in `bar`, full width.
- `"RIFT"` at **x=2, y=4** in accent. In day mode: fill `rect(0, 0, 30, 16)` in #FF4100 first,
  then draw `"RIFT"` in white on top.
- Per-screen subtitle at **x=40, y=4** in `mid`: `CONNECTED` / `9 HEARD` / `LIVE` / `SYSTEM`.
- Battery percentage right-aligned at **x=318, y=4** in `fg`. Nothing else goes on the right —
  frequency and heap belong in the body.

### Nav bar — y 226..239 (14px band + 1px top rule)

- 1px `rule` line at y=226. Band below in `bg`.
- Five 64px columns at x = 0 / 64 / 128 / 192 / 256.
- Labels at **y=228**, centred in their column:
  `MESH` x=20 · `NODES` x=81 · `RADAR` x=145 · `COMMS` x=209 · `SYSTEM` x=270.
- Active label in `fg`; inactive in `dim`.
- **Added by this design:** a 64x2 accent underline at the active column's x, y=226. The current
  code distinguishes tabs by colour alone; the underline gives a second cue that survives the
  sunlight veil.
- **Added by this design:** a 3x3 accent dot at x=246, y=227 when unread messages exist. Unread
  count is not surfaced in the nav today.
- The label is `SYSTEM`, not `SYS`.

Body area is therefore **y 16..225**, 210px tall — 17 rows at 12px pitch.

---

## Screens

### NODES — reachability by hop count

Replaces `RiftConstellationScreen`. Answers one question: who can I reach, and how far away.

**Layout.** Four hop columns at a **68px pitch**, x = 46 / 114 / 182 / 250.

- Column headers at y=20 in `mid`: `DIRECT` `1 HOP` `2 HOPS` `3 HOPS`.
- 1px `rule` separator at y=31, full width.
- `YOU`: 5x5 accent rect at (10, 90), label at (4, 98) in `mid`.
- Node entries per column: 5x5 marker, then its name on the row below (marker y, name y+8).
  Rows used: y = 36 / 64 / 92 (marker) with names at 44 / 72 / 100.
- **Freshness is shape:** `fillRect(x, y, 5, 5)` = heard under 30 min. `drawRect(x, y, 5, 5)`
  = older. Both in `fg`. No grey levels.
- Legend at (2, 132) and (2, 144) in `mid`: `solid = heard under 30m` / `hollow = older`.

**Names cut at 10 characters.** A column is 68px; 10 chars = 60px, leaving a full glyph-width gap
to the next column. At 11 chars the gap is 2px and two names read as one word. MeshCore permits 61
characters, so truncation is mandatory — put the rule on screen rather than letting it surprise.

**Selected node.** Marker is a 7x7 `drawRect` in accent (one pixel larger, so it reads as selected
at both freshness states). Its route is drawn as real `drawLine` segments in accent from YOU
through the actual repeaters — 4 segments max. Example path for a 2-hop node:
`(12,92) -> (40,92) -> (40,38) -> (182,38)`.

**Detail bar** — separator at y=176 (accent in night, #202020 in day), then:
- Name at (2, 182) in `fg`; hop count right-aligned at y=182 in `mid`.
- `via <REPEATER>, heard 26m ago` at (2, 194) in `mid`.
- `ENTER: DM` right-aligned at y=194. Night: accent text. Day: `rect(262, 193, 58, 10)` in
  accent with white text at x=318 right-aligned.

Drop all grey inter-node connector lines. The columns already answer "how many hops"; the only
line worth drawing is the selected route.

### RADAR — presence

Replaces `RiftRadarScreen`'s scatter. Answers: is there anything around me, and is that changing.

**Headline.** `35` at (2, 22) in **setTextSize(3)** — the one large value on the screen.
`DEVICES NEARBY` at (44, 24) in `mid`, `24 wifi  11 ble` at (44, 36) in `fg`.
`+4 new` right-aligned at y=24 (accent; day = fill `rect(280,23,40,10)` + white text),
`in 60s` right-aligned at y=36 in `mid`.

**Distance bands** — 1px `rule` at y=54, then three rows at y = 62 / 78 / 94. Each row:
label at x=2 in `mid`, dBm range at x=46 in `mid`, cells from x=116.
**One 6x8 cell per observed device**, 8px pitch:

| Band | Range | Cell |
|---|---|---|
| `CLOSE` | -30..-60 | `fillRect` in `fg` |
| `MID` | -60..-80 | `fillRect` in `fg` |
| `FAR` | -80..-100 | `drawRect` in `fg` (hollow — form, not a dimmer grey) |

Cells available per row: (320-116-2)/8 = 25. Clamp and show the count in the headline.

**List** — 1px `rule` at y=112, then rows at 12px pitch from y=120: dBm at x=2, name at x=32,
type/channel right-aligned. Strongest row: dBm and name in `fg`, tag in accent (night) or
#202020 (day). Six rows fit above the footer.

**Footer** — `rule` at y=196, `ENTER: waterfall` at (2, 206) and `nothing transmitted`
right-aligned at y=206, both `mid`. That second string is a claim the user is entitled to see on
screen, not only in the README.

**Placement, if you keep a spatial view anywhere:** derive angle from a stable identifier
(BSSID / MAC for RADAR, `pubkey_prefix` for nodes) with a small deterministic offset on collision.
Never from the array index — the array is re-sorted every render.

### MESH — am I on the network

Replaces `RiftMeshScreen`'s layout; all values are the screen's existing ones.

- `LINK` label at (2, 18) in `mid`.
- Link state at (2, 30) in **setTextSize(3)**: `CONNECTED` / `PAIRING` / `STANDBY`. All are
  <= 9 chars = 162px, so all three fit. This moves the state out of the title bar subtitle and
  into the body — it is the question the screen exists for.
- Radar: three nested `drawRect` centred (210, 120) — 100x80, 66x54, 32x26 — in `rule`/`mid`,
  with one 4x4 accent blip at one of the eight existing dx/dy offsets. Unchanged mechanism.
- `NODES 14` at (2, 170) in `fg`; `LINK -87 / 8` right-aligned at y=170 in `fg`.
- `868.000MHz  SF10  22dBm` centred at y=182 in `mid`.
- `roll trackball L/R to change screen` centred at y=202 in `dim`.

### SYSTEM — actions and diagnostics, in two columns

Replaces `RiftSystemScreen`'s single mixed list.

**Divider at x=160**, y 16..225, 1px in `rule`. Left = actions, right = read-only diagnostics.
The divider must be at 160, not 128: the longest menu item is `Send advert (neighbours)` at
24 chars = 144px.

**Left column.** `ACTIONS` at (2, 18) in `mid`. Five items at 12px pitch from y=32; selected row
is `fillRect(0, 30, 158, 12)` in accent with white text at x=4. All five from the current code:
`Send advert (neighbours)` · `Send advert (whole mesh)` · `Edit node name` · `Add channel` ·
`Path hash size: 1 byte`.

Below a `rule` at y=96, a five-line note in `mid` from y=104 explaining that neighbours reaches
direct RF only while whole mesh floods further, and to use the latter before a first DM to a
distant node. This is the one thing a user must understand before messaging a stranger — it
belongs next to the action, not in the README.

**Right column.** `DIAGNOSTICS` at (166, 18) in `mid`, then label/value rows at 12px pitch from
y=34: labels at x=166 in `mid`, values right-aligned in `fg`. 25 chars fit per row.
`NODE` `KEYBOARD` `LAST KEY` `TOUCH` `I2C BUS` `GPS` `FREE HEAP` `EXT POWER` `LAST RESET`.

Status colour only: keyboard OK in green, `GPS not found` in accent — red because GPS varies by
unit and people ask about it, not because anything is wrong.

**Footer.** `rule` at y=178. `PRIVATE KEY EXPORT` / `DISABLED` at y=184.
`up/down select, ENTER activates` at (2, 196) in `dim`.

### Splash — first boot

First boot formats SPIFFS and takes 1-2 minutes. The current screen shows `Loading...`, which
reads as a hang and costs the README a paragraph of explanation.

- Wordmark at (34, 76), `setTextSize(3)`.
- `RADIO INTELLIGENCE` / `& FIELD TERMINAL` at (34, 118) and (34, 130) in `mid`.
- Progress bar: 3px track at x 34..286, y=176 in `rule`; fill in accent.
- `Formatting SPIFFS` at (34, 186) in `mid`; `1-2 min - not a hang` right-aligned at y=186.
- `RIFT 0.1.0 - MeshCore v1.8.2` at (34, 210) in `dim`.

### COMMS

- Channel tabs in the title bar: active tab is an accent fill with white text; inactive are
  `drawRect` outlines in `rule` with `mid` text.
- Messages at 12px pitch. Sender line: time at x=2 in `dim`, name at x=32 in `mid`.
  Body on the next row in `fg`.
- **Own messages get a 2px accent bar down the left edge**, not right alignment — you keep full
  width on a 320px screen and it is one `fillRect` to draw.
- Delivery status sits on the sender line, right-aligned: `ACK 1.2s` in green, `NO ACK` in
  accent, nothing at all on channel messages where no truth exists to show.
- Compose bar: `rule` above, `>` prompt in accent, text in `fg`, char count right-aligned in
  `dim`. MeshCore truncates at 160.

### RF Waterfall

- 30 sweeps, newest at top, one row per sweep filling the plot area.
- 13 channel columns.
- Time axis labelled on the left in `mid`: `now` / `-1m` / `-2m`. Channel axis below:
  `CH1 4 7 10 13`.
- Ramp: black for silent, then four steps mid-grey to white. The single strongest cell in the
  frame is accent. The current four-colour red/blue/white/grey ramp is not ordered and cannot be
  read as a scale.
- Footer reads the answer out directly: `busiest CH6 - quietest CH8`. That is why the screen is
  opened.

---

## Wordmark

RIFT has no wordmark today — `logo/` in the repo holds upstream MeshCore artwork only, and splash
draws `"RIFT"` with `setTextSize(3)`. Section `2a` proposes one: the letters split along a
horizontal offset with a 2-3px accent line through the seam, so the name is the form.

It is drawn in HTML with `clip-path` and is **not** a device asset. To use it on hardware it must
be hand-pixelled as a 1-bit bitmap and drawn with `drawBitmap`. Two sizes are needed: a title-bar
mark at 16x16 and a splash mark. Until that exists, keep `setTextSize(3)` text on splash.

---

## What this design adds beyond the current source

Flagged so you can accept or reject each one deliberately:

1. Active-tab underline in the nav bar (code distinguishes by colour alone).
2. Unread-message dot on the COMMS tab (unread is not surfaced in the nav today).
3. Link state moved from the title bar subtitle into the MESH body at size 3.
4. Day mode / colour-table swap.
5. Splash progress bar and duration text.
6. The advert explanation note under the SYSTEM menu.
7. On-screen legends for the freshness encoding and the 10-char name cut.

## Files

- `screens/` — PNG of each canonical screen at 640x480 (the 320x240 device frame at 2x).
  `nodes.png` `radar.png` (section 3a) · `mesh.png` `system.png` (4a) ·
  `nodes-day.png` `radar-day.png` (5a) · `nodes-night.png` `radar-night.png` (5b).
  Reference images only — measure from this README, not from the pixels.
- `RIFT skjermdesign.dc.html` — the full design document. Open in a browser.
- `support.js` — runtime needed for the HTML to render. Do not port.
- `github.md` — repo/branch association and the screen-to-source map.

## Source references

All in `KrakenSaten/RIFT`, branch `rift-tdeck`:

- `examples/companion_radio/ui-rift/UITask.cpp` — `RiftSplashScreen`, `RiftMeshScreen`,
  `RiftConstellationScreen`, `RiftRadarScreen`, `RiftCommsScreen`, `RiftSystemScreen`,
  `renderTitleBar`, `renderNavBar`
- `src/helpers/ui/ST7789NativeDisplay.h`, `src/helpers/ui/DisplayDriver.h` — `ColorVal`,
  available primitives
