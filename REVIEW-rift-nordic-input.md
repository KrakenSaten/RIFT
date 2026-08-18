# Review of `rift-nordic-input` before merge

Seven commits, unmerged, unreviewed. Read against `origin/rift-tdeck`. 95 tests
pass and all five environments build, verified here.

Nothing found that should block the merge. One finding changes the shape of the
work already queued.

---

## 1. The atomic write is not atomic, and its reason does not apply

`RiftMsgLog::save()` writes a temp file, then:

```cpp
SPIFFS.remove(path);
if (!SPIFFS.rename(tmp_path, path)) { SPIFFS.remove(tmp_path); return false; }
```

The remove is required — SPIFFS `rename` will not overwrite an existing target —
but it opens a window. **Power lost between those two lines leaves neither file.**
The comment above the function says the temp-file dance is there so "a power cut
mid-write can never take the working file with it", and that is exactly what this
window does.

The stated reason is also not the operative one:

> SPIFFS is also where the private identity lives, and RIFT disables key export,
> so a corrupted filesystem costs the node identity permanently

A torn write to a temp file cannot corrupt SPIFFS. Filesystem integrity is not at
stake either way; what the dance protects is the *contents of the log file*.

And the log does not need protecting. `load()` already rejects a bad magic or
version, stops cleanly on a short read, and bounds every length before using it —
a torn log costs at most the log, which is what the weekend handover says too:
"the log is not precious, a corrupt one is discarded on load".

**So this is not a trade.** Keeping one handle open and rewriting in place with
`seek(0)` — the fix the handover proposes if the `phases` reading points at
metadata — removes a guarantee that was never delivered and was never needed. It
should be removed whichever way that reading goes. If the time turns out to be in
`write` rather than in `open`/`swap`, the swap still has to go; it just will not
be enough on its own.

Three of the four phases are metadata — create, flush, remove-and-rename — and
only `t_write` is payload. That is a well-chosen split for the question.

## 2. `HANDOFF.md` has the keyboard poll rate wrong

Section 3 says the keyboard is "polled at 100 ms". The RIFT build sets
`KEYBOARD_POLL_MILLIS=20` in `variants/lilygo_tdeck/platformio.ini`; 100 is only
the fallback in `TDeckKeyboard.h` for a build that does not define it. The 20 ms
value was chosen deliberately — at 100 ms, keystrokes were being missed.

Worth correcting because `HANDOFF.md` is the authoritative reference, and someone
looking into input latency would start from a number that is five times too high.

## 3. Trackball: correct, with one consequence not stated

Clearing all four counters on report is the right fix, and the reasoning in the
comment matches what the code does.

The consequence is that a deliberate fast direction change discards whatever the
new direction had accumulated during the previous one, so it needs a fresh full
threshold. That is almost certainly the right trade — it is the same property
that stops the drift — but it is a real behavioural change to how a quick
up-then-left flick feels, and it is not mentioned.

## 4. Minor

`t_write`, `t_close` and `t_swap` keep values from the previous successful save
when `save()` returns early on a write failure, so a failed write shows a
plausible-looking phase breakdown from the last good one. Diagnostics only, but
this screen exists to be believed.

## Read and found clean

- `load()` — header, version, truncation and length handling are all bounded, and
  zeroing the millis-based delivery fields on load is right: a message that was
  pending when the power went is something the device can no longer know about.
- The keyboard change is comments and a header note, no behaviour.
- The lifecycle work — `onEnter`/`onLeave`/`isModal`/`isOverlay` in `ui-rift`
  rather than the shared header, with the rule in `riftScreenTransition()` where
  it is tested, and overlays never becoming `curr`. That removes the guard rather
  than centralising it, which is why both recorded bugs cannot recur at a third
  call site.
