# SYSTEM reorganisation — field readings, development diagnostics, and the radio

Agreed direction, not yet built. Written before the code for the same reason the NODES
and COMMS rebuilds were: it spans several screens, it has one decision that shapes the
rest, and the constraints are easier to argue about in prose than in a diff.

This is open design question 7.4 in `DESIGN-REVIEW-BRIEF.md`, now with a proposed
answer.

## The problem, in three parts

**SYSTEM mixes two audiences.** The readings page currently holds, in one place: the
node's name, whether the clock is set, GPS state, path cache occupancy — and also the
last keyboard scancode, the I²C bus addresses, the last touch coordinate, free heap,
and the boot phase timings. The first group answers "what is this device doing". The
second answers "what is this hardware doing", which is a question only whoever is
building the firmware asks. Both matter; they are not the same screen.

**The log is receive-only.** `RX LOG` records every packet heard: time, payload type,
route type and hop count, RSSI, SNR, length. Nothing records what this node *sent*. So
the most basic field question — did my advert actually go out — has no answer on the
device, and a transmit that failed before reaching the air is completely invisible.

**The radio cannot be configured on the device.** Frequency, bandwidth, spreading
factor, coding rate and transmit power are shown on the home screen in one small
centred line and are settable only from a companion app or the CLI. For a terminal
whose whole premise is standing on its own, the settings that decide whether it hears
anything at all are the wrong ones to require a phone for.

## The decision: what axis to split on

Not "settings versus readings" — the two pages already split that way and it did not
help, because the readings page is what grew.

**The axis is field versus development.** Does this help someone using the radio, or
someone building the firmware?

| Row | Field | Development |
|---|---|---|
| NODE (name) | ● | |
| CLOCK set/unset | ● | it silently empties NODES when unset |
| GPS state, satellites | ● | |
| PATH CACHE n/n + evictions | ● | explains an empty NODES |
| KEYBOARD present | | ● |
| LAST KEY raw/filtered | | ● |
| I²C BUS addresses | | ● |
| TOUCH coordinate | | ● |
| FREE HEAP | | ● |
| LAST RESET reason | | ● |
| BOOT total, SLOWEST phase, phases | | ● |
| MSGLOG write time + phases | | ● |

The split is clean, which is the argument for it. Nothing lands ambiguously except
path cache, and that one belongs on the field side because its failure mode is a
screen that looks broken.

## Proposed structure

**Page 1 — ACTIONS.** Shortened by grouping rather than by deletion. The two channel
actions become one submenu, the two display toggles become one, and three submenus are
new:

```
SEND ADVERT (zero hop)
SEND ADVERT (flood)
NODE NAME
CHANNELS        >   add, delete
RADIO           >   freq, bw, sf, cr, tx power, rx gain      (new)
ROUTING         >   path hash mode, and room to grow
DISPLAY         >   always on, day/night
SOUND
SET TIME
EVENT LOG       >
AIR LOG         >   was RX LOG
DIAGNOSTICS     >   the development rows above                (new)
```

Same twelve rows, but they are now categories with room rather than a flat list that
grows by one every time something is added. `ROUTING` exists so that `flood.max`,
`flood.max.advert` and `advert.interval` have somewhere to go later without another
reorganisation.

**Page 2 — READINGS.** Field rows only, plus two things it should have said all along:

- `CONTACTS n/350` — the denominator. The anonymous slots are pre-allocated by
  `resetContacts()` and are not available to ordinary contacts, so 350 is the real
  capacity and `getNumContacts()` already excludes them. The
  home screen says `N STORED` with nothing to measure it against. `PATH CACHE` already
  reads `16/16`, so the precedent is here.
- `RADIO` — the parameters in a readable row rather than a small centred line on the
  home screen, and next to the receive counter that says whether they are working.

**DIAGNOSTICS.** A submenu holding everything in the right column of the table above.
Nothing is deleted: this is the screen that found every hardware bug on the project,
and the reason it earns space is exactly the reason it should not be the first thing a
user reads.

## The air log

Rename to `AIR LOG` and record both directions, using hooks upstream already provides
and RIFT currently ignores: `Dispatcher::logTx(Packet*, int len)` and
`logTxFail(Packet*, int len)`. No fork divergence — they are declared virtual and
empty in `src/Dispatcher.h`.

A direction column, and the two signal columns reused rather than widened:

| Column | RX row | TX row |
|---|---|---|
| time | seconds since boot | same |
| dir | `<` | `>` |
| type | payload type | payload type |
| route | route type + hop count | route type |
| RSSI / SNR | as measured | **air time**, in the same two bytes |
| len | bytes | bytes |

`logTxFail` gets its own marker, because a send that never reached the air is currently
invisible and is a different fault from one that was not acknowledged. RIFT already
distinguishes "no ack" from "failed" in the message log for exactly this reason.

Cost: one byte per entry for direction, and air time fits in the two signal bytes as a
`uint16` — 96 entries stays under a kilobyte. The capture still formats nothing on the
packet path.

## Radio settings, and why this is the careful one

The parameters, with upstream's own bounds so the two cannot disagree
(`src/helpers/CommonCLI.cpp`):

| Parameter | Range | Applies |
|---|---|---|
| `freq` | 150.0 – 2500.0 MHz | **on reboot** |
| `bw` | 7.8 – 500.0 kHz | **on reboot** |
| `sf` | 5 – 12 | **on reboot** |
| `cr` | 5 – 8 | **on reboot** |
| `tx` power | −9 – 30 dBm | immediately |
| rx boosted gain | on / off | immediately |

**Reboot-to-apply is a safety property, not an inconvenience.** Upstream's `set radio`
and `set freq` both answer `OK - reboot to apply`, and only `set tx` calls into the
driver. So a wrong frequency cannot deafen the device while you are still looking at
it — the old parameters keep working until you choose to restart. The screen must say
so plainly rather than implying the change is live, and RIFT should honour that
contract rather than calling `setParams()` itself, which would diverge from upstream
for the sake of removing the one property that makes this safe to expose.

**The real hazard is the reboot after a bad save**, and it is genuine: RIFT disables
private key export, so a node that boots deaf on the wrong frequency cannot be
recovered by moving its identity elsewhere. Three things address it, none of which is
a warning nobody reads:

1. **Show current and pending side by side.** A row that reads `868.000 → 915.000`
   states what is about to change; one that reads `915.000` does not.
2. **A pending change is discardable and does not survive leaving the screen
   unconfirmed.** Same requirement as the one-time channel key, which had to be wiped
   on leaving SYSTEM because coming back redisplayed it.
3. **After a reboot into new parameters, the receive counter is the verdict.** Nothing
   heard since boot already reads `NOTHING HEARD SINCE BOOT` on the home screen, and
   its comment already says that points at frequency, SF or antenna rather than at a
   quiet network. That sentence becomes load-bearing the moment frequency is editable
   from the device, which is an argument for this design rather than against it.

**Not exposed:** `airtime_factor`, `rx_delay_base`, the tx delay factors, `agc.reset.interval`,
`cad` and `int.thresh`. Each is a tuning parameter whose wrong value degrades the mesh
for everyone within earshot rather than only for this node, and none has a symptom the
device can show. They stay CLI-only, which is the honest place for a setting whose
effect you cannot see.

## Constraints that carry over

- **One screen, one question.** If a submenu needs a paragraph to explain itself, the
  layout is doing too little.
- **Prose stays only where it prevents a loss.** `Write it down - not shown again`,
  `Not secret`, why Public cannot be deleted, what deleting a channel costs. A radio
  screen adds one: that a change applies on reboot.
- **Every logical item must be reachable and visible.** The action rows already grow a
  warning line under the selection, which moves the rows below — the recorded row
  positions must keep working through a submenu.
- **A pending secret or a pending destructive change is wiped on leaving.**
- **Redraw cost.** The air log already refreshes at 400ms because it is meant to be
  watched; adding transmit rows does not change that, but a radio screen showing a live
  receive counter must not.
