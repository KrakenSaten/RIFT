# COMMS redesign — conversations, not one stream

**Built, except for room servers.** Written before the code, for the same reason the
NODES rebuild was: it is large, it has one decision that shapes everything else, and
the constraints are easier to argue about in prose than in a diff. Kept as written
rather than rewritten in the past tense — the argument is the useful part, and the
commit messages record what shipped.

Three things the code decided that this document did not:

- **Where "largest conversation" stops applying.** Eviction falls back to plain age
  in two cases: a log restored from a version 1 file, where every entry is unknown,
  and a log where no conversation holds more than one entry. In the second, "largest"
  would only mean "whichever the scan reached first" — arbitrary rather than merely
  blunt.
- **Two unknown conversations are not the same conversation.** Treating them as one
  would collect a whole migrated history into a single bucket, which the rule would
  then judge the largest and empty ahead of live traffic. That is a test, not a
  comment.
- **The popup gate had to move with the filter.** `isModal()` returned true
  unconditionally because the unfiltered history was always already showing whatever
  arrived. Filtered, an idle COMMS on Public can miss a DM — so `RiftScreen` gained
  `showsConversation()`, and the question moved from the screen to the message. The
  old comment had named this exact change as the thing that would invalidate it.

## The problem

COMMS shows one history containing every message from every channel and every direct
message, interleaved. Channel colours help you tell rows apart but they do not
separate anything — a conversation is still something you reconstruct by eye. On a
mesh with any traffic this is unreadable, which is the report from use.

## What has to come first

**Every log entry must carry the identity of its conversation**, set when the message
is added and the identity is actually known: a channel index, or a contact's key
prefix. Roughly eight bytes per entry.

This is not an optimisation. Today the only way to ask which conversation a message
belongs to is `riftOriginName()` — strip the `(2) ` or `to ` decoration off a display
string and match the remainder against configured channels and contacts. That
function exists because the channel colours needed it, and its own comment records
that the first version was wrong, that every row drew grey, and that three readings
of the code did not find it. Filtering an entire screen on that mechanism means
building the redesign on the one part of it that has already failed.

With the key on the entry, filtering is exact, and the channel colours stop depending
on string matching as a side effect.

**Migration.** Bump the log file to version 2 and keep reading version 1: old entries
load with the conversation marked unknown and are placed by the existing name match,
which is exactly as good as today. Nothing is lost, and everything new is exact.

## Structure

**A conversation list is the entry point.** One row per channel and per contact you
have history with: name, colour chip for channels, time of the last message, and an
accent dot when there is something unread.

**It opens into the last conversation you were in, not into the list.** The common
case is one channel and it should still be one keypress away; the list is one
backspace behind it. A list-first design that costs a press every time to reach
Public would be worse than what it replaces.

**Direct messages are their own section in that list**, under a heading, not
interleaved with channels. That is the "distinct function" this is for, and it needs
no new level in the navigation to get it.

**The channel strip stays** inside a conversation, as the fast switch between
channels that it already is, and gains the same unread dots.

## Unread

Per conversation, cleared when you open it. Session-only to begin with: persisting it
means another field in the settings file, and a dot that survives a reboot matters
less than one that is correct while the device is on.

The nav-bar dot keeps its current meaning — anything unread anywhere.

## The decision: which message gets evicted

The log holds 48 entries **shared across every conversation**. Today that is honest,
because you see the last 48 and that is genuinely what you have. Once the view is
filtered it stops being honest: a busy Public channel can evict a direct message, and
you open a conversation you had an hour ago to find it empty.

Three answers, and the middle one is not enough on its own:

- **Evict from the largest conversation, not the oldest overall.** When the log is
  full, drop the oldest entry belonging to whichever conversation holds the most. A
  quiet DM then survives any amount of channel traffic. It is a small change to
  `add()` and it solves the problem rather than moving it. **This is the
  recommendation.**
- **Raise the log.** 128 entries costs about 20 KB of RAM — 56% to roughly 62% — and
  a larger flash write on every save. It helps, but a loud channel still dominates
  the space.
- **Accept it.** Free, and discovered by the user as an empty conversation.

## Room servers: the status is the reason to wait

A room server is not another conversation kind with a name. It is one you have to be
**logged in** to, over the radio, before you can post — so a connect/connected status
is exactly the right thing to ask for, and it is why this waits.

The plumbing looks complete. `BaseChatMesh::sendLogin()` is public,
`hasConnectionTo()` answers the question directly, `startConnection()` records a
session with a keep-alive interval taken from the server's own login response, and
`checkConnections()` sends the pings.

Except that last call is commented out in `examples/companion_radio/MyMesh.cpp`:

```
//  checkConnections();    // TODO - deprecate the 'Connections' stuff
```

So keep-alives are never sent. `startConnection()` would fill in the table and
`hasConnectionTo()` would answer yes, while the server at the other end timed the
session out — a status that reads "connected" when it is not, which is worse than no
status at all. And upstream has marked the whole Connections mechanism for
deprecation, so enabling the pump here means diverging from a mechanism its
maintainers intend to remove.

**So the conversation kind is an enum with `room` reserved and unimplemented.** The
structure above accommodates a third kind without rework. Before building it, one of
these has to be true: upstream says what replaces Connections, or RIFT owns the
keep-alive deliberately and accepts the divergence, or rooms are done with a login
per action and no persistent status at all — honest, but probably not what a room is
for.

A second thing to settle first: a room password is a secret on screen. That is the
same class as the one-time channel key, which had to be wiped on leaving SYSTEM
because coming back redisplayed it. Any password field inherits that requirement.

## Constraints that are not negotiable

- **320×240 with a 6×8 cell.** A conversation row is one line. Name, time and dot fit;
  a preview of the message text does not, and adding one would cost the row count
  that makes the list useful.
- **The accent means one thing at a time.** It is already "active tab", "your own
  message" and "you can act here". An unread dot is a fourth use, and it works only
  because it is a 3×3 mark rather than text — shape, not colour, per the palette rules.
- **Four channel colours, and no more.** `design/channel-colours.md` records the
  sweep: 1091 of 65536 RGB565 values clear 4.5:1 on both black and white, and four is
  what remains after keeping 45° from the accent and 35° from the status green. A
  fifth channel gets no colour, and a repeated colour is worse than none.
- **The log is written to flash.** Anything that increases the entry size or the entry
  count increases what a save costs, and a save blocks the main loop, which starves
  the radio. Measure before raising either.
