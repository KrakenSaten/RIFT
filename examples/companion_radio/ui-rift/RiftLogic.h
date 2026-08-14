#pragma once

#include <stdint.h>
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
