# Channel colours — what the contrast check settles

Queue item #17 is colour-coding channels and own messages in COMMS. The note
said the number of usable colours "falls out of a contrast check against both
fields". It does, and the answer is more specific than expected.

## The constraint

A channel colour has to be legible in night mode (`bg` #000000) and day mode
(`bg` #FFFFFF), from a single value — the palette swaps roles per mode, but a
channel's identity colour cannot swap or it stops being an identity.

Contrast against a field is a function of luminance alone:

| Target | Against white needs | Against black needs |
|---|---|---|
| 4.5:1 (text) | L ≤ 0.183 | L ≥ 0.175 |
| 3:1 (non-text UI) | L ≤ 0.300 | L ≥ 0.100 |

So at 4.5:1 the usable band is **L 0.175–0.183** — narrow, but not empty. Any
hue can be tuned to land in it. What it forbids is *variation in lightness*:
every channel colour must be about equally dark, so they can only differ by hue.

That rules out the obvious instinct of a light colour and a dark one.

## Correction: the first table was computed in the wrong colour space

The table that was here listed six hues at 4.6:1 on both fields. It was wrong.
Contrast was computed on the 24-bit source hex, not on the RGB565 value the panel
actually shows. Quantising to 5-6-5 raises the luminance, and every one of the six
fell below the 4.5:1 text threshold against white once rounded:

| Source | RGB565 | Rounds to | On black | On white |
|---|---|---|---|---|
| `#C84868` | `0xCA4D` | `#CE496B` | 4.79 | **4.38** |
| `#707C10` | `0x73E2` | `#737D10` | 4.67 | **4.50** |
| `#008478` | `0x042F` | `#00867B` | 4.70 | **4.47** |
| `#7068D0` | `0x735A` | `#7369D6` | 4.72 | **4.45** |

The band is L 0.175–0.1833 and quantisation moves L by enough to leave it, so a
colour has to be *chosen* in RGB565 rather than converted into it. The lesson is
narrow but it generalises to every colour in this firmware: the panel is the
authority on what a value is, not the source file.

## Four, chosen by sweeping RGB565 directly

All 65536 values, contrast computed on the quantised colour: **1091 clear 4.5:1
against both fields.** From those, requiring at least 45° from the accent (hue 15)
and 35° from the status green (hue 103), and a saturation above 0.55 so the marker
reads as an identity rather than as grey, leaves 423. Choosing four to maximise
the smallest gap between neighbours gives **88°** — better separation than the
first attempt claimed, from a smaller pool.

| Slot | Hue | RGB565 | Shows as | On black | On white | From accent | From green |
|---|---|---|---|---|---|---|---|
| 1 | 65 | `0x73E0` | `#737D00` | 4.66 | 4.50 | 49° | 38° |
| 2 | 153 | `0x0429` | `#00864A` | 4.51 | 4.66 | 138° | 50° |
| 3 | 241 | `0x631E` | `#6361F7` | 4.58 | 4.58 | 135° | 138° |
| 4 | 329 | `0xD170` | `#D62C84` | 4.55 | 4.61 | 47° | 134° |

Slot 1 sits at 4.50 against white, which meets the threshold with nothing to
spare. It is kept because the alternative was a smaller hue gap, and at a 4×6
marker the gap is what the user actually perceives.

Slot 2 is 50° from the status green in hue but far from it in lightness — the
green is `#38C800`, well above this band, and is only ever drawn on the `ACK`
line. They do not appear in the same role.

For comparison, and why neither can be borrowed:

| | Hex | On black | On white |
|---|---|---|---|
| accent | `#F84000` | 5.7:1 | 3.7:1 |
| status green | `#38C800` | 9.4:1 | 2.2:1 |

The accent is already spoken for — own messages, active tab — and is below the
text threshold on white, which is why the design makes it a fill there rather
than text. The status green is 2.2:1 on white and is only ever used on the dark
`ACK` line, so it cannot generalise.

## What this means for the feature

**Four, not six.** The six above are separated in hue, but 210 and 245 are 35°
apart and 300 and 345 are 45° apart. At a 6x8 cell with no antialiasing, and all
of them at the same lightness by necessity, adjacent hues are a coin flip. Taking
every other one — 345, 65, 175, 245 — leaves ~80° between neighbours.

Four is also enough. `MAX_GROUP_CHANNELS` is 40, but a colour per channel stops
being an identity long before that; beyond four the honest fallback is no colour,
which is what an unassigned channel should get.

**It has to be a marker, not the text.** Not because of contrast — these clear
4.5:1 — but because the message body is already `fg` and the sender line already
carries time, name and delivery state in three different roles. A fifth colour in
that row competes with the accent bar that marks own messages. A small block
before the sender name reads as a label rather than as emphasis.

## Decided when it was built

**The channel name in the history row is drawn in the channel's colour**, so the
row and the tab above it carry one identity.

This was first built as a 4x6 block beside the name, on the argument that the row
already carried three roles and a fourth colour in the text would compete with the
accent bar marking own messages. Seen on hardware that was the weaker answer: a
block asks the eye to associate a mark with a border, where the name in the same
colour simply *is* the association. Changed at the user's request after looking at
both.

The block argument was about composition, not contrast — and contrast is what made
the change available. These four were chosen in the 4.5:1 text band rather than the
3:1 band a non-text marker could have used. Had they been picked for a block, the
name could not have been coloured without failing the check.

**Outgoing messages had to be fixed to make this work at all.** `sendToChannel`
recorded this node's own name as the log origin, which said the same thing as the
accent bar beside it and left the row unable to name the channel — so a message you
received on a channel could be coloured and one you sent to it could not. It now
records `to <channel>:`, matching what `sendToContact` already did for a DM.
`riftOriginName` normalises that decoration away, and lives in `RiftLogic.h` so its
edge cases — `to :`, a name too long for the buffer, `general` against `general-2` —
have tests rather than an argument.

**The tab strip carries the colour in the border of inactive tabs.** The active tab
keeps its accent fill untouched: being the selected channel is the stronger thing
to say, and a colour bar at the same lightness as the accent would only muddy it.
So the channel you are on is identified by name inside the fill, and the ones you
are not by colour in the outline.

**Assignment is by channel slot, and that is safe because slots are stable.**
`MyMesh::removeChannel` blanks a slot in place rather than compacting, so deleting
one channel does not recolour the others. Slot 0 — the public channel — gets no
colour deliberately: it is the default every node shares, and marking it would
read as one of the user's own.

**Matching is by name.** Channel messages carry the channel name as their log
origin, because MeshCore prepends the sender to the text itself, so the tab cache
already maps name to slot and no field had to be added to the log or its file
format. Two limits follow and are accepted: a direct message from a contact whose
name equals a channel name picks up that colour, and a channel named longer than
the cache's 20 bytes gets no colour rather than the wrong one.

---

# The meshcore.io blue

Queue item #11 wanted `meshcore.io` on the home screen in MeshCore's brand blue,
and was blocked because the site returns 403 to automated fetches. It does not to
a real browser: the value is **`#2563EB`** — Tailwind `blue-600` — used on the
site's call-to-action buttons and icons.

Two other saturated colours on that page are not MeshCore's and should not be
mistaken for it: `#5865F2` is Discord's and `#FF5700` is Reddit's, both on social
links.

It cannot be used unmodified:

| | On black | On white |
|---|---|---|
| `#2563EB` as published | **3.9:1** | 5.4:1 |
| `#3870E8`, same hue, +5% lightness | 4.7:1 | 4.5:1 |

3.9:1 is below the text threshold, and night is the default mode — so the brand
value as published is the one case where it is least readable. Lifting the
lightness from 53% to 58% keeps the hue and clears both fields.

Whether to use the exact brand value or the readable one is a judgement about
which matters more. The precedent in this design is already set: the accent keeps
its value and changes role instead, because the design would rather be honest
about a contrast limit than quietly print something no one can read.
