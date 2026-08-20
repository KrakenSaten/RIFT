# RIFT — design handoff

For a design pass on the RIFT firmware UI. Everything here is measured from the
shipping code, not remembered: palette values were read out of `UITask.cpp` and
their contrast computed, not eyeballed.

RIFT is a keyboard-driven UI for the original LilyGO T-Deck, so the device works
standalone with no phone app. It is a fork of MeshCore; the mesh stack is upstream's,
the UI is ours.

---

## 1. The panel decides more than taste

| | |
|---|---|
| Display | 320 × 240 ST7789, 16-bit colour over SPI |
| Colour depth | **RGB565** — 5 bits red, 6 green, 5 blue |
| Font | Adafruit GFX classic, CP437. **6 × 8 px cell**, no antialiasing |
| Row pitch | 12 px throughout (`RIFT_LINE_H`) |
| Character width | 6 px (`RIFT_CHAR_W`), so a 320px row holds 53 characters |
| Input | QWERTY keyboard, trackball (4 directions), GT911 touch |
| Viewing | Handheld, outdoors, in direct sunlight |

Three consequences that constrain every visual decision:

**Colour must be chosen in RGB565, not converted into it.** Quantising to 5-6-5
moves luminance by enough to cross a contrast threshold. This was learned the
expensive way: the channel palette was picked in 24-bit, verified at 4.6:1, and every
colour actually failed against white once the panel rounded it — one to 4.38. Pick in
the space the hardware uses, then verify there.

**No antialiasing at a 6 × 8 cell.** Hairlines, gradients, small radii and thin
strokes do not exist. A 1px rule is a 1px rule. Two hues 35° apart on adjacent
markers are a coin flip; ~80° is the working minimum.

**Sunlight flattens greys.** Reflected light lays a veil over the panel and the
middle of the grey ramp collapses. Anything carrying meaning must survive that, which
means **shape or fill, not brightness**. `dim` vs `mid` is a legible difference
indoors and no difference outdoors.

---

## 2. Palette — exact values, both modes

Two modes, switched from SYSTEM. The palette swaps roles; it is not a filter over one
set of colours. Contrast figures are against that mode's own background.

### Night (default)

| Token | RGB565 | Hex | vs bg |
|---|---|---|---|
| `bg` | `0x0000` | `#000000` | — |
| `bar` | `0x18C3` | `#181818` | 1.18:1 |
| `fg` | `0xFFFF` | `#FFFFFF` | 21.0:1 |
| `mid` | `0x9CD3` | `#9C9A9C` | 7.52:1 |
| `dim` | `0x8C51` | `#8C8A8C` | 6.13:1 |
| `rule` | `0x738E` | `#737173` | 4.34:1 |
| `accent` | `0xFA00` | `#FF4100` | 6.01:1 |
| `acc_tx` | `0xFA00` | `#FF4100` | 6.01:1 |
| `ok` | `0x3E40` | `#39CB00` | 9.72:1 |

### Day

| Token | RGB565 | Hex | vs bg |
|---|---|---|---|
| `bg` | `0xFFFF` | `#FFFFFF` | — |
| `bar` | `0xEF5D` | `#EFEBEF` | 1.18:1 |
| `fg` | `0x0000` | `#000000` | 21.0:1 |
| `mid` | `0x5ACB` | `#5A595A` | 6.97:1 |
| `dim` | `0x6B4D` | `#6B696B` | 5.44:1 |
| `rule` | `0x8C51` | `#8C8A8C` | 3.43:1 |
| `accent` | `0xFA00` | `#FF4100` | **3.50:1** |
| `acc_tx` | `0x2104` | `#212021` | 16.2:1 |
| `ok` | `0x4422` | `#428610` | 4.52:1 |

### The two rules the palette encodes

**The accent has two roles, and which one depends on the mode.** `#FF4100` is 6.01:1
on black — fine as text — and 3.50:1 on white, which clears the 3:1 floor for
non-text but fails 4.5:1 for text. So in day mode the accent is a **fill with the
label reversed out of it**, never coloured text. `acc_tx` exists for the cases that
must be text: it is the accent on night and near-black on day.

**A colour that cannot work in one mode is replaced, not compromised in both.** The
status green was a single value until this handoff was written. `#39CB00` is 9.72:1 on
black and **2.16:1 on white** — below even the non-text floor — and COMMS draws
`ACK 1.2s` in it, so in day mode the one label that says a message arrived was the
least readable thing on screen. It is now `#428610` in day mode: the lightest green at
that hue still clearing 4.5:1. Nobody spotted it by looking, because it looks like a
colour choice; it was found by computing the whole palette's contrast.

**Assume the same class of bug exists in anything new.** Compute, do not judge.

---

## 3. Channel colours — a worked example of the constraint

Four colours identify channels. The count is a *result*, not a choice, and the
reasoning generalises to any new colour set.

A channel's identity colour cannot swap between modes or it stops being an identity.
So one value must clear 4.5:1 against both `#000000` and `#FFFFFF`. Contrast is a
function of luminance alone, which leaves the band **L 0.175 – 0.183** — and that
forbids any variation in lightness. The colours can therefore differ **by hue only**,
and must all be about equally dark.

Chosen by sweeping all 65 536 RGB565 values with contrast computed *after*
quantisation: 1091 qualify. Requiring ≥45° from the accent and ≥35° from the status
green, and saturation above 0.55 so a marker reads as an identity rather than as grey,
leaves 423. Four chosen to maximise the smallest neighbour gap, which is 88°.

| Slot | Hue | RGB565 | Shows as | On black | On white |
|---|---|---|---|---|---|
| 1 | 65 | `0x73E0` | `#737D00` | 4.66 | 4.50 |
| 2 | 153 | `0x0429` | `#00864A` | 4.51 | 4.66 |
| 3 | 241 | `0x631E` | `#6361F7` | 4.58 | 4.58 |
| 4 | 329 | `0xD170` | `#D62C84` | 4.55 | 4.61 |

Slot 0 — the public channel — deliberately gets none: it is the default every node
shares, and marking it would read as one of the user's own. Beyond four, none. A
repeated colour is worse than no colour: two channels that look identical can be
mistaken for each other, where two with no marker only tell you to read the name.

Full sweep and rejected candidates: `design/channel-colours.md`.

---

## 4. Layout grid

**Nav bar** — the only fixed chrome, 14px at the bottom.

- Rule at `y = 226`, labels centred at `y = 228`
- Five tabs: `RIFT · NODES · RADAR · COMMS · SYSTEM`
- Label centres at **x = 20, 81, 145, 209, 270**. Not `col × 64 + 32`: even columns
  would centre SYSTEM at 288 and clip it, so the outer two are nudged inward.
- Active tab: 2px accent underline **plus** colour. The underline exists because the
  grey step between active and inactive disappears in sunlight — colour alone was not
  enough.
- The wordmark slot gets the brand colour only while it is the active tab. A permanent
  orange chip on one tab would read as selected from every screen.

**Body** — `y = 0 … 226`. There is no title bar: it was removed, and each screen
carries its own heading only when it has something to say that the screen cannot say
for itself. COMMS gained a message of history from that.

**Text rows** — 12px pitch. A full-screen list holds ~16 rows; two columns of
diagnostics hold ~17 each.

---

## 5. Screen inventory

Each screen answers one question. That framing is load-bearing — it is what stopped
the home screen reporting a USB link nobody cared about.

| Screen | Question it answers | State |
|---|---|---|
| **RIFT** (home) | Is this radio somewhere with a live mesh? | Works. Headlines mesh receive activity, not the companion link |
| **NODES** | Who is out there, and how far? | **The problem screen — see §6** |
| **RADAR** | What Wi-Fi and BLE is around me? | Works. Big count, per-source counts, list sorted by signal, waterfall of channel occupancy over time. Devices can be marked and raise a proximity alert |
| **COMMS** | Conversations | Works. Channel strip along the top, history bottom-up, compose line. Channel colours in the strip border and beside each message |
| **SYSTEM** | Settings, and what is this device doing? | Works, and crowded. Left column actions, right column diagnostics, and a 128-line event log |

There are no current screen renderings. The old ones showed the removed title bar
and were deleted rather than kept behind a caveat: a wrong picture of a UI is worse
than none. New ones come out of this design pass.

---

## 6. The open design problem: NODES

This is the main thing to design.

**What it does now.** A constellation: nodes placed in four fixed hop columns —
`DIRECT | 1 HOP | 2 HOPS | 3+` — three rows per column, with the selected node's route
drawn as elbows through the repeaters it passed. It is the most visually interesting
screen in the firmware and it does not work.

**Why it does not work.** Real meshes are not evenly distributed. A live network looks
more like

```
DIRECT 1 · 1 hop 1 · 2 hops 2 · 3 hops 3 · 4 hops 5 · 5 hops 6 · 6+ 10
```

so almost everything collapses into the last column. The layout spends three quarters
of its width on the minority and hides the majority. Twelve nodes in one column show
three.

**Constraints that are not negotiable.**

- 320 × 240 with a 6 × 8 cell. A faithful topology graph does not fit, and pretending
  otherwise is what produced the current screen.
- A hop count can be **unknown**. A stored contact with no learned route carries
  "flood, route unknown", which is not a number. Any layout that sorts by hops needs
  an honest place to put that. A seeding attempt that treated unknown as 63 hops was
  built and reverted.
- A route hash may be **ambiguous**. Each hop byte is a prefix of a repeater's public
  key, so it can collide. Unknown and ambiguous must both render as `?`. **Never
  pick the first candidate.**
- Every logical node must be reachable by both trackball and touch. A selected node
  that is not drawn — which is what the overflow produced — gives the two inputs
  different reachable sets and a selection the screen will not admit to.

**The direction that came out of review**, and it is sound: stop trying to draw the
graph. Answer four smaller questions instead.

```
NODES  28 HEARD                        MAX 9h

 DIRECT     1-2       3-5        6+
    1         3         9         15
    ▏         ▂         ▅          █

● OSLO-01                 7h        2m
● RPT-NORD                5h        6m
> HYTTA                   9h       12m
○ MOBILE-4                6h        2h

────────────────────────────────────────
HYTTA · 9 hops · heard 12m
via  RPT-NORD > ? > RPT-7 > …

ENTER: message
```

- **Summary buckets** answer "how big is this mesh, and how spread out". Fixed ranges
  — `DIRECT | 1-2 | 3-5 | 6+`. Do **not** make the ranges adapt to the current
  network: the user loses the meaning of a column.
- **A scrollable list** answers "who is active". Default sort by most recently heard,
  which is more useful in the field than by distance. Every node reachable.
- **One selected route** answers "how did this reach me". Route detail is worth far
  more for one node than for all of them at once.

Open within that: whether the buckets get bars or just counts; where the `HEARD` count
lives; how the selected row is marked given the accent's two roles; whether age is
relative (`12m`) or absolute (`14:32`) or both when a clock exists.

---

## 7. Other open design questions

**SYSTEM is crowded.** Ten actions in the left column, seventeen diagnostic rows in
the right, and the footer already had to move out of the way once when the
diagnostics grew into it. It wants either a second page or a split between settings
and diagnostics. Every row earns its place — this is the screen that found every
hardware bug — so the answer is organisation, not deletion.

**Prose is being removed from SYSTEM.** The direction is "readings and actions"; long
explanations move to the README. But four texts stay deliberately, and they are a
different kind of text — they prevent a loss rather than explain a thing:
`Write it down - not shown again` on a generated key, `Not secret` on a hashtag
channel, why Public cannot be deleted, and that deleting a channel loses its key.
A design that removes all prose uniformly would strip these too.

**Emoji show as blocks past a small mapped set.** CP437 has no emoji. The remaining
route is hand-drawn glyphs in a 6 × 8 cell with no antialiasing — which means the
*discard list* is as much the deliverable as the glyphs. An unrecognisable custom
glyph is worse than a block: a block admits it cannot draw the thing, a vague shape
looks like it means something specific.

**Freshness is currently shape, not brightness** — filled square for heard recently,
hollow for older. That was a sunlight decision and it should survive any redesign.

---

## 8. Rules that were learned the hard way

Worth keeping whatever the design becomes.

1. **Compute contrast, do not judge it.** Two real bugs found this way; both looked
   like ordinary colour choices.
2. **Pick colour in RGB565.** Verify after quantisation.
3. **Shape survives sunlight, brightness does not.**
4. **The accent is a fill on white and text on black.** One value, two roles.
5. **Do not invent data to fill a layout.** Unknown hop counts, ambiguous route
   hashes and empty lists must look like what they are. `?` is a legitimate glyph.
6. **Every logical item must be selectable and visible.** If a layout can hide an
   item, the selection must pull it into view.
7. **One screen, one question.** If a screen needs a heading to explain itself, the
   heading is doing work the layout should be doing.
8. **An empty state must say why it is empty.** "No adverts heard **since boot**" is
   a different statement from "no adverts heard", and the first one stopped three
   people reading an empty screen as a fault.

---

## 9. What is in `design/` already

| File | Status |
|---|---|
| `channel-colours.md` | Current. The RGB565 sweep, and the record of getting it wrong first |
| `handoff.md` | The original design concept. Superseded in places; the chrome it describes is gone |

The firmware itself is the reference for anything visual:
`examples/companion_radio/ui-rift/UITask.cpp`.
