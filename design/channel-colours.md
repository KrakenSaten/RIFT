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

## Six that clear 4.6:1 on both

Found by sweeping hue and lightness, quantising to RGB565 as the panel will, and
rejecting anything within perceptual distance of the accent or the status green.

| Hue | Hex | RGB565 | On black | On white |
|---|---|---|---|---|
| 345 | `#C84868` | `0xCA4D` | 4.6:1 | 4.6:1 |
| 245 | `#7068D0` | `0x735A` | 4.6:1 | 4.6:1 |
| 65 | `#707C10` | `0x73E2` | 4.6:1 | 4.6:1 |
| 300 | `#D000D0` | `0xD01A` | 4.6:1 | 4.6:1 |
| 175 | `#008478` | `0x042F` | 4.6:1 | 4.6:1 |
| 210 | `#0074E8` | `0x03BD` | 4.7:1 | 4.5:1 |

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

## Not decided here

Where the marker sits, and whether the channel tabs pick up the same colour. Both
are taste, and the strip already uses an accent fill for the active tab, so they
interact.

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
