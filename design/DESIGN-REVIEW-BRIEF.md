# RIFT — design review brief

Self-contained. Everything a reviewer needs is in this file; no repository access
required. Screenshots are supplied separately by the author and are the ground truth
for appearance — where this document and a screenshot disagree, the screenshot is
right and the mismatch is worth reporting.

**What RIFT is, in one line:** a custom firmware UI for the LilyGO T-Deck, a
handheld LoRa mesh radio with a physical keyboard, a trackball and a 320×240 screen.
It is a field terminal, not a phone app. The user is outdoors, possibly in sunlight,
possibly wearing gloves, and wants to know whether the radio is working.

**What this review is for:** the visual and layout design only. Protocol, firmware
and build concerns are deliberately out of scope. Section 7 lists the specific
questions worth an opinion; sections 1–3 are the constraints that make an opinion
actionable; section 8 lists what has already been decided against, so a review does
not spend itself re-proposing it.

---

## 1. The panel decides more than taste

Every constraint below is physical. None is a preference, and none can be negotiated
by preferring something else.

| Constraint | Value | What it forbids |
|---|---|---|
| Resolution | 320 × 240, landscape | No second column of prose. A full-width list holds ~16 rows |
| Colour depth | RGB565 (5-6-5 bits) | Colours must be chosen **after** quantisation, not before |
| Font | 6 × 8 CP437 bitmap, integer scaling only | No weights, no italics, no letter-spacing, no kerning. `setTextSize(2)` is exactly double |
| Row pitch | 12px | 8px of glyph, 4px of air. Anything tighter is unreadable at arm's length |
| Antialiasing | None | A diagonal is a staircase. A 1px detail either lands on a pixel or does not exist |
| Clipping | **Not available in the driver** | Two overlapping draws cannot be masked against each other. Whatever is drawn second wins, entirely |
| Primitives | `fillRect`, `drawRect`, `drawXbm`, text | No line, no circle, no arc, no gradient, no alpha |
| Diagonals | Composed from `fillRect` runs | Possible and in use, but every diagonal is hand-stepped |
| Bitmaps | `drawXbm`: 1-bit mask, single colour, per-pixel | Works. A 96×28 mark is 2688 pixel writes — fine on a screen drawn once, too slow for one that redraws on a timer |
| Redraw budget | Shares the SPI bus with the LoRa radio | A screen that redraws every 700ms must be cheap. Blocking the bus starves the radio, and there is no watchdog to catch it |

**Sunlight is the design environment.** This is the constraint that has changed the
most decisions. Under reflected daylight the difference between two greys disappears
entirely, while the difference between a filled square and a hollow one does not.
Every place RIFT could have encoded a value in brightness, it encodes it in form
instead.

---

## 2. Palette — exact values, both modes

Two modes, same geometry, swapped colour table. A user toggles between them; nothing
moves when they do.

### Night (default)

| Role | Hex | Use |
|---|---|---|
| `bg` | `#000000` | Field |
| `bar` | `#1A1A1A` | Chrome band |
| `fg` | `#FFFFFF` | Primary text |
| `mid` | `#9A9A9A` | Secondary text, chrome, labels |
| `dim` | `#8A8A8A` | De-emphasised text |
| `rule` | `#707070` | Hairlines and borders |
| `accent` | `#FF4100` | Brand, active state, alert |
| `ok` | `#39C800` | Success, delivery confirmed |

### Day

| Role | Hex | Note |
|---|---|---|
| `bg` | `#FFFFFF` | |
| `bar` | `#EFEBEF` | |
| `fg` | `#000000` | |
| `mid` | `#5A5A5A` | |
| `dim` | `#6B6B6B` | **Darker than `mid`, not lighter** |
| `rule` | `#8C8C8C` | |
| `accent` | `#FF4100` | Same value, different role — see below |
| `accent as text` | `#202020` | The accent is unreadable as text on white |
| `ok` | `#428610` | Not the night green |

### The three rules the palette encodes

**Nothing darker than `#6E6E6E` on black survives a 6×8 glyph with no
antialiasing.** The floor is empirical, not calculated. It is why there is no fourth
grey.

**De-emphasis inverts with the field.** On black, dim means darker. On white, dim
must also mean darker — a lighter grey on white reads as *disabled*, not as
*secondary*. Getting this backwards is the single easiest mistake in a two-mode
palette.

**The accent has one value and two roles.** `#FF4100` measures 6.0:1 against black
and 3.5:1 against white. So on black it may be text. On white it may only be a fill
with white reversed out of it — never text, never a hairline expected to be read.
Two shipped bugs came from ignoring this.

### Channel colours — the constraint worked through

COMMS colour-codes channels. The number of usable colours is not a taste question; it
falls out of contrast arithmetic and the answer is **four**.

A channel's identity colour cannot swap between modes, or it stops being an
identity. So one value must clear 4.5:1 against **both** `#000000` and `#FFFFFF`.
That confines luminance to L 0.175–0.183 — a band, not a point. Of 65 536 RGB565
values, **1 091 clear it.**

Which means: *every channel colour must be equally dark, and they can differ only by
hue.* The instinct of "a light one and a dark one" is arithmetically unavailable.

After keeping 45° of hue from the accent and 35° from the status green, four remain:

| Slot | Hex | Hue | On black | On white |
|---|---|---|---|---|
| 1 | `#73FF00` | 65° | 4.66 | 4.50 |
| 2 | `#00854A` | 153° | 4.51 | 4.66 |
| 3 | `#6361F7` | 241° | 4.58 | 4.58 |
| 4 | `#D62D84` | 329° | 4.55 | 4.61 |

Slot 0 is the shared public channel and deliberately gets **no** colour: it is the
default every node has, and marking it would read as one of the user's own. A fifth
channel also gets none — a repeated colour is worse than an absent one, because it
asserts an identity that is false.

---

## 3. Chrome and grid

**Nav bar — the only fixed chrome.** 14px at the bottom, `y 226…239`.

- Hairline rule at `y = 226`; labels at `y = 228`
- Five tabs: `RIFT · NODES · RADAR · COMMS · SYSTEM`
- Label centres at **x = 20, 81, 145, 209, 270** — not `col × 64 + 32`, because even
  columns would centre SYSTEM at 288 and clip it against the edge
- Active tab: a 2px accent underline **and** a colour change. The underline exists
  because in sunlight the grey step between active and inactive vanishes; colour
  alone was tested and was not enough
- The `RIFT` slot takes the brand colour **only while it is the active tab**. A
  permanent orange chip on one tab would read as selected from every screen and
  destroy the one thing the underline says
- Bottom right: battery percentage in `mid`, switching to accent at ≤15%. On SYSTEM
  the same slot carries the page number instead, because SYSTEM has two pages and
  nowhere else to say which one you are on

**Body — `y 0…226`. There is no title bar.** There used to be a filled 16px band
carrying the wordmark, the battery and the screen name. The first two moved to the
nav bar so all chrome sits on one edge; what remained was a band whose only content
was the screen's own name, which the nav bar already said. Removing it gave COMMS one
extra message of history.

**Headings are per-screen and conditional.** A screen draws one line at `y = 2` in
`mid` *only when it has something to say that its own layout cannot*: which step of a
flow you are in, which contact a message is aimed at, how many nodes were heard.
Screens with nothing to add draw nothing. If a screen needs a heading to explain
itself, the heading is doing work the layout should be doing.

---

## 4. The wordmark

`RIFT` at `setTextSize(3)`, cut along a shallow diagonal with the accent lying in the
cut.

- **Slope:** y 96 at x=0 falling to y 48 at x=319 — about −8.8°. Held as an integer
  rise over run (12/78) so every instance produces an identical staircase
- **The cut:** a 4px gap blanked through the glyphs, with the 2px accent inside it.
  The 1px of air either side is what makes it read as a seam rather than as a line
  struck through the letters
- **Where the cut falls:** roughly 67% down the cap height at the mark's left edge,
  14% at its right. Fixed relative to the glyphs, so the mark is the same wherever it
  is drawn

Two instances, making opposite choices about where the seam ends:

- **Boot screen** (mark at x=32, y=78, in white): seam runs from the left screen edge
  and **stops at the middle**. Edge-to-edge read as a rule with the wordmark sitting
  on it; stopping halfway makes the wordmark the thing the line is part of. It also
  leaves the right half free for the strapline below
- **RIFT tab** (mark at x=240, y=16, in `fg`): seam begins in mid-air at x=196 and
  **leaves the right screen edge.** There is nothing to the mark's right, so stopping
  short would be the arbitrary choice. It does not extend left across the screen
  because that would put a diagonal through the state headline and then through a
  data row

**Known deviation from the original spec, in both instances.** The design called for
two text draws offset 3px so the halves *shear* along the seam. The driver has no
clipping, so whichever half is drawn second spills across the cut, and the blank that
removes the spill also removes the half it was meant to keep — in either order. What
ships is the same cut with the same accent in the same gap, crossing no letter stem,
without the lateral shear. Adding the shear requires a hand-pixelled `drawXbm`
bitmap. **This is a live question — see §7.**

---

## 5. Screen inventory — current state

Each screen answers exactly one question. That framing is load-bearing: it is what
stopped the home screen reporting a USB link nobody cared about.

**RIFT (home) — "is this radio somewhere with a live mesh?"**
Headline at `setTextSize(3)`: `ACTIVE` / `IDLE` / `QUIET` / `NO SIGNAL`, coloured
green / white / grey / accent. `meshcore.io` above it in `mid`, lowercase against the
all-caps convention because a URL in capitals reads worse. The wordmark top right.
Three data rows under the headline — last receive age, packet count, USB/BLE link
state — because every hardware problem on this project was settled by putting the
real number next to the verdict. A small radar animation right of centre: three
nested rectangles and one accent blip at eight discrete positions. Bottom: stored and
heard counts, radio parameters, and one action drawn as an outlined button —
`ENTER: DISCOVER 0-HOP REPEATERS`.

**NODES — "who is out there, and how far?"**
**The weakest screen, and the one most worth reviewing.** A scrollable list from
`y = 68` to `y = 226`, with variable row heights. Hop counts are grouped into
buckets. Unknown values draw as `?` rather than as a guess.

**RADAR — "what Wi-Fi and BLE is around me?"**
A large count, then three signal bands — `CLOSE` / `MID` / `FAR`, each printing its
own dBm range (`-30..-60`, `-60..-80`, `-80..-100`) rather than asking the user to
take the label on trust — rendered as rows of 6×8 cells, one cell per device.
**Filled for close and mid, hollow for far**: a
brightness step would disappear outdoors and a form change does not. A row that
overflows draws an accent cell rather than silently clipping. Below, a list sorted
strongest-first, and a waterfall of channel occupancy over time. Devices can be given
a friendly name and marked to raise a proximity alert.

**COMMS — "conversations."** Recently redesigned; the screenshots are new.
One conversation at a time. A four-tab channel strip along the top, history laid out
bottom-up from the compose line, compose line at the foot. Own messages carry a 2px
accent bar down the left edge rather than being right-aligned — on 320px, right
alignment costs half the width of every outgoing line, and the bar costs two pixels.
Each row is a time, the origin name **in that channel's colour**, and a right-hand
slot holding either delivery state or hop count.

`ENTER` on an empty compose line opens a conversation list: `CHANNELS` and `DIRECT`
section headings, a 2px colour chip for channels, the time of the last message, and
**a 3×3 accent square where there is something unread**. Unread is a shape, not a
colour change, and the count is deliberately not drawn — the question a row answers
is whether to open it, not how far behind you are. The same dot appears on channel
tabs you are not currently looking at.

**SYSTEM — "settings, and what is this device doing?"**
Two pages. Left column actions, right column diagnostic rows, plus a 128-line event
log. **Crowded, and every row earns its place** — this is the screen that found every
hardware bug on the project. The answer is organisation, not deletion.

---

## 6. Rules already established

These were each learned from a shipped mistake. They should survive any redesign.

1. **Compute contrast, do not judge it.** Two real bugs found this way. Both looked
   like ordinary colour choices, and one made the single label meaning "your message
   arrived" the least readable thing on the screen in day mode
2. **Choose colour in RGB565.** Verify after quantisation, not before
3. **Shape survives sunlight; brightness does not**
4. **The accent is a fill on white and text on black.** One value, two roles
5. **Never invent data to fill a layout.** Unknown hop counts, ambiguous route hashes
   and empty lists must look like what they are. `?` is a legitimate glyph
6. **Every logical item must be reachable and visible.** If a layout can hide an item,
   the selection must pull it into view
7. **One screen, one question**
8. **An empty state must say why it is empty.** "No adverts heard **since boot**" is a
   different claim from "no adverts heard", and the first one stopped three people
   reading an empty screen as a fault
9. **The accent is spoken for.** It already means active tab, your own message, you
   can act here, alert state, and unread. Each new use makes the others weaker. A
   sixth use needs an argument, not just a free pixel

---

## 7. Questions worth an opinion

Ordered by how much a good answer would change.

**7.1 — NODES is the problem screen.** It answers "who is out there and how far", and
it does so as a list of rows with grouped hop counts. A list is the honest form for
data with unknowns in it, but it is a poor form for *distance*, which is the thing the
screen is actually about. What visual form carries "how far away, and how reliably do
I reach them" on 320×240, with four colours available, no brightness encoding, no
antialiasing, and an unknown-value case that must not be hidden? Concentric bands
have been considered and not resolved.

**7.2 — Should the wordmark become a bitmap?** The lateral shear the original spec
asked for needs `drawXbm` and a hand-pixelled mask. It is available. The costs are
that a bitmap is one fixed size (integer text scaling currently gives the mark for
free at any size), it is single-colour, it is ~2 700 pixel writes, and a hand-drawn
6×8-grid glyph set is a maintenance surface. Is the shear worth those, or is the
current cut already the mark?

**7.3 — Is the RIFT tab's composition balanced?** The wordmark went into an empty
top-right corner measuring roughly 150×76. The headline is now bottom-left-weighted
with the mark top-right and a radar box mid-right. Two `setTextSize(3)` elements now
share one baseline. Screenshot supplied.

**7.4 — SYSTEM's organisation.** Two pages of actions and diagnostics. Nothing can be
removed. Is a page split the right axis, or should it split settings from readings,
or by subsystem?

**7.5 — Emoji.** CP437 has no emoji. A small set is mapped to substitutes; the rest
draw as blocks, and the author considers that still too many blocks. The remaining
route is hand-drawn 6×8 glyphs — in which case **the discard list matters as much as
the glyphs.** An unrecognisable custom glyph is worse than a block: a block admits it
cannot draw the thing, while a vague shape claims to mean something specific. Which
emoji are drawable at 6×8 with no antialiasing and no ambiguity?

**7.6 — Prose on SYSTEM.** The direction is "readings and actions", with long
explanations moved to documentation. But four texts must stay, and they are a
different kind of text — they prevent an irreversible loss rather than explain a
thing: `Write it down - not shown again` on a generated key, `Not secret` on a
hashtag channel, why the public channel cannot be deleted, and that deleting a
channel loses its key. A rule that removes prose uniformly would strip these too. Is
there a visual treatment that distinguishes a warning-of-loss from an explanation?

**7.7 — A fifth channel.** Four colours is the arithmetic limit at 4.5:1 against both
fields. A fifth channel currently gets nothing. Is "no colour" the right treatment,
or should it get a non-colour differentiator — and if so, which, given that the
accent is spoken for and brightness is unavailable?

---

## 8. Decided against — please do not re-propose without new information

Each of these was tried or costed. Reopening one is legitimate with an argument that
addresses the reason; the reason is given so that argument is possible.

- **A gradient, a glow, or any alpha.** No blending in the driver
- **Brightness or opacity as a data encoding.** Disappears in sunlight, which is the
  operating environment
- **A fifth grey.** Nothing darker than `#6E6E6E` survives a 6×8 glyph on black
- **A light channel colour paired with a dark one.** Arithmetically unavailable: both
  values must sit in L 0.175–0.183 to work in both modes
- **The accent as text on white.** 3.5:1. Fill only, white reversed out
- **A lighter dim in day mode.** Reads as disabled rather than secondary
- **Returning the title bar.** It said what the nav bar already said, and cost a
  message of COMMS history
- **A message preview in the conversation list.** A row is one 12px line. Name, time
  and dot fit; a preview does not, and adding one costs the row count that makes the
  list useful
- **An unread count as digits.** One glyph of digits in that space is unreadable, and
  the question is whether to open the row, not how far behind you are
- **A permanent accent chip on the RIFT tab.** Would read as selected from every
  screen
- **Right-aligning own messages in COMMS.** Costs half of 320px on every outgoing
  line, where a 2px bar costs two pixels
- **Two-text-draw shear on the wordmark.** No clipping. See §7.2 for the route that
  remains open

---

## 9. What a useful review returns

In rough order of value:

1. A resolution or a strong direction on **7.1 (NODES)** — the one screen whose form
   is wrong rather than merely improvable
2. Anything in the palette or the chrome that is **measurably** wrong: a contrast
   failure, a role collision, a value that will not survive quantisation. Numbers
   preferred over impressions, because two bugs of exactly this kind were invisible
   to the eye and obvious to arithmetic
3. Composition judgements on the screenshots — balance, hierarchy, whether the one
   question each screen answers actually reads as the loudest thing on it
4. Anything in §8 that deserves reopening, **with the reason addressed**
5. Inconsistencies between screens. Five screens were built at different times and
   the chrome was reworked underneath them; drift is likely and is exactly the sort of
   thing a fresh pass catches
