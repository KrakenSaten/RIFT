# Signal first: RIFT home screen

Reference: [approved 320×240 mockup](01-signal-first.png). The PNG uses illustrative
values; firmware renders live readings with the existing 6×8 bitmap font and day/night palettes.
No new destination or secondary view is introduced.

The home screen retains the RIFT wordmark, mesh activity state, last RX age, raw RX
count, stored/heard contacts, RSSI/SNR, frequency/SF/power, tropo status/history,
clock/time zone/date/uptime, battery/charging indication, unread and tropo navigation
markers, and all three radio actions. USB/BLE connection text is removed only here.

The battery moves to the masthead. The graph occupies the middle band; diagnostics
and the clock sit beneath it. Actions have 100×34-pixel targets at x=4/110/216,
y=176. Up/down and Enter retain their original selection/activation behavior; a tap
selects and activates the same action. Left/right still change destination. Home
navigation uses five equal 64-pixel columns at y=216–239. Other screens retain their
existing layout and input regions.

## Receive history

Twenty one-minute bins include the current partial minute, oldest left/newest
right. Counts are RX frames, including repeats and frames not addressed to this
node, not delivered messages, unique nodes or airtime. TX and TX failures do not
increment the graph. The history is independent of the 64-entry air-log ring.

Each bin holds fourteen saturating uint16 counters (560 bytes total; +440 bytes).
Classification is read-only instrumentation in the existing raw receive hook.
It never changes the frame, parser, routing decisions, decryption or notifications.

The classifier checks protocol version, transport prefix, path encoding/bounds,
payload bounds and public envelope lengths. Encrypted envelopes must contain a
whole AES block after their public hashes and MAC. Reserved type codes 0xC–0xE,
unsupported versions and structurally malformed frames go to UNKNOWN. A supported
header with a structurally complete envelope remains its declared type even if
later signature/MAC verification or decryption fails. The chart does not claim
that these contents were authenticated or decoded. TXT also includes CLI data.
RAW_CUSTOM has no assumed application payload format.

| Header type | Code | Night graph color (RGB565) |
|---|---|---|
| Advert | 0x04 | Green `0x3E40` |
| Channel text | 0x05 | Blue `0x039C` |
| TXT (including CLI data) | 0x02 | Orange `0xFA00` |
| Request | 0x00 | Purple `0x9334` |
| Response | 0x01 | Cyan `0x069B` |
| Trace | 0x09 | Pink `0xFC36` |
| Path | 0x08 | Teal `0x0592` |
| Anonymous request | 0x07 | Red `0xF986` |
| Group data | 0x06 | Violet `0xB1BC` |
| Multipart | 0x0A | Dark teal `0x0B4C` |
| Control | 0x0B | Dark orange `0xBB00` |
| Raw custom | 0x0F | Magenta `0xD814` |
| ACK | 0x03 | Gray `0x9CD3` |
| UNKNOWN | — | White `0xFFFF` |

Day mode darkens these hues and uses black for UNKNOWN. The compact key shows
ADV, CHAN, TXT, ACK and UNKNOWN. The existing air-log screen is unchanged.

## Graph scale and limits

Bars use a shared **linear** packets/minute scale: minimum 8, raised to the next
power of two for larger peaks in the visible window. The y-axis shows that scale;
MAX reports the actual busiest-minute count, not the scale ceiling. Changing scale
can resize old bars. One packet is 3 pixels at the 8-packet scale, in a 24-pixel
plot. Integer apportionment conserves total height and gives nonempty categories a
pixel where space permits. Very small categories may disappear when the number
of categories exceeds available pixels; colors are not invented for empty bins.

Counters saturate at 65,535 per type/minute rather than wrap. Like the previous
history, the window resets on the 32-bit millis wrap (about 49.7 days). Device-only
checks still include touch accuracy, daytime color distinction and radio/display
coexistence; host tests cannot verify those.
