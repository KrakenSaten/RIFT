#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Decisions that were buried inside screen classes, where nothing could reach
// them. Every one of these has been wrong in shipped firmware; all are pure
// functions of their arguments, so all can be tested without a T-Deck.

// A path hash is only the first byte or three of a public key, so more than one
// node we know can legitimately match it - a 1-byte hash collides once in 256.
// Returning the first match named a repeater with a confidence the data does not
// support.
#define RIFT_HASH_NONE       (-1)
#define RIFT_HASH_AMBIGUOUS  (-2)

// candidates[i] is that node's key prefix, or NULL for a slot to ignore.
// Returns the index of the single match, RIFT_HASH_NONE if there is none, or
// RIFT_HASH_AMBIGUOUS as soon as a second one is found.
static inline int riftResolveHash(const uint8_t* hash, uint8_t len,
                                  const uint8_t* const* candidates, int count) {
  if (hash == NULL || len == 0 || candidates == NULL) return RIFT_HASH_NONE;

  int found = RIFT_HASH_NONE;
  for (int i = 0; i < count; i++) {
    if (candidates[i] == NULL) continue;
    if (memcmp(hash, candidates[i], len) != 0) continue;
    if (found != RIFT_HASH_NONE) return RIFT_HASH_AMBIGUOUS;
    found = i;
  }
  return found;
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

    // control characters carry nothing drawable and are not worth a block
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

static inline void riftFormatAge(uint32_t millis_since, char* buf, size_t len) {
  if (buf == NULL || len == 0) return;

  uint32_t secs = millis_since / 1000;
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
