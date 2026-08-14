#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Decisions that were buried inside screen classes, where nothing could reach
// them. Both of these have been wrong in shipped firmware; both are pure
// functions of their arguments, so both can be tested without a T-Deck.

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
