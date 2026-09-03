# RIFT — design review brief, September 2026

Self-contained. Everything a reviewer needs is in this file; no repository access is
required. Screenshots are supplied separately by the author and are ground truth for
appearance — where this document and a screenshot disagree, the screenshot is right
and the mismatch is worth reporting.

This supersedes `design/DESIGN-REVIEW-BRIEF.md` (23 August 2026). **Section 0 is the
delta**, for a reviewer who has read that one. Everything after it is complete on its
own, so a reviewer who has not read it loses nothing by starting at section 1.

**What RIFT is, in one line:** a custom firmware UI for the LilyGO T-Deck, a handheld
LoRa mesh radio with a physical keyboard, a trackball, a capacitive touchscreen and a
320×240 panel. It is a field terminal, not a phone app. The user is outdoors, possibly
in sunlight, possibly wearing gloves, and wants to know whether the radio is working.

**A warning about the screenshots.** Everything in `design/screens/` dates from
20 August and therefore predates the COMMS redesign, the sender colours, the
three-button home row, the repeater panel, the discovery panel and the channel-scope
screen — that is, most of what this document describes. **Fresh captures are needed
before any composition judgement is worth making.** Where this document describes a
screen and the supplied image shows something else, the image is probably the old one.

**What this review is for:** visual and layout design, and now also **interaction**
design — section 4 is new and is the largest single change since August. Protocol,
firmware and build concerns remain out of scope except where they constrain a visual
choice, in which case the constraint is stated in section 1.

Section 7 lists the questions worth an opinion. Section 8 lists what has already been
decided against, so a review does not spend itself re-proposing it. Section 9 lists
**errors found in the previous brief** — a reviewer working from the August document
has been reasoning from at least one colour that is not on the device.

---

## 0. What changed since the August brief

Eleven days of work, forty-three commits. In rough order of design significance:

1. **The touchscreen became a real input.** In August the panel was used for taps
   only. It now scrolls NODES, RADAR, COMMS, SYSTEM and the conversation list by
   drag. This changed layout assumptions on four screens and introduced a whole
   class of interaction question the previous brief did not cover. **Section 4.**
2. **Sender colours in COMMS, and the palette grew from four to twelve.** The August
   brief's arithmetic — four colours is the ceiling — was about *channel identity*.
   Colouring *people* turned out to be a different problem with a different ceiling.
   **Section 2.4.** This is the most likely place for a reviewer to think the
   project has contradicted itself, so the reasoning is given in full.
3. **Clipping now exists in practice, by masking.** The August brief listed clipping
   under "what the panel forbids". That is still true of the driver, but COMMS now
   gets the effect by drawing and then painting over. This **may reopen 7.2**, the
   wordmark shear, which was closed on the grounds that clipping was unavailable.
4. **Three new full-screen surfaces**: a repeater control panel, a discovery
   results panel, and a channel-scope editor. Plus a tropo-conditions alert.
5. **The home screen gained two more buttons** and now has a three-button row.
6. **SYSTEM gained prose** — five lines of it — after 7.6 asked whether prose
   belongs on SYSTEM at all. Flagged honestly rather than quietly.

Resolved since August and therefore **not** open questions any more: COMMS scrolling
feel, the conversation list being unusable, sender identity in channels.

Still open and unchanged: **NODES** (7.1) remains the weakest screen and the single
most valuable thing a review could resolve.

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
| Clipping | **Not in the driver — but see below** | No clip rectangle exists. The effect is now obtained by drawing and then masking |
| Primitives | `fillRect`, `drawRect`, `drawXbm`, text | No line, no circle, no arc, no gradient, no alpha |
| Diagonals | Composed from `fillRect` runs | Possible and in use, but every diagonal is hand-stepped |
| Bitmaps | `drawXbm`: 1-bit mask, single colour, per-pixel | A 96×28 mark is 2 688 pixel writes — fine on a screen drawn once, too slow for one that redraws on a timer |
| Redraw budget | Shares the SPI bus with the LoRa radio | A screen redrawing every 700ms must be cheap. Blocking the bus starves the radio, and **there is no watchdog to catch it** |

### 1.1 Clipping, revised

The driver has no clip rectangle and no scissor. What COMMS does instead:

1. Draw the scrolling content, including the parts that fall outside the viewport
2. `fillRect` the background over everything above and below the viewport
3. Redraw the chrome that the mask just destroyed — heading, tab strip, nav rule

This is real clipping for any region bounded by **horizontal edges**, which is what a
scrolling list needs. It costs two full-width fills and a chrome redraw per frame.

Two things it does not give:

- **Non-rectangular clipping.** A diagonal mask is possible in principle — diagonals
  are already composed from `fillRect` runs elsewhere — but it has not been built,
  and the cost is roughly 80 fills per diagonal per frame.
- **Clipping against something already drawn.** The mask paints background, so it
  erases whatever is underneath. It works because the chrome can be cheaply redrawn
  on top. It would not work over an expensive background.

**One bug came from this.** The bottom mask was sized 5px when a message block can
overrun the viewport by 60px, so a long message drew through the compose line. Found
in review, not on screen. It is worth knowing that this technique fails *quietly* and
in proportion to content that only sometimes exists.

### 1.2 Sunlight is the design environment

This is the constraint that has changed the most decisions. Under reflected daylight
the difference between two greys disappears entirely, while the difference between a
filled square and a hollow one does not. Every place RIFT could have encoded a value
in brightness, it encodes it in form instead.

---

## 2. Palette

Two modes, same geometry, swapped colour table. A user toggles between them; nothing
moves when they do.

### 2.1 Night (default)

| Role | Hex | RGB565 | Use |
|---|---|---|---|
| `bg` | `#000000` | `0x0000` | Field |
| `bar` | `#1A1A1A` | `0x18C3` | Chrome band |
| `fg` | `#FFFFFF` | `0xFFFF` | Primary text |
| `mid` | `#9A9A9A` | `0x9CD3` | Secondary text, chrome, labels |
| `dim` | `#8A8A8A` | `0x8C51` | De-emphasised text |
| `rule` | `#707070` | `0x738E` | Hairlines and borders |
| `accent` | `#FF4100` | `0xFA00` | Brand, active state, alert |
| `ok` | `#39C800` | `0x3E40` | Success, delivery confirmed |

### 2.2 Day

| Role | Hex | RGB565 | Note |
|---|---|---|---|
| `bg` | `#FFFFFF` | `0xFFFF` | |
| `bar` | `#EFEBEF` | `0xEF5D` | |
| `fg` | `#000000` | `0x0000` | |
| `mid` | `#5A5A5A` | `0x5ACB` | |
| `dim` | `#6B6B6B` | `0x6B4D` | **Darker than `mid`, not lighter** |
| `rule` | `#8C8C8C` | `0x8C51` | |
| `accent` | `#FF4100` | `0xFA00` | Same value, different role |
| `accent as text` | `#202020` | `0x2104` | The accent is unreadable as text on white |
| `ok` | `#428610` | `0x4422` | Not the night green. 4.52:1 on white |

### 2.3 The three rules the palette encodes

**Nothing darker than `#6E6E6E` on black survives a 6×8 glyph with no antialiasing.**
Empirical, not calculated. It is why there is no fourth grey.

**De-emphasis inverts with the field.** On black, dim means darker. On white, dim must
*also* mean darker — a lighter grey on white reads as *disabled*, not as *secondary*.
This is the single easiest mistake in a two-mode palette.

**The accent has one value and two roles.** `#FF4100` measures 6.0:1 against black and
3.5:1 against white. On black it may be text. On white it may only be a fill with
white reversed out of it — never text, never a hairline expected to be read. Two
shipped bugs came from ignoring this.

### 2.4 Channel colours, and why there are now twelve name colours

**This section exists because it looks like a contradiction and is not.** The August
brief proved four was the ceiling. The device now draws twelve. Both are correct; they
answer different questions.

**The four channel colours.** A channel's identity colour cannot swap between modes or
it stops being an identity. So one value must clear 4.5:1 against **both** `#000000`
and `#FFFFFF`. That confines luminance to L 0.175–0.183 — a band, not a point. Of
65 536 RGB565 values, **1 091 clear it**, all of them equally dark, differing only by
hue. "A light one and a dark one" is arithmetically unavailable.

After holding 45° of hue from the accent and 35° from the status green, four remain:

| Slot | RGB565 | RGB | Hue | On black | On white |
|---|---|---|---|---|---|
| 1 | `0x73E0` | `rgb(115,125,0)` | 65° | 4.66 | 4.50 |
| 2 | `0x0429` | `rgb(0,133,74)` | 153° | 4.51 | 4.66 |
| 3 | `0x631E` | `rgb(98,97,246)` | 241° | 4.58 | 4.58 |
| 4 | `0xD170` | `rgb(213,44,131)` | 329° | 4.55 | 4.61 |

Slot 0 is the shared public channel and deliberately gets **no** colour: it is the
default every node has, and marking it would read as one of the user's own. A fifth
channel also gets none — a repeated colour is worse than an absent one, because it
asserts an identity that is false.

**The twelve name colours are a different problem.** A channel message goes on the air
as `"<sender>: <text>"`. The receive path told the UI the message came from the
*channel*, so every row in a channel was drawn in that channel's one colour, and a
conversation between three people looked like a conversation between one.

Colouring the *sender* instead removes the both-modes constraint's companion
requirement — hue separation from the accent and the green still holds, but the
**mutual** separation requirement is much weaker, because a name colour is a reading
aid sitting next to the name itself, not a symbol that has to be identified alone.

The ceiling is therefore not the palette; it is whether two colours can be told apart
at a glance in a 6px font in daylight. Measured as the worst separation between any
two entries, the greedy pick degrades:

| Count | 6 | 8 | 12 | 16 | 20 |
|---|---|---|---|---|---|
| Worst pair separation | 50 | 33 | **25** | 21 | 16 |

Below about 20 the two colours have to be compared side by side, which is no use for
reading a conversation. **Twelve is the last count that still buys anything.** Four
meant two names collided one time in four, which in a channel with a handful of people
is most of the time.

The colour is assigned by FNV-1a hash of the name, so the same person is the same
colour on every device, across reboots, and after the channel list is edited. Random
assignment would mean somebody changes colour when the firmware restarts, which is
worse than no colour at all.

| # | RGB565 | RGB | on black | on white | from accent | from green | nearest other |
|---|---|---|---|---|---|---|---|
| 0 | `0x73E0` | `rgb(115,125,0)` | 4.66 | 4.50 | 49 | 70 | 50 |
| 1 | `0x0429` | `rgb(0,133,74)` | 4.45 | 4.71 | 111 | **19** | 48 |
| 2 | `0x631E` | `rgb(98,97,246)` | 4.56 | 4.61 | 110 | 84 | 29 |
| 3 | `0xD170` | `rgb(213,44,131)` | 4.52 | 4.65 | 63 | 104 | 31 |
| 4 | `0x039C` | `rgb(0,113,230)` | 4.50 | 4.66 | 137 | 61 | 31 |
| 5 | `0xB81F` | `rgb(189,0,255)` | 4.61 | 4.56 | 109 | 130 | 30 |
| 6 | `0x63EC` | `rgb(98,125,98)` | 4.63 | 4.54 | 74 | 56 | 29 |
| 7 | `0xE00B` | `rgb(230,0,90)` | 4.51 | 4.65 | 60 | 123 | 25 |
| 8 | `0xB1BC` | `rgb(180,52,230)` | 4.57 | 4.59 | 95 | 106 | 29 |
| 9 | `0x9334` | `rgb(148,101,164)` | 4.66 | 4.51 | 80 | 81 | 29 |
| 10 | `0x2BD7` | `rgb(41,121,189)` | 4.56 | 4.60 | 109 | 56 | 29 |
| 11 | `0xD814` | `rgb(222,0,164)` | 4.64 | 4.52 | 82 | 126 | 25 |

Entries 0–3 **are the four channel colours, reused unchanged**. See 7.8 — this is a
deliberate reuse with a consequence nobody has judged.

Entry 1 sits **19** from the ok green, well inside what every later entry was rejected
for. It was already on screen as channel colour 2, so it stayed. It is not a
precedent, and a reviewer is entitled to say it should go.

### 2.5 `RIFT_CHAN_COL_NONE` is `0x0000`

"No colour" is the literal value black. In night mode that is the background, so a
missed check draws nothing and the bug is invisible. In day mode it is `fg`, so the
same missed check draws what looks like ordinary text. Every current read site checks
the sentinel. It is noted as a hazard, not as a live bug.

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
  permanent orange chip on one tab would read as selected from every screen
- Bottom right: battery percentage in `mid`, switching to accent at ≤15%. On SYSTEM
  the same slot carries the page number instead

**Body — `y 0…226`. There is no title bar.** It used to be a filled 16px band carrying
the wordmark, the battery and the screen name. The first two moved to the nav bar so
all chrome sits on one edge; what remained said only what the nav bar already said.
Removing it gave COMMS one extra message of history.

**Headings are per-screen and conditional.** A screen draws one line at `y = 2` in
`mid` *only when it has something to say that its own layout cannot*: which step of a
flow you are in, which contact a message is aimed at, how many nodes were heard.
Screens with nothing to add draw nothing. If a screen needs a heading to explain
itself, the heading is doing work the layout should be doing.

**Row pitch is 12px everywhere.** `RIFT_LINE_H` is not a per-screen choice. The
character cell is 6×8, so a full-width row is 53 characters.

---

## 4. Interaction model — new since August

Three inputs, all live at once, none of them dominant. This is the part of the design
that has moved most and has been reviewed least.

### 4.1 The three inputs

| Input | Physical | What it does |
|---|---|---|
| Keyboard | 4×10 membrane, no modifiers beyond shift/alt | Text; `ENTER` acts; `BACKSPACE` goes back |
| Trackball | 4-way, reported as arrow keys | Moves a selection, one step per detent |
| Touch | GT911 capacitive, polled at 8ms | Taps a target; drags a list |

**The trackball is the only input that works with gloves on.** Every function reachable
by touch must also be reachable by trackball, and that ordering is deliberate: touch is
the convenience, the trackball is the guarantee. Nothing is touch-only.

### 4.2 Drag

Touch was polled at 25ms when its only job was noticing a tap. Following a finger is a
different job — at 40 samples a second a moderate drag advances ten pixels between
samples, and the scroll can only move in those steps. It is now polled at **8ms**. One
I2C transaction of a few bytes is well under a millisecond against a frame that spends
tens of milliseconds pushing 153KB over HSPI.

Two different scroll models, chosen per screen and for a stated reason:

- **Lists scroll by row.** NODES, RADAR, SYSTEM and the conversation list step their
  selection, because stepping is what the arrow keys already do and one idea of a
  "step" is better than two. `RIFT_DRAG_PITCH` is 16 pixels of finger travel per row,
  with the remainder carried between samples so a slow drag still eventually moves.
- **COMMS scrolls by pixel.** Its blocks vary from one line to six, so a step per
  message moved the view by wildly different amounts for the same gesture. This is
  the screen where scrolling had to *feel* right, and it took four attempts.

**A drag suppresses its own release tap.** The panel reports once per completed tap, on
release. Without suppression, every scroll ended by opening whatever was under your
finger when you let go. This was a real shipped bug for several days — a dangling
`else` in front of an unbraced if-chain — and the user's description of the symptom
("it springs back up when I let go") named it exactly.

**Drag does not wrap; arrow keys do.** Rolling the trackball past the end of a list
returns to the top. Dragging past the end stops. Under a finger, wrapping reads as the
list having *jumped*, not as the cursor having reached the end.

### 4.3 Hit-testing

Rows are hit-tested against **where they actually landed**, recorded during render,
not against a computed pitch. Section headings take a row each and move everything
below them; a computed offset picks the wrong row, and on the conversation list that
means opening a different person's conversation. This mistake has been made twice in
this codebase — on NODES and on SYSTEM — before the rule was written down.

### 4.4 Overlays

Five surfaces are drawn *over* the screen beneath rather than replacing it: message
preview, rename/watch, the repeater panel, the Nordic character picker, and discovery
results. The screen underneath keeps its state and is handed back untouched.

The mechanism exists because of the Nordic picker: holding a vowel to choose `å` must
not disturb the half-typed message underneath, and the base letter is already in the
buffer because the initial press inserted it normally — holding a key must not make
ordinary typing feel delayed.

Overlays are drawn as a bordered box inset from the edges, with the border in the
accent. The discovery panel is `x=6 y=22 w=308 h=186`.

### 4.5 Confirmation

Destructive repeater commands arm on the first `ENTER` and fire on the second. The
armed state is held as **the index of the armed row**, not a boolean, so moving the
cursor and pressing `ENTER` cannot fire the command you were confirming.

---

## 5. Screen inventory

Each screen answers exactly one question. That framing is load-bearing: it is what
stopped the home screen reporting a USB link nobody cared about.

### RIFT (home) — "is this radio somewhere with a live mesh?"

Headline at `setTextSize(3)`: `ACTIVE` / `IDLE` / `QUIET` / `NO SIGNAL`, coloured
green / white / grey / accent. `meshcore.io` above it in `mid`, lowercase against the
all-caps convention because a URL in capitals reads worse. The wordmark top right.

Three data rows under the headline — last receive age, packet count, USB/BLE link
state — because every hardware problem on this project was settled by putting the real
number next to the verdict.

A small radar animation right of centre: three nested rectangles and one accent blip at
eight discrete positions.

**New: a three-button row at `y = 198`**, 14px tall — `DISCOVER 0-HOP`, `ADVERT NEAR`,
`ADVERT MESH`. The selected button is **filled, not outlined**; a note line at `y = 214`
explains what the selected one will do (`direct RF only - use MESH before a first DM`).
The two advert buttons moved here from a menu because they are the two things a user
does when arriving somewhere new, and they were three levels deep.

**When a tropo opening is running, the radio parameter line is replaced** by
`TROPO OPEN · peak N hops`.

### NODES — "who is out there, and how far?"

**The weakest screen, and the one most worth reviewing.** A scrollable list from
`y = 68` to `y = 226`, with variable row heights. Hop counts are grouped into buckets.
Unknown values draw as `?` rather than as a guess. Selecting a repeater and pressing
`ENTER` opens the repeater control panel.

Now drag-scrollable. That fixed the *access* problem — the list is long and the
trackball is slow — without touching the *form* problem, which is 7.1.

### RADAR — "what Wi-Fi and BLE is around me?"

A large count, then three signal bands — `CLOSE` / `MID` / `FAR`, each printing its own
dBm range (`-30..-60`, `-60..-80`, `-80..-100`) rather than asking the user to take the
label on trust — rendered as rows of 6×8 cells, one cell per device. **Filled for close
and mid, hollow for far**: a brightness step would disappear outdoors, a form change
does not. A row that overflows draws an accent cell rather than silently clipping.

Below: a list sorted strongest-first, and a waterfall of channel occupancy over time.
Devices can be given a friendly name and marked to raise a proximity alert. A watched
device that is also selected carries **two marks rather than one colour**, because
colour cannot say two things in one cell.

### COMMS — "conversations"

One conversation at a time. A four-tab channel strip along the top, history laid out
bottom-up from the compose line, compose line at the foot.

Own messages carry a 2px accent bar down the left edge rather than being right-aligned
— on 320px, right alignment costs half the width of every outgoing line, and the bar
costs two pixels.

Each row is a time, the origin name, and a right-hand slot holding either delivery
state or hop count. **The name is now the sender's, in the sender's colour** (2.4), not
the channel's. Channel names keep the channel's colour, so an outgoing row and the tab
above it match.

The channel colour appears on the tab strip as the tab's **border**, not its fill: the
active tab is already a stronger thing to say, and a colour bar at the same lightness
as the active marker would compete with it.

**The conversation list** (`ENTER` on an empty compose line): `CHANNELS` and `DIRECT`
section headings each with a hairline, a 2px colour chip for channels, the name, the
time of the last message right-aligned, and **a 3×3 accent square where there is
something unread**. Unread is a shape, not a colour change, and the count is
deliberately not drawn — the question a row answers is whether to open it, not how far
behind you are. The same dot appears on channel tabs you are not looking at.

When the list is full it says `list full - contacts with no history not shown`, worded
precisely because the previous wording was read as the contact table being full, which
is a different and much more serious thing.

**Five improvements to this list are designed and not built** — see 7.7.

### SYSTEM — "settings, and what is this device doing?"

Two pages. Left column actions, right column diagnostic rows, plus a 128-line event
log. **Crowded, and every row earns its place** — this is the screen that found every
hardware bug on the project. The answer is organisation, not deletion.

Page 2 now carries `TROPO`, reporting both halves of the signal so it can be judged
rather than trusted: `ok <days> p<peak> b<baseline>/<seen>`.

### Overlay: repeater control — new

Reached from NODES. Four modes: view, password, command, menu. Shows the repeater's
stats — uptime, airtime, duty cycle, battery, packet counters — decoded from a binary
frame with per-field length gates, and telemetry in Cayenne LPP with real units.

A command can be **picked from a menu** rather than typed, because the T-Deck keyboard
is not a place anyone wants to type `set repeat.enabled 1`. Secrets typed here are
redacted on screen for 30 seconds — a window, not a one-shot flag, because the reply
echoes back and would have redrawn the secret.

### Overlay: discovery results — new

`DISCOVER 0-HOP` collects direct-range repeaters. **Two SNR columns are the point of
the feature**: `rx` is how well we heard the response, `tx` is the SNR the repeater
reported for our request. Nothing else in this firmware can show the second one —
adverts only ever tell us the inbound half — and an asymmetric link is exactly the
thing worth knowing before you rely on a route.

### SYSTEM sub-screen: channel scope — new

A list of channels against their region, `(node default)` where none is set. The
selected row is an **accent fill with white reversed out**, which is the sanctioned
day-mode use of the accent and reads identically in both modes.

The entry screen carries **four lines of explanatory prose**. See 7.6 — this is new
prose on SYSTEM, added after the question of whether prose belongs there was raised.

### Boot: splash

The wordmark at `x=32 y=78` in white, seam running from the left screen edge and
stopping at the middle, strapline below right.

---

## 6. Rules already established

Each learned from a shipped mistake. They should survive any redesign.

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
9. **The accent is spoken for.** It already means active tab, your own message, you can
   act here, alert state, unread, overlay border, and armed confirmation. Each new use
   makes the others weaker. **This list has grown by two since August** and is worth a
   reviewer's attention in its own right
10. **New since August — every touch target must have a keyboard route.** Touch is the
    convenience; the trackball is the guarantee, because it is what works with gloves
11. **New since August — a fix applied to one screen must be applied to its siblings.**
    Touch hit-testing, then drag support, each landed on some screens and not others,
    twice, and each time the missing one was found by a user rather than by us

---

## 7. Questions worth an opinion

Ordered by how much a good answer would change.

### 7.1 — NODES is still the problem screen

Unchanged from August, and now the only major open question. It answers "who is out
there and how far", and it does so as a list of rows with grouped hop counts. A list is
the honest form for data with unknowns in it, but it is a poor form for *distance*,
which is the thing the screen is actually about.

What visual form carries "how far away, and how reliably do I reach them" on 320×240,
with the colour constraints in section 2, no brightness encoding, no antialiasing, and
an unknown-value case that must not be hidden? Concentric bands have been considered
and not resolved.

**New information since August:** the screen now scrolls by drag, so a form that is
taller than 158px is no longer disqualified on access grounds. And RADAR's
filled/hollow cell grid is a proven pattern on this hardware for "many items, coarse
distance bands" — it may or may not transfer, since RADAR's items are anonymous and
NODES' items have names that must be readable.

### 7.2 — Should the wordmark become a bitmap? Reopened

The mark is `RIFT` at `setTextSize(3)`, cut along a shallow diagonal — y 96 at x=0
falling to y 48 at x=319, about −8.8°, held as an integer rise over run (12/78) so
every instance produces an identical staircase. The cut is a 4px gap blanked through
the glyphs with the 2px accent inside it; the 1px of air either side is what makes it
read as a seam rather than as a line struck through the letters.

The original spec called for two text draws offset 3px so the halves **shear** along
the seam. That was abandoned because whichever half is drawn second spills across the
cut, and the blank that removes the spill also removes the half it was meant to keep.

**This closed on the grounds that clipping was unavailable. Section 1.1 says it now
is** — for horizontal edges, and in principle for a hand-stepped diagonal, since
diagonals are already composed from `fillRect` runs. Draw the upper half, mask below
the diagonal, draw the lower half offset, mask above it.

So the question is now a cost question rather than a possibility question. The diagonal
mask is roughly 80 fills per instance per frame. That is nothing on the boot screen,
which is drawn once. The RIFT tab redraws every 700ms and shares the SPI bus with the
radio. **Is the shear worth it on the boot screen only, accepting that the two
instances would then differ?**

The bitmap route remains available and its costs are unchanged: one fixed size,
single-colour, ~2 700 pixel writes, and a hand-drawn glyph set is a maintenance
surface.

### 7.3 — Is the RIFT tab's composition still balanced?

It was asked in August with two `setTextSize(3)` elements sharing one baseline. It has
since gained **two more buttons and a note line**, and the tropo state can now replace
the radio parameter line. The bottom third of the screen is considerably busier than
the composition that was reviewed. Screenshot supplied.

### 7.4 — SYSTEM's organisation

Two pages of actions and diagnostics. Nothing can be removed. Is a page split the right
axis, or should it split settings from readings, or by subsystem? It has since gained a
tropo row and a channel-scope sub-screen, so the pressure has increased rather than
eased.

### 7.5 — Emoji

CP437 has no emoji. A small set is mapped to substitutes; the rest draw as blocks, and
the author considers that still too many blocks. The remaining route is hand-drawn 6×8
glyphs — in which case **the discard list matters as much as the glyphs**. An
unrecognisable custom glyph is worse than a block: a block admits it cannot draw the
thing, while a vague shape claims to mean something specific. Which emoji are drawable
at 6×8 with no antialiasing and no ambiguity?

### 7.6 — Prose on SYSTEM, and a rule that has already been violated

The direction was "readings and actions", with long explanations moved to
documentation. Four texts had to stay because they prevent an irreversible loss rather
than explain a thing: `Write it down - not shown again` on a generated key, `Not
secret` on a hashtag channel, why the public channel cannot be deleted, and that
deleting a channel loses its key.

**Since then the channel-scope screen added five more lines of prose**, of the
explaining kind, not the warning kind:

> Region name. Empty clears it back to the node default.
> Anyone using the same name reaches the same region - the key is the name.
> A scope keeps a channel inside a region.

This is reported rather than defended. Two questions: is there a visual treatment that
distinguishes a warning-of-loss from an explanation, and does that scope text survive
the rule or should it move to documentation?

### 7.7 — Five designed-and-unbuilt improvements to the conversation list

The list currently gives each row: unread dot, channel colour chip, name, last-message
time. Touch and drag landed last; the following were held back deliberately so the
access fix could be judged alone. They are one render pass together.

1. **Relative age instead of a clock time.** `14:52` requires the reader to know what
   time it is now. `3m` / `2h` / `4d` does not. Costs nothing in width
2. **Node type** — `room` / `repeater` / `sensor` — because a room server and a person
   behave completely differently and currently look identical
3. **A filled selection bar** instead of the `> ` prefix, matching the channel-scope
   list, which already does this
4. **Name colours matching the message view**, so a person is the same colour in the
   list and in the conversation
5. **A count in the header**, because "how many conversations do I have" currently
   requires scrolling to the end

The row is 12px and 53 characters. **Do all five fit, and if not, which two lose?** The
August decision against a message preview (section 8) was made on exactly this budget,
so the budget is real.

### 7.8 — Name colours 0–3 are the channel colours

A person whose name hashes to index 0 is drawn in **channel 1's colour**, which is also
on the tab strip and on every outgoing row to that channel. The reuse was deliberate —
those four are the measured ones — but the consequence has not been judged: in a
channel view, a sender's name can be the same colour as a *different* channel's tab.

Is that a collision that matters, given the name is written next to the colour? Or
should the first four name colours be four *different* entries from the 1 091 that
qualify, leaving the channel colours unique to channels?

### 7.9 — A fifth channel

Four colours is the arithmetic limit for channel identity. A fifth channel gets
nothing. Is "no colour" the right treatment, or should it get a non-colour
differentiator — and if so, which, given that the accent is spoken for and brightness
is unavailable?

Note that 7.8, if answered by giving names their own four, changes the arithmetic
available here.

### 7.10 — Two scroll models on one device

Lists step; COMMS scrolls by pixel. Both were chosen for stated reasons (4.2) and both
now feel right to the author. But a user moves between them constantly, and nobody has
asked whether the *inconsistency* costs more than either model's fit gains.

---

## 8. Decided against — please do not re-propose without new information

Each was tried or costed. Reopening one is legitimate with an argument that addresses
the reason; the reason is given so that argument is possible.

- **A gradient, a glow, or any alpha.** No blending in the driver
- **Brightness or opacity as a data encoding.** Disappears in sunlight
- **A fifth grey.** Nothing darker than `#6E6E6E` survives a 6×8 glyph on black
- **A light channel colour paired with a dark one.** Arithmetically unavailable: both
  values must sit in L 0.175–0.183 to work in both modes
- **The accent as text on white.** 3.5:1. Fill only, white reversed out
- **A lighter dim in day mode.** Reads as disabled rather than secondary
- **Returning the title bar.** It said what the nav bar already said, and cost a
  message of COMMS history
- **A message preview in the conversation list.** A row is one 12px line. Name, time
  and dot fit; a preview does not
- **An unread count as digits.** One glyph of digits in that space is unreadable, and
  the question is whether to open the row, not how far behind you are
- **A permanent accent chip on the RIFT tab.** Would read as selected from every screen
- **Right-aligning own messages in COMMS.** Costs half of 320px on every outgoing line,
  where a 2px bar costs two pixels
- **Random per-session name colours.** Somebody changing colour on reboot is worse than
  no colour at all
- **Wrapping a list under a drag.** Reads as the list jumping, not as reaching the end
- **A touch-only control anywhere.** The trackball is the gloved-hands guarantee
- ~~Two-text-draw shear on the wordmark~~ — **reopened, see 7.2**

---

## 9. Errors in the August brief

Reported because a reviewer working from that document has been reasoning from at least
one value that is not on the device.

**Channel colour 1 was given as `#73FF00`.** The device draws `0x73E0`, which is
`rgb(115,125,0)` — `#737D00`. Those are not close: one is a bright chartreuse, the
other is a dark olive. The brief's own contrast figures (4.66 / 4.50) are correct for
`#737D00` and impossible for `#73FF00`, which measures about 1.5:1 on white. It was a
transcription error in the document, not a bug in the firmware. Slots 2–4 in that table
are correct to within RGB565 rounding.

**Two contrast tables in the source disagree with each other.** The channel-colour
table and the name-colour table give different distances from the accent and from the
green for the same colours — slot 2 is `accent 138 / green 50` in one and
`accent 111 / green 19` in the other. They were produced by two sweeps at different
times, and the palette gained a second (day-mode) green in between. **Neither has been
re-derived.** A reviewer relying on those separation figures should treat them as
approximate and say so if it matters to a recommendation.

---

## 10. What a useful review returns

In rough order of value:

1. A resolution or a strong direction on **7.1 (NODES)** — the one screen whose form is
   wrong rather than merely improvable
2. **7.7** — which of the five conversation-list improvements fit in 53 characters, and
   which lose. This is the change most likely to ship immediately
3. Anything in the palette or the chrome that is **measurably** wrong: a contrast
   failure, a role collision, a value that will not survive quantisation. Numbers
   preferred over impressions, because three bugs of exactly this kind — two shipped,
   one in the brief itself — were invisible to the eye and obvious to arithmetic
4. A judgement on **rule 9**: the accent now carries seven meanings. Which of them
   should be something else?
5. Composition judgements on the screenshots — balance, hierarchy, whether the one
   question each screen answers actually reads as the loudest thing on it
6. Anything in section 8 that deserves reopening, **with the reason addressed**
7. Inconsistencies between screens. Six surfaces were built at different times, the
   chrome was reworked underneath them, and the interaction model changed after four of
   them were finished. Drift is likely and is exactly what a fresh pass catches

**What this review cannot use:** anything that requires alpha, antialiasing, a font
weight, or a colour chosen before quantisation. Those are not preferences.
