#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Decisions that were buried inside screen classes, where nothing could reach
// them. Every one of these has been wrong in shipped firmware; all are pure
// functions of their arguments, so all can be tested without a T-Deck.

// Outcome of resolving a path hash. A hash is only the first byte or three of a
// public key, so more than one node we know can legitimately match - 1 in 256 at the
// one-byte setting. Returning the first match named a node with a confidence the data
// does not support, which is why this is three states rather than a pointer or null.
#define RIFT_RESOLVE_UNIQUE      0
#define RIFT_RESOLVE_NONE      (-1)
#define RIFT_RESOLVE_AMBIGUOUS (-2)

// Deduplicated on a 7-byte prefix, the width the advert cache stores - so it is the
// longest the two identity sets can be compared on. Two different nodes agreeing over
// seven bytes is 1 in 2^56, a smaller risk than the one this reports.
#define RIFT_RESOLVE_DEDUP_LEN 7

// An accumulator rather than a function over an array, and the shape is the fix.
//
// The array form invited its caller to decide the candidate set, and the caller chose
// the nodes the screen happened to be showing - the recent advert cache, up to 96
// entries, against 358 stored contacts. So a hash unique among recently heard nodes
// resolved as UNIQUE while a stored contact it collided with was never looked at, and
// the screen named the wrong node with full confidence. The three states were careful;
// the candidate set was not.
//
// Folded one identity at a time so the walk stays with whoever owns the tables, and so
// nothing builds a 454-pointer array per hop per frame to answer a question that only
// ever needs the first match remembered.
struct RiftHashResolve {
  uint8_t first[RIFT_RESOLVE_DEDUP_LEN];
  bool have;
  bool ambiguous;
};

static inline void riftResolveBegin(RiftHashResolve* r) {
  if (r != NULL) memset(r, 0, sizeof(*r));
}

// Offers one identity. Returns true only when this became the first match, so the
// caller knows when to copy a name for it. Once ambiguous nothing changes it: a third
// match cannot make an answer less uncertain.
//
// key must be at least RIFT_RESOLVE_DEDUP_LEN long. Both callers pass a public key or
// a 7-byte prefix, and hash_len is bounded to that by the caller.
static inline bool riftResolveStep(RiftHashResolve* r, const uint8_t* hash,
                                   uint8_t hash_len, const uint8_t* key) {
  if (r == NULL || r->ambiguous) return false;
  if (hash == NULL || key == NULL || hash_len == 0) return false;
  if (memcmp(hash, key, hash_len) != 0) return false;

  if (!r->have) {
    memcpy(r->first, key, RIFT_RESOLVE_DEDUP_LEN);
    r->have = true;
    return true;
  }
  // The same node is normally in both sets, so this comparison is what stops almost
  // every known node being reported as ambiguous with itself.
  if (memcmp(r->first, key, RIFT_RESOLVE_DEDUP_LEN) != 0) r->ambiguous = true;
  return false;
}

static inline int riftResolveResult(const RiftHashResolve* r) {
  if (r == NULL) return RIFT_RESOLVE_NONE;
  if (r->ambiguous) return RIFT_RESOLVE_AMBIGUOUS;
  return r->have ? RIFT_RESOLVE_UNIQUE : RIFT_RESOLVE_NONE;
}

// How much message text actually fits in a channel send. MeshCore prepends
// "<sender>: " and then truncates the whole thing at max_text_len, so the usable
// length depends on how long this node's name is - a 20-character name costs 22
// characters of message. The composer allowed the full length regardless, and
// the history then showed text longer than what was transmitted.
static inline int riftChannelCapacity(int max_text_len, const char* sender_name) {
  int prefix = (sender_name != NULL ? (int) strlen(sender_name) : 0) + 2;  // ": "
  int cap = max_text_len - prefix;
  return (cap < 0) ? 0 : cap;
}

// ------------------------------------------------------- UTF-8 to the display

// The panel draws CP437 through a 6x8 bitmap font, so incoming UTF-8 has to be
// reduced to single bytes the font can actually show. This lived as a static
// function inside UITask.cpp, where nothing could test it, and it had already
// shipped wrong once: the fallback block was written as a character literal,
// became a two-byte multi-character constant in a UTF-8 source file, narrowed to
// 0x9B, and drew every unmappable character as a cent sign.
//
// Note that test_utf8_helpers covers upstream's mesh::validUtf8PrefixLength,
// which is a different function on a different problem.

// CP437 full block: the honest "something was here that cannot be drawn".
#define RIFT_GLYPH_BLOCK  0xDB

// o-slash and O-slash are absent from CP437 entirely. The display driver
// synthesises them from the base letter plus a stroke, keyed on these two
// values. They MUST agree with ST7789NativeDisplay.h, which defines the same
// names; the guard lets that header win when both are included, and lets this
// header stand alone in the native tests.
//
// Consequence worth knowing: 0x01 and 0x02 are CP437's two smiley faces, and
// they are spent on this. An incoming smiley cannot be mapped to them - it would
// draw an o-slash.
#ifndef RIFT_GLYPH_OSLASH
#define RIFT_GLYPH_OSLASH     0x01
#endif
#ifndef RIFT_GLYPH_OSLASH_UC
#define RIFT_GLYPH_OSLASH_UC  0x02
#endif

// Decodes one UTF-8 code point. Returns the number of bytes consumed and writes
// the code point to *cp, or returns 0 for anything malformed - a bad lead byte,
// a missing or invalid continuation byte, or a sequence that would run past the
// terminator. Callers advance one byte on 0 so a corrupt stream cannot stall.
// ------------------------------------------------------------ channel colours
//
// Four colours, and the count is a result rather than a choice. A channel's
// identity colour cannot swap between night and day mode or it stops being an
// identity, so one value has to clear contrast against both #000000 and #FFFFFF.
// Contrast is a function of luminance alone, which leaves the band L 0.175-0.183
// at 4.5:1 - narrow, and it forbids variation in lightness. Every channel colour
// is therefore about equally dark and they differ only by hue.
//
// These four are the result of sweeping all 65536 RGB565 values and keeping the
// ones that clear 4.5:1 on both fields - 1091 of them - then choosing four at
// least 45 degrees from the accent and 35 from the status green, and maximising
// the smallest gap between neighbours. That gap is 88 degrees; at a 6x8 cell with
// no antialiasing, and all of them necessarily at the same lightness, hue is the
// only thing telling them apart.
//
// The contrast has to be computed on the quantised RGB565 value, not on the
// source hex. The first version of this table was picked in 24-bit and failed:
// quantisation raises the luminance, and all six candidates dropped below 4.5:1
// against white once the panel had rounded them - one to 4.38. design/channel-
// colours.md records that, and the sweep, and why neither the accent nor the
// status green can be borrowed.
//
// Beyond four, no colour. A repeated colour is worse than none: two channels that
// look identical can be mistaken for each other, where two with no marker only
// tell you to read the name.
// A log entry's origin, reduced to the bare name of the conversation.
//
// The origin stored in the message log is a display string, not data, and it comes
// in three shapes. UITask::newMsg decorates an incoming entry with the hop count:
// "(2) name:", or "(D) name:" when the path is direct. sendToContact and
// sendToChannel record an outgoing one as "to name:". Anything matching an origin
// against configured channels or contacts has to undo all of that first.
//
// This is what the channel colours got wrong on the first attempt. The lookup was
// written believing an incoming entry stored the bare name, so it matched nothing
// and every message row drew grey while the tab strip - which goes straight from
// slot to colour without this function - was correct. Three readings of the code
// did not find it; the device printed the stored string and it was obvious.
//
// Returns false when there is no name left to compare, which covers "to :",
// decoration with nothing inside it, and a name too long for the buffer. Both mean
// "do not claim to know which conversation this is", the safe answer for a caller
// choosing an identity colour.
static inline bool riftOriginName(const char* origin, char* out, size_t out_size) {
  if (origin == NULL || out == NULL || out_size == 0) return false;
  const char* p = origin;

  // the hop marker, whatever is inside the brackets - a count, or D for direct
  if (*p == '(') {
    const char* close = strchr(p, ')');
    if (close != NULL && close[1] == ' ') p = close + 2;
  } else if (strncmp(p, "to ", 3) == 0) {
    p += 3;
  }

  // Trailing spaces as well as colons: the separator is written as ":" today, and
  // a name is never meaningfully distinguished by whitespace at its end.
  size_t len = strlen(p);
  while (len > 0 && (p[len - 1] == ':' || p[len - 1] == ' ')) len--;
  if (len == 0 || len >= out_size) return false;
  memcpy(out, p, len);
  out[len] = 0;
  return true;
}

// The origin with its hop marker removed, and nothing else touched.
//
// The marker exists because newMsg writes "(2) name:" into the stored origin, and
// COMMS now reports the hop count in the row's right-hand slot instead - so the
// prefix was the same fact twice, six pixels from the clock. It cannot simply be
// dropped at capture: the stored string is what arrived, riftOriginHops reads the
// count back out of it, and the message preview has no right-hand slot to move it
// to.
//
// "to name:" is returned unchanged. riftOriginName strips that too, because it
// wants the bare name; here the "to" is the only thing marking a row as outgoing.
static inline const char* riftOriginSkipHops(const char* origin) {
  if (origin == NULL || origin[0] != '(') return origin;
  const char* close = strchr(origin, ')');
  if (close == NULL || close[1] != ' ') return origin;
  return close + 2;
}

// The hop count out of the same decoration riftOriginName strips off. newMsg writes
// "(2) name:" for a routed message and "(D) name:" for a direct one, and until now
// that number was thrown away by every reader - so the sender line showed
// "14:52 (6) Public:", which reads as two clock times.
//
// Returns false for an outgoing entry ("to name:") and for anything without the
// marker: there is no hop count to report for a message this node sent.
static inline bool riftOriginHops(const char* origin, int* hops, bool* direct) {
  if (origin == NULL || origin[0] != '(') return false;
  const char* close = strchr(origin, ')');
  if (close == NULL || close[1] != ' ') return false;

  if (origin[1] == 'D' && close == origin + 2) {
    if (direct) *direct = true;
    if (hops) *hops = 0;
    return true;
  }
  int v = 0, digits = 0;
  for (const char* p = origin + 1; p < close; p++) {
    if (*p < '0' || *p > '9') return false;
    v = v * 10 + (*p - '0');
    if (++digits > 2) return false;      // a hop count is never three digits
  }
  if (digits == 0) return false;
  if (direct) *direct = false;
  if (hops) *hops = v;
  return true;
}

#define RIFT_CHAN_COL_NONE  0x0000

static inline uint16_t riftChannelColour(int channel_idx) {
  // Slot 0 is the public channel and deliberately gets none: it is the default
  // every node shares, and marking it would read as one of the user's own.
  //
  // Keyed on the slot, which is stable - MyMesh::removeChannel blanks a slot in
  // place rather than compacting, so deleting one channel does not recolour the
  // others.
  // Generated: tools/palette-check.py --channels. Hue is OKLab, and the separation
  // columns are OKLab dE, because the numbers that used to be here were neither -
  // the hue was HSV where the name table's was OKLab, so one constant was 65
  // degrees in this comment and 115 in that one, and the two "from accent" columns
  // disagreed by 90 for the same pair of colours.
  //
  // The dE columns say one thing worth acting on. Channel 1 and channel 2 are 0.061
  // and 0.053 from day mode's ok green, which is the closest pair anywhere in this
  // palette - closer than any two of the twelve name colours, whose worst pair is
  // 0.047 and is considered the usable floor. In day mode a channel name and the
  // "delivered" mark are nearly the same colour. The day green was moved to
  // #428610 to clear 4.5:1 on white and checked against the field it sits on, not
  // against these four.
  //
//     RGB565  rgb              blk   wht   hue  chroma  dE accent  dE night grn  dE day grn
//  1  0x73E0  rgb(115,125,  0)  4.66  4.50  115  0.128      0.263         0.221        0.061
//  2  0x0429  rgb(  0,134, 74)  4.51  4.66  154  0.136      0.341         0.222        0.053
//  3  0x631E  rgb( 99, 97,247)  4.58  4.58  278  0.218      0.391         0.453        0.357
//  4  0xD170  rgb(214, 44,132)  4.55  4.61  355  0.215      0.167         0.458        0.355
  switch (channel_idx) {
    case 1: return 0x73E0;
    case 2: return 0x0429;
    case 3: return 0x631E;
    case 4: return 0xD170;
    default: return RIFT_CHAN_COL_NONE;
  }
}

// ------------------------------------------------------------------ civil time
//
// Setting the clock by hand needs a conversion in both directions, and neither uses
// mktime()/gmtime(): those depend on a timezone database and a TZ setting that this
// firmware never establishes, so the answer would depend on state nobody set.
//
// RIFT has no timezone. Every display of a timestamp is (epoch / 3600) % 24 with no
// offset, so the clock is local time stored as an epoch. That is self-consistent -
// what you type is what you read back - and it is what the rest of the screen
// already assumes. The cost is that the value is not a true UTC epoch, which matters
// only if it were compared against another node's absolute clock.
//
// Hinnant's days-from-civil, which is exact for the whole range and has no loops.

static inline bool riftIsLeap(int y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static inline int riftDaysInMonth(int y, int m) {
  static const int D[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (m < 1 || m > 12) return 0;
  if (m == 2 && riftIsLeap(y)) return 29;
  return D[m - 1];
}

// Returns false and leaves *out alone if the date does not exist. February 30th and
// month 13 are refused rather than normalised: silently turning a typo into a
// different date is worse than saying no, because the user cannot see it happen.
static inline bool riftEpochFromCivil(int y, int mo, int d, int h, int mi, uint32_t* out) {
  if (out == NULL) return false;
  // 2020 is the low bound because it is also the "has the clock ever been set"
  // threshold used elsewhere; a year below it would read as unset the moment it was
  // written. 2099 keeps the result inside uint32_t with room to spare.
  if (y < 2020 || y > 2099) return false;
  if (mo < 1 || mo > 12) return false;
  if (d < 1 || d > riftDaysInMonth(y, mo)) return false;
  if (h < 0 || h > 23) return false;
  if (mi < 0 || mi > 59) return false;

  int yy = y;
  yy -= (mo <= 2) ? 1 : 0;                    // shift so March is month 1
  const int era = (yy >= 0 ? yy : yy - 399) / 400;
  const unsigned yoe = (unsigned) (yy - era * 400);
  const unsigned doy = (unsigned) ((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = (long) era * 146097 + (long) doe - 719468;

  *out = (uint32_t) days * 86400u + (uint32_t) h * 3600u + (uint32_t) mi * 60u;
  return true;
}

static inline void riftCivilFromEpoch(uint32_t epoch, int* y, int* mo, int* d,
                                      int* h, int* mi) {
  uint32_t secs = epoch % 86400u;
  long days = (long) (epoch / 86400u) + 719468;
  const long era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = (unsigned) (days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long yy = (long) yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned dd = doy - (153 * mp + 2) / 5 + 1;
  const unsigned mm = mp + (mp < 10 ? 3 : -9);

  if (y)  *y  = (int) (yy + (mm <= 2 ? 1 : 0));
  if (mo) *mo = (int) mm;
  if (d)  *d  = (int) dd;
  if (h)  *h  = (int) (secs / 3600u);
  if (mi) *mi = (int) ((secs % 3600u) / 60u);
}

// Parses exactly "YYYY-MM-DD HH:MM". Strict on purpose: a clock is one of the few
// things where a partially understood input should be refused rather than guessed at.
static inline bool riftParseCivil(const char* text, uint32_t* out) {
  if (text == NULL) return false;
  int v[5] = { 0, 0, 0, 0, 0 };
  // Four separators for five fields: '-', '-', ' ', ':'. The first version of this
  // had five characters in the string, so the field after the hour expected a space
  // where a colon belongs and every well-formed input was refused.
  const char* sep = "-- :";
  const int width[5] = { 4, 2, 2, 2, 2 };
  const char* p = text;
  for (int f = 0; f < 5; f++) {
    int acc = 0;
    for (int i = 0; i < width[f]; i++) {
      if (*p < '0' || *p > '9') return false;
      acc = acc * 10 + (*p - '0');
      p++;
    }
    v[f] = acc;
    if (f < 4) {
      if (*p != sep[f]) return false;
      p++;
    }
  }
  if (*p != 0) return false;      // trailing rubbish is a refusal, not a suffix
  return riftEpochFromCivil(v[0], v[1], v[2], v[3], v[4], out);
}

// ------------------------------------------------------------- packet decoding
//
// The names for a raw packet header, for the RX log. Kept here rather than beside
// the ring so they can be tested without an Arduino: the mapping is a table, and a
// table with an off-by-one is exactly the thing a test catches and a reading does
// not. Values are MeshCore's PAYLOAD_TYPE_* and ROUTE_TYPE_*; the header packs the
// route in bits 0-1 and the type in bits 2-5.
//
// Short on purpose. The log row also carries time, hops, RSSI, SNR and length, and
// at 6px a character the type has about eight to spend.
static inline const char* riftPayloadTypeName(uint8_t payload_type) {
  switch (payload_type) {
    case 0x00: return "REQ";
    case 0x01: return "RESP";
    case 0x02: return "TXT";
    case 0x03: return "ACK";
    case 0x04: return "ADVERT";
    case 0x05: return "GRP TXT";
    case 0x06: return "GRP DAT";
    case 0x07: return "ANONREQ";
    case 0x08: return "PATH";
    case 0x09: return "TRACE";
    case 0x0A: return "MULTI";
    case 0x0B: return "CONTROL";
    case 0x0F: return "RAW";
    default:   return "?";
  }
}

// F and D are flood and direct; the T prefix means the packet also carries transport
// codes. Two characters, because this column sits between two numbers.
static inline const char* riftRouteTypeName(uint8_t route_type) {
  switch (route_type & 0x03) {
    case 0x00: return "TF";
    case 0x01: return "F";
    case 0x02: return "D";
    default:   return "TD";
  }
}

static inline uint8_t riftHeaderPayloadType(uint8_t header) { return (header >> 2) & 0x0F; }
static inline uint8_t riftHeaderRouteType(uint8_t header)   { return header & 0x03; }

// Where path_len sits in a raw frame. Packet::readFrom puts four transport bytes
// between the header and path_len when the route type carries transport codes -
// TRANSPORT_FLOOD (0x00) and TRANSPORT_DIRECT (0x03) - so path_len is byte 5 for
// those and byte 1 for the other two.
//
// logRxRaw read byte 1 unconditionally, which on a transport-coded packet is the
// low byte of transport_codes[0]: the RX log showed random hop counts for TF/TD
// rows, and riftTropoUsableHops accepted any byte whose low six bits fell in
// 20..61 as a deep packet, which is two thirds of all values. Five scoped floods
// inside the window were enough for a false TROPO alert, and scoped floods are
// what RiftScopes exists to send.
static inline int riftRawPathLenIndex(uint8_t header) {
  uint8_t route = riftHeaderRouteType(header);
  return (route == 0x00 || route == 0x03) ? 5 : 1;
}

// A route, the other way round: the reply path to hand a node we hold a direct
// route to, so its answer comes back through the same repeaters.
//
// Each hop in a path is a prefix of that repeater's public key, so the way back is
// the same hashes in reverse order. Reversed in hash-size units, not bytes - at a
// two-byte hash size a byte reversal would corrupt every hop. path_len is Packet's
// encoding (size bits 6-7, count bits 0-5); OUT_PATH_UNKNOWN (0xFF) is not a
// length and is refused. Returns the number of bytes written, or -1.
//
// riftRegionsReq used to send a reply path of length zero on the reading that
// zero meant "back the way it came". The responder reads zero as a direct path
// of no hops and answers zero-hop, so only adjacent repeaters ever answered.
#ifndef MAX_PATH_SIZE
  #define MAX_PATH_SIZE 64   // MeshCore.h's value; the native tests build without it
#endif

static inline int riftReversePath(const uint8_t* path, uint8_t path_len, uint8_t* out) {
  if (path_len == 0xFF) return -1;
  // Packet::pathHashSize / pathHashCount, spelled out because this header is
  // also built for the native tests, where mesh::Packet does not exist.
  int sz = (path_len >> 6) + 1;
  int n = path_len & 63;
  if (n == 0) return 0;
  if (path == NULL || out == NULL) return -1;
  if (n * sz > MAX_PATH_SIZE) return -1;
  for (int i = 0; i < n; i++) {
    memcpy(&out[i * sz], &path[(n - 1 - i) * sz], (size_t) sz);
  }
  return n * sz;
}

// ------------------------------------------------------------ millis deadlines
//
// A plain millis() > deadline comparison is not wrap-safe. millis() wraps at
// 49.7 days, and a deadline computed as millis() + interval wraps first - so for
// the last interval before the wrap the deadline is a small number while millis()
// is still large, and every deadline reads as long past.
//
// The consequences ranged from cosmetic to not: an alert that never appeared, a
// popup that dismissed instantly, and - the one that matters - a render interval
// that would have redrawn the whole screen on every pass through the main loop,
// contending with the SX1262 on the shared HSPI bus, with no watchdog to notice
// the radio going quiet.
//
// The subtraction is unsigned, which wraps correctly, and the result is read as
// signed. Valid for any interval below 2^31 ms - 24.8 days - which every interval
// in this firmware is by a very wide margin.
static inline bool riftDue(uint32_t now, uint32_t deadline) {
  return (int32_t) (now - deadline) >= 0;
}

static inline int riftUtf8Decode(const char* s, uint32_t* cp) {
  unsigned char c = (unsigned char) s[0];
  int len;
  uint32_t v;

  if (c < 0x80)        { *cp = c; return 1; }
  else if (c < 0xC2)   return 0;                 // stray continuation byte, plus C0
                                                 // and C1, which can only ever begin
                                                 // an overlong two-byte sequence
  else if (c < 0xE0)   { len = 2; v = c & 0x1F; }
  else if (c < 0xF0)   { len = 3; v = c & 0x0F; }
  else if (c < 0xF5)   { len = 4; v = c & 0x07; }
  else                 return 0;                 // F5..FF cannot begin a code point
                                                 // at or below U+10FFFF

  for (int i = 1; i < len; i++) {
    unsigned char n = (unsigned char) s[i];
    if ((n & 0xC0) != 0x80) return 0;            // also catches the terminator
    v = (v << 6) | (n & 0x3F);
  }

  // Three classes decode arithmetically but are not valid UTF-8. Rejecting them
  // here sends them down the caller's malformed path, which draws one block - the
  // same treatment an unmappable character gets - so the screen says "something was
  // here" rather than showing nothing.
  //
  // The overlong forms are the ones with a consequence. C0 80 and E0 80 80 both
  // decode to U+0000, which riftTranslateUTF8 then drops as a control character, so
  // a sender could hide bytes from this display that every other client renders.
  // The surrogate range and anything above U+10FFFF are rejected for the reason
  // upstream's validUtf8PrefixLength rejects them: they are not characters. Those
  // two were already drawn as blocks, so this only makes the decoder agree with
  // the validator rather than changing what appears.
  if (v < (len == 2 ? 0x80u : (len == 3 ? 0x800u : 0x10000u))) return 0;
  if (v >= 0xD800u && v <= 0xDFFFu) return 0;
  if (v > 0x10FFFFu) return 0;

  *cp = v;
  return len;
}

// Code points that render as nothing in Unicode and so must not become a
// visible block. These are the reason a single emoji used to produce two or five
// squares: a heart with U+FE0F is two code points, a ZWJ family is five.
static inline bool riftIsInvisibleCodePoint(uint32_t cp) {
  if (cp >= 0x200B && cp <= 0x200F) return true;    // ZWSP, ZWNJ, ZWJ, LRM, RLM
  if (cp >= 0xFE00 && cp <= 0xFE0F) return true;    // variation selectors
  if (cp >= 0x1F3FB && cp <= 0x1F3FF) return true;  // skin tone modifiers
  if (cp >= 0xE0020 && cp <= 0xE007F) return true;  // tag characters (flags)
  return false;
}

// Two-byte Latin-1 supplement characters the font does carry. Returns 0 if this
// is not one of them.
static inline char riftNordicToCP437(uint32_t cp) {
  switch (cp) {
    case 0x00E6: return (char) 0x91;                  // ae
    case 0x00C6: return (char) 0x92;                  // AE
    case 0x00E5: return (char) 0x86;                  // a-ring
    case 0x00C5: return (char) 0x8F;                  // A-ring
    case 0x00E4: return (char) 0x84;                  // a-umlaut
    case 0x00C4: return (char) 0x8E;                  // A-umlaut
    case 0x00F6: return (char) 0x94;                  // o-umlaut
    case 0x00D6: return (char) 0x99;                  // O-umlaut
    case 0x00F8: return (char) RIFT_GLYPH_OSLASH;     // o-slash: synthesised
    case 0x00D8: return (char) RIFT_GLYPH_OSLASH_UC;  // O-slash: synthesised
  }
  return 0;
}

// Emoji with a genuine equivalent the panel can show. Returns NULL when there
// is none, and the caller falls back to a block.
//
// Every entry has to be a real synonym, not an approximation. Mapping the whole
// U+1F600 block to ":)" would put a smile where someone sent a sobbing face,
// which is a statement the device cannot support - worse than admitting the
// glyph is missing. Faces resolve to ASCII emoticons because CP437's two smiley
// slots are spent on o-slash; hearts, suits, notes, the sun and the arrows
// resolve to real glyphs.
//
// TWO CP437 SLOTS ARE UNUSABLE. Adafruit_GFX::write() special-cases 0x0A as a
// newline and 0x0D as a carriage return before it ever reaches drawChar, so
// neither is drawn. 0x0D is CP437's eighth note, and mapping notes there made
// them vanish rather than render - caught only because the heart at 0x03 worked
// on hardware and proved the low range is otherwise fine. Use 0x0E for notes.
// riftNoGlyphAt() below states the rule; there is a test that enforces it.
//
// Extending the table is safe. Guessing in it is not.
static inline bool riftNoGlyphAt(char c) {
  return c == 0x0A || c == 0x0D;
}

static inline const char* riftEmojiToText(uint32_t cp) {
  struct Entry { uint32_t lo, hi; const char* out; };
  static const Entry MAP[] = {
    { 0x00263A, 0x00263B, ":)"   },   // white/black smiling face
    { 0x01F600, 0x01F601, ":)"   },   // grinning, beaming
    { 0x01F602, 0x01F602, ":D"   },   // tears of joy
    { 0x01F603, 0x01F606, ":D"   },   // grinning variants, laughing, sweat
    { 0x01F609, 0x01F609, ";)"   },   // winking
    { 0x01F60A, 0x01F60B, ":)"   },   // smiling eyes, savouring
    { 0x01F60D, 0x01F60E, ":)"   },   // heart eyes, sunglasses
    { 0x01F641, 0x01F641, ":("   },   // slightly frowning
    { 0x01F642, 0x01F643, ":)"   },   // slightly smiling, upside-down
    { 0x01F61E, 0x01F61F, ":("   },   // disappointed, worried
    { 0x01F622, 0x01F622, ":("   },   // crying
    { 0x01F62D, 0x01F62D, ":("   },   // loudly crying
    { 0x01F923, 0x01F923, ":D"   },   // rolling on the floor

    // Typographic punctuation, which phones insert on their own. A curly
    // apostrophe drew as a block in the middle of a sender's name on the first
    // screenshot taken from the device ("Tommsen's T-Deck"), and the ASCII form
    // is not a lossy substitute - it is the same character as far as anyone
    // reading a 6x8 cell is concerned.
    { 0x002018, 0x002019, "'"    },   // left/right single quotation mark
    { 0x00201A, 0x00201A, "'"    },   // single low-9 quotation mark
    { 0x00201C, 0x00201D, "\""   },   // left/right double quotation mark
    { 0x00201E, 0x00201E, "\""   },   // double low-9 quotation mark
    { 0x002013, 0x002014, "-"    },   // en dash, em dash
    { 0x002026, 0x002026, "..."  },   // horizontal ellipsis
    { 0x0000A0, 0x0000A0, " "    },   // no-break space

    { 0x002665, 0x002665, "\x03" },   // heart suit
    { 0x002764, 0x002764, "\x03" },   // heavy black heart
    { 0x01F493, 0x01F49F, "\x03" },   // beating/sparkling/coloured hearts
    { 0x002666, 0x002666, "\x04" },   // diamond suit
    { 0x002663, 0x002663, "\x05" },   // club suit
    { 0x002660, 0x002660, "\x06" },   // spade suit
    { 0x00266A, 0x00266B, "\x0E" },   // eighth note, beamed notes - NOT 0x0D
    { 0x01F3B5, 0x01F3B6, "\x0E" },   // musical note, notes

    // Agreement and its opposite. "+1" is the chat convention for exactly what a
    // thumbs up means, so it is a synonym rather than a description.
    { 0x01F44D, 0x01F44D, "+1"   },   // thumbs up
    { 0x01F44E, 0x01F44E, "-1"   },   // thumbs down

    { 0x002705, 0x002705, "\xFB" },   // white heavy check mark -> CP437 radical
    { 0x002714, 0x002714, "\xFB" },   // heavy check mark
    { 0x00274C, 0x00274C, "x"    },   // cross mark
    { 0x002716, 0x002716, "x"    },   // heavy multiplication x
    { 0x0026A0, 0x0026A0, "!"    },   // warning sign
    { 0x002757, 0x002757, "!"    },   // heavy exclamation mark
    { 0x002753, 0x002753, "?"    },   // question mark ornament

    { 0x002600, 0x002600, "\x0F" },   // black sun with rays -> CP437 sun
    { 0x01F31E, 0x01F31E, "\x0F" },   // sun with face

    { 0x002605, 0x002606, "*"    },   // black/white star
    { 0x01F31F, 0x01F31F, "*"    },   // glowing star

    { 0x0027A1, 0x0027A1, "\x1A" },   // right arrow -> CP437 arrows
    { 0x002B05, 0x002B05, "\x1B" },   // left arrow
    { 0x002B06, 0x002B06, "\x18" },   // up arrow
    { 0x002B07, 0x002B07, "\x19" },   // down arrow
  };

  for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
    if (cp >= MAP[i].lo && cp <= MAP[i].hi) return MAP[i].out;
  }
  return NULL;
}

// UTF-8 in, CP437 out. dest_size includes the terminator.
//
// Consecutive unmappable code points collapse to a single block. A ZWJ family is
// five code points and would otherwise be five squares; three unrelated emoji in
// a row also collapse to one, which is the deliberate half of the trade - three
// blocks carry no more information than one and are far noisier to read.
static inline void riftTranslateUTF8(char* dest, const char* src, size_t dest_size) {
  if (dest == NULL || dest_size == 0) return;
  if (src == NULL) { dest[0] = 0; return; }

  size_t j = 0;
  bool last_was_block = false;

  // room() keeps every branch below from having to repeat the bounds check
  #define RIFT_XL_ROOM(n)  (j + (size_t)(n) <= dest_size - 1)

  for (size_t i = 0; src[i] != 0; ) {
    unsigned char c = (unsigned char) src[i];

    if (c >= 32 && c <= 126) {                      // ASCII, by far the common case
      if (!RIFT_XL_ROOM(1)) break;
      dest[j++] = (char) c;
      last_was_block = false;
      i++;
      continue;
    }

    uint32_t cp;
    int used = riftUtf8Decode(&src[i], &cp);
    if (used == 0) {                                // malformed: one block, one byte
      if (!RIFT_XL_ROOM(1)) break;
      if (!last_was_block) { dest[j++] = (char) RIFT_GLYPH_BLOCK; last_was_block = true; }
      i++;
      continue;
    }
    i += used;

    // A line break or a tab is a word boundary, and dropping it merged the words
    // either side: "line1\nline2" drew as line1line2. One space, and a run of
    // them - CR LF, or a blank line - is still one space.
    if (cp == '\n' || cp == '\r' || cp == '\t') {
      if (j > 0 && dest[j - 1] != ' ') {
        if (!RIFT_XL_ROOM(1)) break;
        dest[j++] = ' ';
        last_was_block = false;
      }
      continue;
    }

    // other control characters carry nothing drawable and are not worth a block
    if (cp < 32 || cp == 127) continue;

    if (riftIsInvisibleCodePoint(cp)) continue;     // emits nothing, and does not
                                                    // break a collapsing run

    char nordic = riftNordicToCP437(cp);
    if (nordic != 0) {
      if (!RIFT_XL_ROOM(1)) break;
      dest[j++] = nordic;
      last_was_block = false;
      continue;
    }

    const char* text = riftEmojiToText(cp);
    if (text != NULL) {
      size_t n = strlen(text);
      if (!RIFT_XL_ROOM(n)) break;
      memcpy(&dest[j], text, n);
      j += n;
      last_was_block = false;
      continue;
    }

    if (!RIFT_XL_ROOM(1)) break;
    if (!last_was_block) { dest[j++] = (char) RIFT_GLYPH_BLOCK; last_was_block = true; }
  }

  #undef RIFT_XL_ROOM
  dest[j] = 0;
}

// --------------------------------------------------------- Nordic characters

// What a long press on a base vowel offers. The T-Deck keyboard is a US QWERTY
// with no Nordic keys, and SYM is not free - it is a working symbol layer that
// emits the characters printed on the keys. Holding the base letter is the
// remaining trigger, and it is the one every phone keyboard already uses.
//
// UTF-8, because this goes into the compose buffer and the compose buffer is what
// goes on the air. The CP437 codes are for drawing only: o-slash displays as 0x01,
// which on the wire would be a C0 control byte that other clients may mangle or
// drop the message over.
//
// Covers Norwegian, Swedish and Danish. Ordered by how often each is wanted for
// the language this device is used in, so the first is one press away.
#define RIFT_NORDIC_MAX_VARIANTS  3

static inline int riftNordicVariants(char base, const char* out[RIFT_NORDIC_MAX_VARIANTS]) {
  if (out == NULL) return 0;
  switch (base) {
    case 'a': out[0] = "\xC3\xA6"; out[1] = "\xC3\xA5"; out[2] = "\xC3\xA4"; return 3;  // ae a-ring a-uml
    case 'A': out[0] = "\xC3\x86"; out[1] = "\xC3\x85"; out[2] = "\xC3\x84"; return 3;
    case 'o': out[0] = "\xC3\xB8"; out[1] = "\xC3\xB6"; return 2;                       // o-slash o-uml
    case 'O': out[0] = "\xC3\x98"; out[1] = "\xC3\x96"; return 2;
  }
  return 0;
}

// -------------------------------------------------------- screen transitions

// Which lifecycle hooks a screen change should fire. Both bugs this encodes were
// the same mistake made twice: a transient popup was treated as navigation. Once
// it tore the BT controller down mid-scan and panicked the device, once it wiped
// a one-time channel key while it was still being read. Each was fixed locally,
// with the same open-coded guard, at two separate call sites.
//
// It is a decision table rather than a condition inline in setCurrScreen()
// because it has been wrong in shipped firmware, and because three call sites
// used to answer it three different ways.
#define RIFT_XN_NONE   0
#define RIFT_XN_LEAVE  1
#define RIFT_XN_ENTER  2

static inline int riftScreenTransition(bool same_screen, bool from_overlay, bool to_overlay) {
  // re-selecting the current screen is not a transition; firing onLeave here
  // would wipe SYSTEM's state on a tap that changed nothing
  if (same_screen) return RIFT_XN_NONE;

  // A popup does not navigate. The screen behind it has not been left, and
  // must not be told it was - this is the case that caused both bugs.
  if (to_overlay) return RIFT_XN_NONE;

  // Dismissing a popup by navigating straight out of it. There is no screen to
  // leave: the overlay was never really "on" anything, so only the destination
  // is entered.
  //
  // Unreachable as the UI is wired today - overlays no longer become the current
  // screen at all, so nothing can transition *from* one. Kept because it is the
  // correct answer if that ever changes, and because leaving it out would make
  // the table look like it had considered only three of the four cases.
  if (from_overlay) return RIFT_XN_ENTER;

  return RIFT_XN_LEAVE | RIFT_XN_ENTER;
}

// ------------------------------------------------------------- mesh activity

// How the MESH headline decides what to say. The screen used to report the
// USB/BLE companion link, which is a different question: a standalone node with
// a busy mesh around it read STANDBY forever.
//
// "Heard" means the radio decoded bytes off the air - see MyMesh::logRxRaw(),
// which Dispatcher calls for every raw reception before any parsing. Traffic
// addressed to other nodes, and traffic that does not decrypt, both count.
// That is deliberate: the question is whether this radio is somewhere with a
// live mesh, not whether anyone is talking to us.
//
// NEVER is kept distinct from a very old QUIET because they mean different
// things in the field - nothing at all since boot points at frequency, SF or
// antenna, not at a quiet network.
#define RIFT_MESH_NEVER    0
#define RIFT_MESH_ACTIVE   1
#define RIFT_MESH_IDLE     2
#define RIFT_MESH_QUIET    3

// Thresholds. Adverts default to a few times an hour, so a minute of silence is
// normal and only a quarter of an hour starts to mean something.
#define RIFT_MESH_ACTIVE_MILLIS  (60UL * 1000)
#define RIFT_MESH_IDLE_MILLIS    (15UL * 60 * 1000)

static inline int riftMeshActivity(bool ever_heard, uint32_t millis_since) {
  if (!ever_heard) return RIFT_MESH_NEVER;
  if (millis_since < RIFT_MESH_ACTIVE_MILLIS) return RIFT_MESH_ACTIVE;
  if (millis_since < RIFT_MESH_IDLE_MILLIS) return RIFT_MESH_IDLE;
  return RIFT_MESH_QUIET;
}

// Age as a short human string: "9s", "12m", "3h", "5d". One unit only - this
// goes in a 6x8 field next to a size-3 headline, and a second unit would cost
// more width than it adds meaning. Truncates rather than rounds, so the number
// never runs ahead of the elapsed time.
//
// Needs 6 bytes. Callers pass millis() - last_rx as unsigned, which is correct
// across the millis() wrap; a gap longer than the 49.7-day wrap period would
// read as a small age instead, which is not worth code to handle.
#define RIFT_AGE_BUF_LEN  6

// Split from riftFormatAge because the conversation list has an age in seconds
// already - the difference of two RTC timestamps - and multiplying it back up to
// milliseconds would overflow a uint32 at 49.7 days, which is well inside the
// range this field is meant to print.
static inline void riftFormatAgeSecs(uint32_t secs, char* buf, size_t len) {
  if (buf == NULL || len == 0) return;

  if (secs < 60)          snprintf(buf, len, "%us", (unsigned) secs);
  else if (secs < 3600)   snprintf(buf, len, "%um", (unsigned) (secs / 60));
  else if (secs < 86400)  snprintf(buf, len, "%uh", (unsigned) (secs / 3600));
  else {
    uint32_t days = secs / 86400;
    // the field is four characters wide; past 99 days the exact count is noise
    if (days > 99) snprintf(buf, len, ">99d");
    else           snprintf(buf, len, "%ud", (unsigned) days);
  }
}

static inline void riftFormatAge(uint32_t millis_since, char* buf, size_t len) {
  riftFormatAgeSecs(millis_since / 1000, buf, len);
}

// Who can actually receive a direct message.
//
// NODES allowed ADV_TYPE_CHAT only while the COMMS picker allowed CHAT and ROOM,
// so a room you could message from one screen was refused from the other with an
// explanation that sounded authoritative. One rule, in one place, used by both.
//
// Rooms are included: a room server receives and stores messages, which is the
// whole point of it. Repeaters and sensors do not read, so a send to one goes
// nowhere and reports nothing.
//
// The values mirror AdvertDataHelpers.h so this header stays free of MeshCore and
// can be tested natively; UITask.cpp static_asserts that they still agree.
#define RIFT_ADV_NONE      0
#define RIFT_ADV_CHAT      1
#define RIFT_ADV_REPEATER  2
#define RIFT_ADV_ROOM      3
#define RIFT_ADV_SENSOR    4

static inline bool riftCanDirectMessage(uint8_t advert_type) {
  return advert_type == RIFT_ADV_CHAT || advert_type == RIFT_ADV_ROOM;
}

// What to call it on screen when it cannot. A missing affordance with no reason
// reads as a bug, and the type is information the screen does not otherwise show.
static inline const char* riftAdvertTypeName(uint8_t advert_type) {
  switch (advert_type) {
    case RIFT_ADV_CHAT:     return "chat";
    case RIFT_ADV_REPEATER: return "repeater";
    case RIFT_ADV_ROOM:     return "room";
    case RIFT_ADV_SENSOR:   return "sensor";
    default:                return "unknown";
  }
}

// When the message log may next be written.
//
// save() leaves the log dirty when it fails, and the flush condition is "dirty
// and the debounce elapsed" - which stays true once it has. A persistent SPIFFS
// failure therefore retried every loop iteration at ~553ms a go. These two are
// the decision, extracted so the backoff can be tested without a filesystem.
static inline uint32_t riftSaveBackoffMillis(uint8_t failures) {
  if (failures <= 1) return 5000u;
  if (failures == 2) return 15000u;
  return 60000u;
}

static inline bool riftShouldFlush(bool dirty, uint32_t now, uint32_t dirty_at,
                                   uint32_t debounce, uint8_t failures, uint32_t retry_at) {
  if (!dirty) return false;
  if (now - dirty_at < debounce) return false;          // burst has not settled
  if (failures > 0 && (int32_t) (now - retry_at) < 0) return false;   // backing off
  return true;
}

// ---------------------------------------------------------------- hop buckets
//
// NODES' summary row. The ranges are fixed on purpose: DIRECT | 1-2 | 3-5 | 6+.
//
// design/DESIGN-HANDOFF.md §6 is explicit that they must not adapt to the current
// network, because a column whose meaning moves is a column you cannot read. The
// four fixed columns it replaces failed the other way - measured on a live mesh,
// 13 of 16 nodes landed in `3+` with a maximum of 7 hops, so three quarters of the
// width described three nodes.
//
// Unknown is a bucket, not a number. A stored contact with no learned route
// carries path_len 0xFF, meaning "flood, route unknown". riftHopCount() masks bits
// 0-5, so decoding that yields 63 - and anything sorting or bucketing by hops then
// files the node at the far edge of the mesh. That exact mistake was built and
// reverted (2e0f0171 / bac7f804). Ask riftHopsUnknown() before decoding.
#define RIFT_PATH_UNKNOWN   0xFF

#define RIFT_HOPB_DIRECT    0
#define RIFT_HOPB_1_2       1
#define RIFT_HOPB_3_5       2
#define RIFT_HOPB_6PLUS     3
#define RIFT_HOPB_UNKNOWN   4
#define RIFT_HOPB_COUNT     5

static inline bool riftHopsUnknown(uint8_t path_len) {
  return path_len == RIFT_PATH_UNKNOWN;
}

// Negative hops means unknown, so a caller that has already decided can say so
// without a second argument.
static inline int riftHopBucket(int hops) {
  if (hops < 0)  return RIFT_HOPB_UNKNOWN;
  if (hops == 0) return RIFT_HOPB_DIRECT;
  if (hops <= 2) return RIFT_HOPB_1_2;
  if (hops <= 5) return RIFT_HOPB_3_5;
  return RIFT_HOPB_6PLUS;
}

static inline const char* riftHopBucketLabel(int bucket) {
  switch (bucket) {
    case RIFT_HOPB_DIRECT: return "DIRECT";
    case RIFT_HOPB_1_2:    return "1-2";
    case RIFT_HOPB_3_5:    return "3-5";
    case RIFT_HOPB_6PLUS:  return "6+";
    default:               return "?";
  }
}

// ------------------------------------------------------------- conversations
//
// Which conversation a message belongs to, carried on the log entry rather than
// worked out from its display string.
//
// riftOriginName() exists because the channel colours needed to answer this from an
// origin like "(2) Public:" or "to HYTTA:", and its own comment records that the
// first version was wrong, that every row drew grey, and that three readings of the
// code did not find it. Both call sites in MyMesh have the real identity in hand - a
// channel index, or the sender's public key - so the answer is stored when it is
// known instead of reconstructed later.
//
// `room` is reserved and not implemented. A room server needs a login and therefore
// a connection status, and the keep-alive that would maintain one is commented out
// upstream as something they intend to deprecate - see design/comms-redesign.md.
// Reserving the value means the third kind costs no rework when that is settled.
// A channel's identity, as a number that survives the slot being reused.
//
// A slot index is a storage location, not an identity. Delete #private-a from slot 2,
// create #private-b in slot 2, and every stored message keyed on "channel 2" appears
// to belong to the new channel. Purging on delete was the obvious fix and it is not
// sufficient: CMD_SET_CHANNEL lets a companion app overwrite any slot without RIFT's
// UI being involved at all, so the delete handler would simply never run.
//
// FNV-1a over the channel key. Not the GroupChannel::hash that setChannel() already
// derives - PATH_HASH_SIZE is 1, so that is a single byte and collides once in 256,
// which is far too often for "does this history belong to this channel". Thirty-two
// bits over a handful of configured channels is not a collision worth costing a
// SHA-256 per key construction to avoid.
//
// A hash rather than a slice of the key, so a 32-bit value sitting in a log file on
// flash is not four bytes of somebody's channel secret.
static inline uint32_t riftChannelFingerprint(const uint8_t* key, size_t len) {
  if (key == NULL || len == 0) return 0;
  uint32_t h = 2166136261u;                 // FNV offset basis
  for (size_t i = 0; i < len; i++) {
    h ^= key[i];
    h *= 16777619u;                         // FNV prime
  }
  // Zero is reserved for "no fingerprint recorded" - see riftConvSame(). A key that
  // hashes to it takes 1 instead, which is one collision pair in 2^32 rather than an
  // ambiguous sentinel.
  return h == 0 ? 1u : h;
}

#define RIFT_CONV_UNKNOWN  0
#define RIFT_CONV_CHANNEL  1
#define RIFT_CONV_DM       2
#define RIFT_CONV_ROOM     3

#define RIFT_CONV_PEER_LEN 6

struct RiftConvKey {
  uint8_t  kind;
  uint8_t  channel_idx;                   // RIFT_CONV_CHANNEL only
  uint32_t channel_fp;                    // RIFT_CONV_CHANNEL only; 0 = not recorded
  uint8_t  peer[RIFT_CONV_PEER_LEN];      // RIFT_CONV_DM only
};

static inline RiftConvKey riftConvUnknown() {
  RiftConvKey k;
  memset(&k, 0, sizeof(k));
  k.kind = RIFT_CONV_UNKNOWN;
  return k;
}

static inline RiftConvKey riftConvChannel(uint8_t channel_idx, uint32_t fingerprint) {
  RiftConvKey k;
  memset(&k, 0, sizeof(k));
  k.kind = RIFT_CONV_CHANNEL;
  k.channel_idx = channel_idx;
  k.channel_fp = fingerprint;
  return k;
}

static inline RiftConvKey riftConvDM(const uint8_t* pubkey) {
  RiftConvKey k;
  memset(&k, 0, sizeof(k));
  if (pubkey == NULL) return k;          // unknown rather than a DM with a zero peer
  k.kind = RIFT_CONV_DM;
  memcpy(k.peer, pubkey, RIFT_CONV_PEER_LEN);
  return k;
}

// Two unknowns are NOT the same conversation. Entries loaded from a pre-v2 log all
// carry unknown, and treating them as one bucket would collect a whole history into
// a single fake conversation - and, worse, make the eviction below consider it the
// largest and empty it first.
static inline bool riftConvSame(const RiftConvKey& a, const RiftConvKey& b) {
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case RIFT_CONV_CHANNEL:
      if (a.channel_idx != b.channel_idx) return false;
      // No wildcard for a missing fingerprint. Matching on the slot alone was
      // defended as "as good as it was when it was written", but it is not: when it
      // was written slot 2 was one channel, and after a delete and recreate it is
      // another - so the wildcard hands one channel's history to a different one.
      // Records without a fingerprint are now loaded as unknown conversations
      // instead, so nothing reaches here with zero; if anything ever does, it must
      // fail to match rather than match everything.
      return a.channel_fp == b.channel_fp;
    case RIFT_CONV_DM:      return memcmp(a.peer, b.peer, RIFT_CONV_PEER_LEN) == 0;
    default:                return false;
  }
}

// Which conversation is holding the most entries, for eviction.
//
// The log is one pool shared by every conversation. Dropping the globally oldest
// entry is honest while the view is a single stream, and stops being honest once it
// is filtered per conversation: a busy channel then evicts a quiet direct message,
// and the conversation you had an hour ago opens empty rather than short. Dropping
// from the largest instead means a quiet conversation survives any amount of traffic
// in a loud one.
//
// Returns the number of entries the winner holds, and writes its key to *out. Zero
// when there is nothing to evict.
static inline int riftLargestConv(const RiftConvKey* keys, int n, RiftConvKey* out) {
  if (keys == NULL || out == NULL || n <= 0) return 0;
  int best = 0;
  for (int i = 0; i < n; i++) {
    if (keys[i].kind == RIFT_CONV_UNKNOWN) continue;   // never a bucket, see above
    int c = 0;
    for (int j = 0; j < n; j++) if (riftConvSame(keys[i], keys[j])) c++;
    if (c > best) { best = c; *out = keys[i]; }
  }
  // All unknown - a log restored from a pre-v2 file and nothing new yet. Fall back to
  // the oldest, which is what the ring did before any of this existed.
  if (best == 0) { *out = riftConvUnknown(); return 0; }
  return best;
}

// ------------------------------------------------------------ settings migration
//
// The flags byte of /rift.cfg, decoded. Pure, so the migration has tests instead of
// an argument - which is what this needed, because the bug it fixes was a comment
// reasoning about the wrong thing.
//
// Bit layout: 0 day mode, 1 screen always on, 2-3 radar source, 4 sound.
//
// VERSION 1 vs 2, AND WHY THE BUMP WAS OWED. Sound arrived in v0.8 on bit 4 and the
// file stayed at version 1, on the reasoning that the low four bits were already
// spoken for so no bump was needed. That checked whether the *bit* was free. The
// question was whether *zero* means the new default - and it does not: sound defaults
// to on, so every settings file written before sound existed said "off". A fresh
// install got sound; an upgrade went silent. The rule, stated so it is not rederived:
//
//   adding a setting where the old value 0 does not equal the new default
//   requires a version bump.
//
// A version 1 file therefore takes the new default rather than its own bit 4.
//
// THE COST OF THAT, STATED PLAINLY. v0.8.0 shipped writing version 1, so a device
// where the user deliberately turned sound off has a file indistinguishable from a
// v0.7 one. Those users get sound back on once, and it sticks after that. The
// alternative is leaving every v0.7 upgrade silently muted, and silence reads as
// broken hardware where an unexpected beep reads as a setting to change. One-time
// annoyance for a few beats a permanent wrong answer for everyone.
#define RIFT_SETTINGS_VERSION 2

// Mirrored from UITask.cpp, which defines these inside #ifdef RIFT_RADAR and so
// cannot be the source for a file that compiles without a radio.
#define RIFT_SETTINGS_SRC_BOTH 0
#define RIFT_SETTINGS_SRC_WIFI 1
#define RIFT_SETTINGS_SRC_BLE  2

struct RiftSettings {
  bool    day_mode;
  bool    always_on;
  uint8_t radar_src;
  bool    sound_on;
  bool    migrated;   // the file was an older version and should be rewritten
};

// Returns false for a version this build does not know, in which case *out is
// untouched and the caller keeps its defaults - a format from the future is not
// something to guess at.
static inline bool riftDecodeSettings(uint8_t version, uint8_t flags, RiftSettings* out) {
  if (out == NULL) return false;
  if (version != 1 && version != RIFT_SETTINGS_VERSION) return false;

  out->day_mode  = (flags & 1) != 0;
  out->always_on = (flags & 2) != 0;

  uint8_t src = (uint8_t) ((flags >> 2) & 3);
  // 3 is not a state this ever writes; treat it as both rather than trusting it
  out->radar_src = (src == RIFT_SETTINGS_SRC_WIFI || src == RIFT_SETTINGS_SRC_BLE)
                     ? src : RIFT_SETTINGS_SRC_BOTH;

  if (version == 1) {
    out->sound_on = true;      // the new default, not the old zero
    out->migrated = true;
  } else {
    out->sound_on = (flags & 16) != 0;
    out->migrated = false;
  }
  return true;
}

static inline uint8_t riftEncodeSettings(const RiftSettings& s) {
  return (uint8_t) ((s.day_mode ? 1 : 0) | (s.always_on ? 2 : 0)
                    | ((s.radar_src & 3) << 2) | (s.sound_on ? 16 : 0));
}

// Which entry a full log should drop. keys are the conversations of the stored
// entries, oldest first; the return is an index into them.
//
// Oldest-first is honest while a history is shown as one stream: you see the last N
// and that is genuinely what you have. Once the view is per conversation it stops
// being honest - a busy channel evicts the direct message you had an hour ago, and
// that conversation opens empty. So drop the oldest entry of whichever conversation
// holds the most: a loud channel loses a tail it has plenty of, and a quiet DM
// survives any amount of channel traffic.
//
// Two cases fall back to plain oldest-first. A log restored from a pre-key file has
// every entry unknown, so there are no buckets to compare. And a log where no
// conversation holds more than one entry has nothing dominant to take from, where
// "largest" would only mean "whichever came first" - which is a worse rule than age
// because it is arbitrary rather than merely blunt.
// Unread, per conversation.
//
// Session-only by design: persisting it would mean another field in the settings
// file, and a dot that survives a reboot matters less than one that is correct while
// the device is on.
//
// The nav-bar dot *is* driven from here: it is any(). It used to be driven from the
// companion queue length, which lit it for messages already read on the device, and
// this became the single source of truth when that was fixed. The line that said
// otherwise outlived the change by a release.
//
// Rows are ordered by when they were last marked, oldest first, so a full table
// drops the conversation that has gone longest without a new message. Overflowing
// needs 32 conversations unread at once, and at that point every policy is arbitrary;
// keeping the most recent 32 correct is the useful end to be right about.
#define RIFT_UNREAD_MAX 32

struct RiftUnread {
  RiftConvKey keys[RIFT_UNREAD_MAX];
  uint8_t counts[RIFT_UNREAD_MAX];   // capped at 255; the dot only needs "any"
  int n = 0;

  // Arrived with no conversation identity, so there is no row to put a dot on and no
  // conversation to open in order to clear it. Counted rather than dropped: this is
  // the authoritative unread model now, and a model that silently loses a
  // notification is worse than one that admits it cannot place it. Nothing in RIFT
  // produces these today - both MyMesh paths carry identity - so a non-zero value here
  // means some caller is using the older entry point.
  uint8_t unattributable = 0;

  // True if anything at all is unread. This is what the nav dot means.
  bool any() const { return n > 0 || unattributable > 0; }

  // Saturating, because the caller only ever prints it.
  uint16_t total() const {
    uint32_t t = unattributable;
    for (int i = 0; i < n; i++) t += counts[i];
    return (uint16_t) (t > 0xFFFFu ? 0xFFFFu : t);
  }

  // Dismissing the preview is the only acknowledgement possible for something with no
  // conversation to open, so it is the only thing that clears these. It must not touch
  // the per-conversation counts: those are cleared by opening the conversation, and
  // resetting them here was half of what made two models disagree.
  void onPreviewDismissed() { unattributable = 0; }

  void mark(const RiftConvKey& k) {
    if (k.kind == RIFT_CONV_UNKNOWN) {
      if (unattributable < 255) unattributable++;
      return;
    }
    for (int i = 0; i < n; i++) {
      if (!riftConvSame(keys[i], k)) continue;
      uint8_t c = counts[i] < 255 ? counts[i] + 1 : 255;
      // Moved to the newest position, not just incremented in place. The comment
      // above describes eviction by least-recent activity, and incrementing without
      // moving meant a conversation could receive a hundred messages and still be
      // the first one dropped - the documented policy and the implemented one
      // disagreed, and the documentation was the one that was right about what is
      // wanted here.
      memmove(&keys[i], &keys[i + 1], (size_t) (n - i - 1) * sizeof(keys[0]));
      memmove(&counts[i], &counts[i + 1], (size_t) (n - i - 1) * sizeof(counts[0]));
      keys[n - 1] = k;
      counts[n - 1] = c;
      return;
    }
    if (n >= RIFT_UNREAD_MAX) {
      memmove(&keys[0], &keys[1], (size_t) (RIFT_UNREAD_MAX - 1) * sizeof(keys[0]));
      memmove(&counts[0], &counts[1], (size_t) (RIFT_UNREAD_MAX - 1) * sizeof(counts[0]));
      n = RIFT_UNREAD_MAX - 1;
    }
    keys[n] = k;
    counts[n] = 1;
    n++;
  }

  uint8_t count(const RiftConvKey& k) const {
    for (int i = 0; i < n; i++) if (riftConvSame(keys[i], k)) return counts[i];
    return 0;
  }

  // Called from render() rather than from a navigation event: a conversation is read
  // when it is on screen, which is a thing the frame knows and the navigation does
  // not - opening COMMS on Public and switching to a channel in the strip never
  // enters or leaves a screen.
  void clear(const RiftConvKey& k) {
    for (int i = 0; i < n; i++) {
      if (!riftConvSame(keys[i], k)) continue;
      memmove(&keys[i], &keys[i + 1], (size_t) (n - i - 1) * sizeof(keys[0]));
      memmove(&counts[i], &counts[i + 1], (size_t) (n - i - 1) * sizeof(counts[0]));
      n--;
      return;
    }
  }
};

// An entry with an unknown conversation goes before anything with a known one.
//
// Unknown is what a record restored from a pre-v3 log becomes: history that cannot
// prove which conversation it belongs to. riftLargestConv never counts those as a
// bucket, which was right - collecting them into one fake conversation would have
// made it the largest and emptied it first. But it also meant they were never the
// eviction target once any real conversation held two entries, so they could not
// leave: after an upgrade, 46 migrated entries sat in the 48-slot log for ever and
// every live conversation was trimmed back to two messages as soon as a third
// arrived. The test that pinned this said the migrated entries were "left alone",
// which they were, at the price of everything after them.
//
// Oldest unknown first, then the busiest conversation's oldest as before. Migrated
// history still shows until live traffic needs the room, which is the most it was
// ever going to do.
static inline int riftEvictIndex(const RiftConvKey* keys, int n) {
  if (keys == NULL || n <= 0) return 0;

  for (int i = 0; i < n; i++) {
    if (keys[i].kind == RIFT_CONV_UNKNOWN) return i;
  }

  RiftConvKey biggest = riftConvUnknown();
  if (riftLargestConv(keys, n, &biggest) <= 1) return 0;

  for (int i = 0; i < n; i++) {
    if (riftConvSame(keys[i], biggest)) return i;   // oldest of that conversation
  }
  return 0;   // unreachable: riftLargestConv only names a key it found
}

// ---- Repeater status ------------------------------------------------------
//
// A repeater answers REQ_TYPE_GET_STATUS with a four-byte tag followed by its
// RepeaterStats struct, memcpy'd straight out of the sender's memory. RIFT does
// not include the repeater's header, and that struct has grown across firmware
// versions: err_events was called n_full_events, and the last twelve bytes did
// not exist at all. An older repeater therefore answers with a shorter reply.
//
// So this decodes field by field against the length actually received, rather
// than copying into a struct of its own. A struct copy would read past a short
// reply and show an old repeater twelve bytes of whatever followed it in the
// receive buffer, presented as duplicate counts and receive airtime. The two
// growth tiers carry a flag each so the screen can print a dash instead of a
// number nobody sent.
//
// Byte order is the sender's memcpy of a packed little-endian struct, which is
// what every MeshCore target is, so each field is read back the same way.

#define RIFT_STATS_TAG_LEN   4    // response tag, ahead of the struct
#define RIFT_STATS_MIN      44    // through last_snr: sent by every version
#define RIFT_STATS_DUPS     48    // adds n_direct_dups, n_flood_dups
#define RIFT_STATS_FULL     56    // adds total_rx_air_time_secs, n_recv_errors

struct RiftRepeaterStats {
  uint16_t batt_milli_volts;
  uint16_t tx_queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t packets_recv;
  uint32_t packets_sent;
  uint32_t air_time_secs;
  uint32_t up_time_secs;
  uint32_t sent_flood;
  uint32_t sent_direct;
  uint32_t recv_flood;
  uint32_t recv_direct;
  uint16_t err_events;
  int16_t  last_snr_x4;     // SNR times four, as the repeater sends it
  uint16_t direct_dups;
  uint16_t flood_dups;
  uint32_t rx_air_time_secs;
  uint32_t recv_errors;
  bool have_dups;           // reply reached RIFT_STATS_DUPS
  bool have_rx_air;         // reply reached RIFT_STATS_FULL
};

static inline uint16_t riftRd16(const uint8_t* p) {
  return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
}

static inline uint32_t riftRd32(const uint8_t* p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8)
       | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

// `data`/`len` are the whole response as onContactResponse receives it, tag
// included, so a caller cannot get the offset wrong by forgetting to skip it.
static inline bool riftDecodeRepeaterStats(const uint8_t* data, int len,
                                          RiftRepeaterStats* out) {
  if (data == NULL || out == NULL) return false;
  if (len < RIFT_STATS_TAG_LEN + RIFT_STATS_MIN) return false;

  const uint8_t* s = data + RIFT_STATS_TAG_LEN;
  int n = len - RIFT_STATS_TAG_LEN;

  memset(out, 0, sizeof(*out));
  out->batt_milli_volts = riftRd16(s + 0);
  out->tx_queue_len     = riftRd16(s + 2);
  out->noise_floor      = (int16_t) riftRd16(s + 4);
  out->last_rssi        = (int16_t) riftRd16(s + 6);
  out->packets_recv     = riftRd32(s + 8);
  out->packets_sent     = riftRd32(s + 12);
  out->air_time_secs    = riftRd32(s + 16);
  out->up_time_secs     = riftRd32(s + 20);
  out->sent_flood       = riftRd32(s + 24);
  out->sent_direct      = riftRd32(s + 28);
  out->recv_flood       = riftRd32(s + 32);
  out->recv_direct      = riftRd32(s + 36);
  out->err_events       = riftRd16(s + 40);
  out->last_snr_x4      = (int16_t) riftRd16(s + 42);

  if (n >= RIFT_STATS_DUPS) {
    out->direct_dups = riftRd16(s + 44);
    out->flood_dups  = riftRd16(s + 46);
    out->have_dups   = true;
  }
  if (n >= RIFT_STATS_FULL) {
    out->rx_air_time_secs = riftRd32(s + 48);
    out->recv_errors      = riftRd32(s + 52);
    out->have_rx_air      = true;
  }
  return true;
}

// Uptime and airtime arrive as seconds and are read at a glance, so the unit
// that matters is the largest one present rather than a full breakdown. Written
// into a caller-owned buffer because nothing here may allocate.
static inline void riftFormatDuration(uint32_t secs, char* buf, int sz) {
  if (buf == NULL || sz < 1) return;
  if (sz < 8) { buf[0] = 0; return; }   // "999d23h" is seven plus terminator

  uint32_t mins = secs / 60;
  uint32_t hours = mins / 60;
  uint32_t days = hours / 24;
  if (days > 0) {
    snprintf(buf, (size_t) sz, "%ud%02uh", (unsigned) days, (unsigned) (hours % 24));
  } else if (hours > 0) {
    snprintf(buf, (size_t) sz, "%uh%02um", (unsigned) hours, (unsigned) (mins % 60));
  } else if (mins > 0) {
    snprintf(buf, (size_t) sz, "%um%02us", (unsigned) mins, (unsigned) (secs % 60));
  } else {
    snprintf(buf, (size_t) sz, "%us", (unsigned) secs);
  }
}

// Airtime as a share of uptime is the number that says whether a repeater is
// busy or merely switched on, and it is the one a duty-cycle limit is stated
// against. Returned in tenths of a percent to stay integer; -1 when uptime is
// zero, which is a repeater that has just booted rather than an idle one.
static inline int riftDutyTenths(uint32_t air_secs, uint32_t up_secs) {
  if (up_secs == 0) return -1;
  if (air_secs > up_secs) return -1;      // cannot be over 100%: reject, don't clamp
  return (int) ((air_secs * 1000ULL) / up_secs);
}

// ---- CLI secrets ----------------------------------------------------------
//
// The repeater CLI carries values that must not be drawn on a screen or held in
// a buffer: `password <new>`, and the config keys guest.password, prv.key and
// bridge.secret. Three of them can be read back in plaintext, and upstream's
// `password` handler confirms the change by echoing the new password - so a
// secret can reach the panel transcript without anyone having typed it there.
//
// Named tokens rather than whole-command matches, and a suffix rule alongside
// them, because password is demonstrably not the only one and the next key
// added upstream should be covered without a change here. Over-inclusive on
// purpose: redacting something harmless costs a line of display, missing a
// secret costs the secret.

#define RIFT_REDACTED "[redacted]"

static inline bool riftCliTokenIsSecret(const char* tok, int len) {
  if (tok == NULL || len <= 0) return false;
  static const char* named[] = { "password", "guest.password", "prv.key", "bridge.secret" };
  for (int i = 0; i < 4; i++) {
    int n = (int) strlen(named[i]);
    if (n == len && memcmp(tok, named[i], (size_t) n) == 0) return true;
  }
  // Anything ending in one of these reads as a secret whatever it is prefixed
  // with, which is how a key added upstream is covered without editing this.
  static const char* suffixes[] = { ".password", ".secret", ".key" };
  for (int i = 0; i < 3; i++) {
    int n = (int) strlen(suffixes[i]);
    if (len > n && memcmp(tok + len - n, suffixes[i], (size_t) n) == 0) return true;
  }
  return false;
}

// Does this command line carry a secret, or ask for one back?
static inline bool riftCliIsSecret(const char* cmd) {
  if (cmd == NULL) return false;
  const char* p = cmd;
  while (*p) {
    while (*p == ' ') p++;
    const char* start = p;
    while (*p && *p != ' ') p++;
    if (riftCliTokenIsSecret(start, (int) (p - start))) return true;
  }
  return false;
}

// Display-safe copy of an outgoing command: every token after the secret-named
// one is replaced, so `password hunter2` and `set bridge.secret abc` keep the
// part that says what was done and lose the part that must not be shown.
static inline void riftRedactCliCommand(const char* cmd, char* out, int sz) {
  if (out == NULL || sz <= 0) return;
  out[0] = 0;
  if (cmd == NULL) return;

  int w = 0;
  bool hide = false;
  const char* p = cmd;
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char* start = p;
    while (*p && *p != ' ') p++;
    int len = (int) (p - start);

    const char* piece = start;
    int piece_len = len;
    if (hide) {
      piece = RIFT_REDACTED;
      piece_len = (int) strlen(RIFT_REDACTED);
    }
    if (w > 0) {
      if (w + 1 >= sz) break;
      out[w++] = ' ';
    }
    if (w + piece_len >= sz) piece_len = sz - w - 1;
    if (piece_len <= 0) break;
    memcpy(out + w, piece, (size_t) piece_len);
    w += piece_len;
    out[w] = 0;

    if (hide) return;                                   // one placeholder, not one per token
    if (riftCliTokenIsSecret(start, len)) hide = true;   // everything after this goes
  }
  out[w] = 0;
}

// Upstream confirms a password change by echoing the new password. That reply
// arrives whether or not this device sent the command, so it is recognised by
// its own shape rather than only by what we asked for.
static inline bool riftCliReplyEchoesSecret(const char* reply) {
  if (reply == NULL) return false;
  return strncmp(reply, "password now:", 13) == 0;
}

// ---- destructive CLI commands ---------------------------------------------
//
// The rule is "a destructive command needs a second Enter", and it belongs here
// rather than in one menu. It first lived as a per-entry flag on the command
// list, which meant free text went straight past it: typing `reboot` rebooted a
// repeater on one keypress while picking Reboot from the list asked twice.
//
// Prefix matches, because these arrive with arguments. Over-inclusive again on
// purpose - a confirmation on something harmless costs one keypress.
static inline bool riftCliIsDestructive(const char* cmd) {
  if (cmd == NULL) return false;
  while (*cmd == ' ') cmd++;
  static const char* destructive[] = {
    "reboot", "clkreboot", "poweroff", "shutdown", "erase",
    "clear stats", "start ota", "clock sync", "time ",
    "password", "setperm", "set prv.key", "set guest.password",
    "set bridge.secret", "neighbor.remove", "tempradio",
  };
  for (int i = 0; i < 16; i++) {
    size_t n = strlen(destructive[i]);
    if (strncmp(cmd, destructive[i], n) == 0) return true;
  }
  return false;
}

// Is our own clock worth sending to somebody else?
//
// `clock sync` hands the repeater our timestamp, and upstream refuses to move a
// clock backwards - so a wrong value cannot be corrected with the same command.
// An unset RTC is detectable and refused here. A clock that is set but wrong in
// the forward direction is not detectable without a reference, which is why the
// confirmation shows the value: the operator can see 2031 and decline.
#define RIFT_CLOCK_MIN 1577836800u   // 2020-01-01, below which the RTC is unset
#define RIFT_CLOCK_MAX 4102444800u   // 2100-01-01

static inline bool riftClockPlausible(uint32_t epoch) {
  return epoch >= RIFT_CLOCK_MIN && epoch < RIFT_CLOCK_MAX;
}

// ---- a colour per name ----------------------------------------------------
//
// In a channel every row is the same channel, so colouring the row by channel
// tells you nothing the tab above does not already say - and it only ever
// worked in one direction anyway. An outgoing row records "to <channel>:", which
// matches a tab and gets its colour; an incoming row records the sender's name,
// which matches nothing, so every other participant was drawn in the same grey.
// A conversation between three people looked like a conversation between one.
//
// Derived from the name rather than assigned, so it is stable: the same person
// is the same colour on every device, across reboots, and after the channel list
// is edited. Random would mean somebody changes colour when the firmware
// restarts, which is worse than no colour at all.
//
// Drawn from riftChannelColour's palette rather than a new one. Those four were
// measured after RGB565 quantisation for 4.5:1 against both black and white, which
// is the work that makes them legal as text, and inventing more without repeating
// that sweep would be guessing at the only property that matters here.
//
// Four meant two names collided one time in four, which in a channel with a
// handful of people is most of the time, and a shared colour is not an identity.
// Twelve, and twelve is where this stops being worth widening: the ceiling is not
// the palette, it is whether two colours can be told apart at a glance in a 6px
// font in daylight.
//
// Colour is an aid here, not the identity - the name is written next to it - so
// the right answer to a very busy channel is not more hues.
//
// The first four are the channel colours untouched: they are in the tab strip and
// in every outgoing row. The rest come from the same sweep repeated - every RGB565
// value scored after quantisation, keeping 4.5:1 or better against black AND
// white, held away from the accent and the two ok greens because a name in alert
// red or delivery green reads as a status rather than as a person, and chosen
// greedily for maximum separation.
//
// ---- what a September review pass changed here, which is nothing but the table
//
// The numbers below are generated: tools/palette-check.py --tables prints exactly
// these lines. The two tables that used to be here disagreed with each other about
// the same constant and carried separation columns that no standard metric
// reproduces - entry 0 was given as 49 from the accent where the sRGB Euclidean
// distance is 153 and the hue gap is 81 degrees. A number that cannot be
// recomputed cannot defend or attack an entry, which is the only thing a table
// like this is for.
//
// Three findings from re-deriving it, and the useful one is the third.
//
// One figure was wrong. This table said 4.45 on black for entry 1 and the channel
// table said 4.51 for the same constant. 4.51 is right, so the entry clears the
// rule it was admitted under after all, and it is the channel table that was
// accurate. It sits on the line either way - a truncating rather than rounding
// 5-to-8-bit expansion returns 4.45 - so if margin is ever wanted here, re-sweep;
// do not nudge a shipping identity on a rounding convention.
//
// The hue guard is stated in the wrong unit. Entries 6 and 7 violate it - 4
// degrees from the night green and 24 from the accent - and both are fine, because
// hue angle at a fixed luminance says nothing about whether two colours can be
// told apart. Measured in OKLab, every entry is further from every reserved colour
// than the set's own worst internal pair (0.047), entry 7 included at 0.116. The
// guard is a cheap filter for the sweep and not a test of anything; the ordering
// it produced is fine and the degrees are not evidence.
//
// Entry 6 is nearly colourless - chroma 0.050 against 0.109 for the next lowest -
// and that is the price of covering the hue wheel rather than a mistake. It is the
// only entry between 154 and 248 degrees, a 94-degree hole, and the luminance band
// contains nothing saturated there: every value in that region that is well
// separated from the other eleven has chroma between 0.017 and 0.048. Saturated
// colours at L 0.18 exist only in blue-purple-magenta, where five entries already
// sit, and in olive-green, where the greens are. Dropping entry 6 for a chroma
// floor buys a twelfth purple or a grey. Widening the set has the same problem:
// picking twelve for maximum separation from the whole pool reaches 0.091 against
// 0.047, and nine of the twelve land between 276 and 337 degrees. It measures
// better and reads worse.
//
// The one thing here worth a second look is not in this table at all. Day mode's
// ok green is OKLab 0.053 from channel colour 2 and 0.061 from channel colour 1,
// which is the closest pair anywhere in the palette - closer than any two names.
// It arrived when the day green was moved to #428610 to clear 4.5:1 on white, and
// that change was checked against the field and not against these. In day mode a
// channel name and "delivered" are nearly the same colour.
//
//     RGB565  rgb              blk   wht   hue  chroma  accent  green  nearest
//   0 0x73E0  rgb(115,125,  0)  4.66  4.50  115  0.128      81     21    6 0.088
//   1 0x0429  rgb(  0,134, 74)  4.51  4.66  154  0.136     120     14    6 0.089
//   2 0x631E  rgb( 99, 97,247)  4.58  4.58  278  0.218     117    137    4 0.080
//   3 0xD170  rgb(214, 44,132)  4.55  4.61  355  0.215      40    141   11 0.062
//   4 0x039C  rgb(  0,113,231)  4.52  4.65  257  0.198     138    116   10 0.072
//   5 0xB81F  rgb(189,  0,255)  4.61  4.56  312  0.303      82    172    8 0.047
//   6 0x63EC  rgb( 99,125, 99)  4.64  4.52  145  0.050     110      4    0 0.088
//   7 0xE00B  rgb(231,  0, 90)  4.55  4.62   11  0.237      24    125    3 0.067
//   8 0xB1BC  rgb(181, 52,231)  4.61  4.56  314  0.257      81    174    5 0.047
//   9 0x9334  rgb(148,101,165)  4.66  4.50  317  0.109      78    176   10 0.136
//  10 0x2BD7  rgb( 41,121,189)  4.56  4.60  248  0.130     146    108    4 0.072
//  11 0xD814  rgb(222,  0,165)  4.65  4.52  344  0.257      51    152    3 0.062

#define RIFT_NAME_COLOURS 12

static inline uint16_t riftNameColourAt(int i) {
  switch (i) {
    case 0: case 1: case 2: case 3: return riftChannelColour(i + 1);
    case 4:  return 0x039C;
    case 5:  return 0xB81F;
    case 6:  return 0x63EC;
    case 7:  return 0xE00B;
    case 8:  return 0xB1BC;
    case 9:  return 0x9334;
    case 10: return 0x2BD7;
    case 11: return 0xD814;
    default: return RIFT_CHAN_COL_NONE;
  }
}

static inline uint16_t riftNameColour(const char* name) {
  if (name == NULL || name[0] == 0) return RIFT_CHAN_COL_NONE;

  // FNV-1a. Any spread would do; this one is short, has no state, and gives the
  // same answer for the same bytes on every device.
  uint32_t h = 2166136261u;
  for (const char* p = name; *p; p++) {
    h ^= (uint32_t) (uint8_t) *p;
    h *= 16777619u;
  }
  return riftNameColourAt((int) (h % RIFT_NAME_COLOURS));
}

// ---- who sent a channel message -------------------------------------------
//
// A group message goes on the air as "<sender>: <text>" - BaseChatMesh builds
// that prefix in sendGroupMessage - and the receive path hands the whole string
// to the UI while telling it the message came from the *channel*. So the row
// header named the channel, the sender sat unnoticed at the head of the body,
// and every participant in a channel was drawn in the channel's one colour.
//
// Split here rather than at capture, so the stored message stays exactly what
// arrived and this can be corrected without a migration.
//
// Returns the length of the prefix to skip, or 0 when there is none: a message
// from a node that does not add one, or a body that simply has no colon, must
// come through untouched rather than losing its first word.
//
// Two things this name is not.
//
// It is not authenticated. The prefix is ordinary text inside the encrypted
// group payload, so any member of a channel can write any name in front of their
// message. The colour and the name are a reading aid for a cooperative channel,
// not proof of who spoke - a direct message is where identity is cryptographic,
// and that is a real difference the UI should not blur.
//
// And it is not unambiguous. A node whose own name contains ": " sends
// "Ops: North: hello", and the first delimiter splits it as "Ops" saying
// "North: hello". Taking the last delimiter instead breaks the far more common
// case of a body that contains a colon. The caller resolves it where it can, by
// preferring a split that names a contact it knows - see riftChannelSenderNth -
// and falls back to the first delimiter for a sender it has never heard of.
#define RIFT_SENDER_MAX 32

static inline int riftChannelSender(const char* text, char* out, int out_sz) {
  if (out != NULL && out_sz > 0) out[0] = 0;
  if (text == NULL) return 0;

  const char* sep = strstr(text, ": ");
  if (sep == NULL) return 0;

  int n = (int) (sep - text);
  // A node name is at most 31 characters, and an empty one is not a name. Both
  // bounds matter: without the upper one, a body containing a colon halfway
  // through a sentence would be read as a very long sender.
  if (n <= 0 || n >= RIFT_SENDER_MAX) return 0;

  for (int i = 0; i < n; i++) {
    unsigned char c = (unsigned char) text[i];
    if (c < 32) return 0;    // a newline before the colon is a body, not a name
  }

  if (out != NULL && out_sz > 0) {
    int copy = n < out_sz - 1 ? n : out_sz - 1;
    memcpy(out, text, (size_t) copy);
    out[copy] = 0;
  }
  return n + 2;   // the name, the colon and the space
}

// The same split, at the nth delimiter rather than the first, so a caller that
// can recognise names is able to try the alternatives. n = 0 is riftChannelSender.
// Returns 0 once there are no more candidates.
static inline int riftChannelSenderNth(const char* text, int nth, char* out, int out_sz) {
  if (out != NULL && out_sz > 0) out[0] = 0;
  if (text == NULL || nth < 0) return 0;

  const char* p = text;
  for (int i = 0; i < nth; i++) {
    const char* sep = strstr(p, ": ");
    if (sep == NULL) return 0;
    p = sep + 2;
  }
  const char* sep = strstr(p, ": ");
  if (sep == NULL) return 0;

  int n = (int) (sep - text);   // from the start of the message, not of p
  if (n <= 0 || n >= RIFT_SENDER_MAX) return 0;
  for (int i = 0; i < n; i++) {
    if ((unsigned char) text[i] < 32) return 0;
  }
  if (out != NULL && out_sz > 0) {
    int copy = n < out_sz - 1 ? n : out_sz - 1;
    memcpy(out, text, (size_t) copy);
    out[copy] = 0;
  }
  return n + 2;
}

// ---- tropospheric opening detector ----------------------------------------
//
// During a tropo opening, clusters of the mesh that normally cannot reach each
// other become connected, and flood packets start arriving having traversed far
// more relays than the local mesh contains. A run of those is the signal.
//
// What this measures, stated plainly so it is not over-trusted: it counts
// received packets whose recorded path is unusually deep. That is a consequence
// of an opening, not the opening itself. The stronger signature is a distant node
// suddenly audible at zero hops, which needs identity rather than just the path
// byte - the counters below are exposed so that can be judged before it is built.
//
// The trap this has to avoid is specific and already cost this project once.
// path_len packs the hash size in bits 6-7 and the hop count in bits 0-5, so the
// "no path recorded" value 0xFF reads as 63 hops. A naive comparison against a
// threshold fires on every one of those, forever. Anything at the top of the
// range is therefore not a measurement and is dropped.

#ifndef RIFT_TROPO_HOPS
  #define RIFT_TROPO_HOPS 20        // hops at or above this are "deep"
#endif
#ifndef RIFT_TROPO_NEEDED
  #define RIFT_TROPO_NEEDED 5       // deep packets in a window before saying so
#endif
#ifndef RIFT_TROPO_WINDOW_MS
  #define RIFT_TROPO_WINDOW_MS (15UL * 60UL * 1000UL)
#endif
// Held for this long after the last deep packet, so one opening is one alert
// rather than a flutter as the count crosses back and forth.
#ifndef RIFT_TROPO_HOLD_MS
  #define RIFT_TROPO_HOLD_MS (20UL * 60UL * 1000UL)
#endif

// 62 rather than 63: one below the sentinel is still not a plausible mesh depth,
// and treating it as data would mean trusting a byte that is more likely damaged
// than true.
#define RIFT_TROPO_IMPLAUSIBLE 62

struct RiftTropo {
  uint32_t window_start;
  uint32_t last_deep;      // millis of the most recent deep packet
  uint16_t deep_count;     // deep packets in the current window
  uint8_t  peak_hops;      // deepest seen, deep packets only
  // The deepest path among packets BELOW the threshold - the ordinary depth of
  // this mesh.
  //
  // Without it the detector only recorded what it had already decided was
  // interesting, so there was nothing to judge the threshold against: a first
  // real reading of "1 deep, peak 21" says nothing about whether 21 hops is
  // remarkable here or a Tuesday. Twenty was a guess, and adjusting it from one
  // deep packet would have been a second guess.
  uint8_t  base_hops;
  uint16_t seen_count;     // packets with a usable path in this window
  bool     active;
  uint32_t opened_at;
};

static inline void riftTropoReset(RiftTropo* t) {
  if (t == NULL) return;
  t->window_start = 0;
  t->last_deep = 0;
  t->deep_count = 0;
  t->peak_hops = 0;
  t->base_hops = 0;
  t->seen_count = 0;
  t->active = false;
  t->opened_at = 0;
}

// True if this path byte represents a usable hop count at all.
static inline bool riftTropoUsableHops(uint8_t path_len, uint8_t* out_hops) {
  uint8_t hops = (uint8_t) (path_len & 63);
  if (hops >= RIFT_TROPO_IMPLAUSIBLE) return false;   // 0xFF and friends
  if (out_hops != NULL) *out_hops = hops;
  return true;
}

// Fold in one received packet. Returns true on the transition into an opening,
// so the caller can announce it once rather than on every packet.
static inline bool riftTropoStep(RiftTropo* t, uint32_t now, uint8_t path_len) {
  if (t == NULL) return false;

  uint8_t hops = 0;
  if (!riftTropoUsableHops(path_len, &hops)) return false;

  // A window that has run its length starts again from this packet rather than
  // being cleared on a timer nobody calls: the only thing that has to be true is
  // that the count describes a bounded stretch of time.
  //
  // Rolled here rather than inside the deep-packet branch, so the baseline
  // covers the same stretch of time the count does. It did not, when only deep
  // packets could roll the window.
  if (t->window_start == 0 || riftDue(now, t->window_start + RIFT_TROPO_WINDOW_MS)) {
    t->window_start = now;
    t->deep_count = 0;
    t->seen_count = 0;
    t->base_hops = 0;
    // The peak belongs to the window too. Left standing it described a stretch of
    // time the count no longer covered, so the readout could say "0 deep, peak 40"
    // with the 40 an hour old - a number that means something other than what it
    // sits next to.
    if (!t->active) t->peak_hops = 0;
  }

  if (t->seen_count < 0xFFFF) t->seen_count++;

  // Below the threshold this is all that happens: the packet contributes to the
  // baseline and nothing else.
  if (hops < RIFT_TROPO_HOPS) {
    if (hops > t->base_hops) t->base_hops = hops;
    return false;
  }

  t->last_deep = now;
  if (t->deep_count < 0xFFFF) t->deep_count++;
  if (hops > t->peak_hops) t->peak_hops = hops;

  if (!t->active && t->deep_count >= RIFT_TROPO_NEEDED) {
    t->active = true;
    t->opened_at = now;
    return true;
  }
  return false;
}

// Called on a timer. Returns true on the transition out of an opening.
static inline bool riftTropoTick(RiftTropo* t, uint32_t now) {
  if (t == NULL || !t->active) return false;
  if (!riftDue(now, t->last_deep + RIFT_TROPO_HOLD_MS)) return false;
  t->active = false;
  t->peak_hops = 0;
  t->deep_count = 0;
  t->window_start = 0;
  return true;
}

// ---- flood scope ----------------------------------------------------------
//
// A scope is a region: a name and a 16-byte key. For a publicly-known hashtag
// region the key is simply SHA256 of the name, so anyone who knows the name can
// derive it - that is upstream's TransportKeyStore::getAutoKeyFor.
//
// Repeaters relay a scoped flood only within their own region, so a scope is how
// a channel stays local instead of crossing the whole mesh.
//
// A scope cannot be recognised from a packet you merely overhear. The transport
// code on it is HMAC(key, payload_type || payload), so it differs for every
// packet and identifies no region; it can only be tested against a name already
// held, by recomputing the code and comparing.
//
// That is a narrow statement and it was first written as a broad one - "scopes
// cannot be discovered" - which is wrong. They can: simple_repeater answers an
// ANON_REQ_TYPE_REGIONS request with the names it floods for, so the question is
// asked rather than overheard. See MyMesh::riftRegionsReq.
#define RIFT_SCOPE_NAME_MAX 32

// Canonical region name, and the reason this function exists rather than the
// name being hashed as typed.
//
// RegionMap::getTransportKeysFor decides the key from the first character:
//
//   $name  private region  - keys come from the key store and CANNOT be derived
//   #name  hashtag region  - key is SHA256("#name")
//   name   implicit        - upstream prepends '#' and hashes SHA256("#name")
//
// So the canonical form always carries the hash. RIFT hashed whatever the user
// typed, which meant "oslo" produced SHA256("oslo") while the repeater used
// SHA256("#oslo") - a different key, with a UI that looked correctly configured.
// The way to hit it was the obvious one: MeshCore's region export strips the '#'
// before sending names back, so the name a user reads off a repeater is exactly
// the one that produced the wrong key.
//
// Character rule copied from RegionMap::is_name_char rather than invented. A
// local rule that disagreed is how a test came to assert that "North West" was a
// valid region name: internally consistent, and refused by every repeater,
// because upstream does not accept a space. That test protected the wrong
// behaviour against regression, which is worse than having no test.
//
// $private is refused. Its key is imported, not derived, and RIFT has no way to
// obtain one - offering it would mean a scope that silently reaches nobody.
static inline bool riftRegionCharValid(unsigned char c) {
  // RegionMap::is_name_char, verbatim.
  return c == '-' || c == '$' || c == '#' || (c >= '0' && c <= '9') || c >= 'A';
}

static inline bool riftCanonicalRegion(const char* name, char* out, int out_sz) {
  if (out != NULL && out_sz > 0) out[0] = 0;
  if (name == NULL || out == NULL || out_sz < 2) return false;

  const char* p = name;
  if (*p == '$') return false;      // private region: key is imported, not derived
  if (*p == '#') p++;               // already canonical; the body is what follows

  int n = 0;
  for (const char* q = p; *q; q++) {
    if (!riftRegionCharValid((unsigned char) *q)) return false;
    if (*q == '#' || *q == '$') return false;   // legal chars, but not inside a name
    n++;
  }
  if (n == 0) return false;                     // "" and "#" are not regions
  if (n + 2 > out_sz) return false;             // '#' + body + terminator

  out[0] = '#';
  memcpy(out + 1, p, (size_t) n);
  out[n + 1] = 0;
  return true;
}

// Kept as the name-level check the UI asks, now defined by the canonical form
// succeeding rather than by a second rule that could drift from it.
static inline bool riftScopeNameValid(const char* name) {
  char tmp[RIFT_SCOPE_NAME_MAX];
  return riftCanonicalRegion(name, tmp, sizeof(tmp));
}

// ---- dragging a list ------------------------------------------------------
//
// COMMS scrolls by pixel because its blocks vary from one line to six, so a step
// per message moved the view by wildly different amounts for the same gesture. A
// list of rows has no such problem: stepping is what it already does for the
// arrow keys, and matching that keeps one idea of what a step is.
//
// What a drag needs is the conversion. The touch driver reports every 25ms, so a
// slow finger delivers a pixel at a time and a fast one delivers thirty; the
// remainder has to be carried or a slow drag never moves at all.
//
// Returns whole steps and leaves the rest in residual. Sign follows the finger:
// dragging down moves earlier into view, which is a negative step.
#ifndef RIFT_DRAG_PITCH
  #define RIFT_DRAG_PITCH 16      // pixels of finger travel per row
#endif

static inline int riftDragSteps(int* residual, int dy, int pitch) {
  if (residual == NULL || pitch <= 0) return 0;
  *residual += dy;
  int steps = *residual / pitch;
  *residual -= steps * pitch;     // truncation toward zero, so the sign is kept
  return -steps;                  // finger down, list goes back
}

// Whether a release is a tap or the end of a drag, decided from how far the finger
// travelled rather than from whether a screen did anything with it.
//
// It used to be the latter: the loop suppressed the tap only when handleDrag()
// had returned true. Every cursor list returns false when the movement produced
// no step - the cursor was already on the last row, or the drag was shorter than
// a pitch - so a long drag that changed nothing arrived as a tap on release, and
// SYSTEM's handleTouch() activated whatever row the finger lifted from. Two of
// those rows are "Delete channel" and "Advert mesh".
//
// The slop is a few pixels, because a finger does not lift straight up: a real
// tap reports a pixel or two of travel and must still be a tap. Anything past
// that was a drag, whatever the screen made of it.
#ifndef RIFT_TAP_SLOP_PX
  #define RIFT_TAP_SLOP_PX 6      // up to this much travel is still a tap
#endif

static inline bool riftDragIsMove(int travel_px, int slop_px) {
  if (travel_px < 0) travel_px = -travel_px;
  return travel_px > slop_px;
}
