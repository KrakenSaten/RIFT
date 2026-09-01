#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <SHA256.h>

#include "RiftLogic.h"

// A flood scope per channel, which upstream leaves as a TODO in
// MyMesh::sendFloodScoped(const mesh::GroupChannel&, ...).
//
// Without it every channel floods under the node's one default scope, so a
// channel that should stay inside a region either crosses the whole mesh or
// forces every other channel to be regional too. The scope belongs to the
// channel, not to the node.
//
// Sparse on purpose. MAX_GROUP_CHANNELS is 40 here and a 32-byte name for each
// would be 1280 bytes of RAM for something most channels will never set. Eight
// entries covers having a few regional channels alongside the ordinary ones, and
// costs a tenth of that.
//
// The key is not stored, only the name: for a publicly-known hashtag region the
// key is SHA256 of the name, which is upstream's own rule in
// TransportKeyStore::getAutoKeyFor. Deriving it on use means the name is the only
// thing to persist, and the only thing to get right.

#define RIFT_SCOPE_SLOTS 8

// Slot and fingerprint, because a slot is a storage location and not an identity -
// the rule the message history and the unread counts already follow.
//
// Without the fingerprint: slot 2 holds #Emergency scoped to oslo, the channel is
// deleted, slot 2 later holds #Friends, and #Friends inherits a regional routing
// limit nobody set. That failure is close to undiagnosable in the field - the
// message says sent, LoRa works, the key is right, other channels work, and this
// one silently does not leave the region. And clearing it in RIFT's delete
// handler would not be enough, because CMD_SET_CHANNEL can replace a slot from
// the companion app without passing through this UI at all.
//
// The name is stored canonical, so what is persisted is what gets hashed.
struct RiftChannelScope {
  uint8_t  channel_idx;
  uint32_t channel_fp;
  char     name[RIFT_SCOPE_NAME_MAX];
};

class RiftScopeTable {
  RiftChannelScope _s[RIFT_SCOPE_SLOTS];
  int _n;

public:
  RiftScopeTable() : _n(0) { memset(_s, 0, sizeof(_s)); }

  int count() const { return _n; }
  const RiftChannelScope& at(int i) const { return _s[i]; }

  // The scope name for a channel, or NULL when it has none and the node default
  // applies. NULL rather than an empty string so a caller cannot mistake "no
  // scope" for "a scope whose name is blank" - those send differently.
  const char* nameFor(uint8_t channel_idx, uint32_t channel_fp) const {
    for (int i = 0; i < _n; i++) {
      if (_s[i].channel_idx != channel_idx) continue;
      // A fingerprint of 0 is "not recorded" and must not act as a wildcard: it
      // would match whatever channel later occupies the slot, which is the whole
      // failure this field exists to prevent.
      if (_s[i].channel_fp == 0 || channel_fp == 0) return NULL;
      if (_s[i].channel_fp != channel_fp) return NULL;   // stale: a different channel
      return _s[i].name;
    }
    return NULL;
  }

  // An empty or invalid name clears the entry, which is how a channel is put
  // back on the node default.
  // Stores the canonical form, so nothing downstream has to remember to convert.
  // A slot may hold only one scope, so this replaces rather than appends - which
  // is also what makes the loader safe to route through here.
  bool set(uint8_t channel_idx, uint32_t channel_fp, const char* name) {
    if (channel_idx >= MAX_GROUP_CHANNELS) return false;
    char canon[RIFT_SCOPE_NAME_MAX];
    if (name == NULL || !riftCanonicalRegion(name, canon, sizeof(canon))) {
      return clear(channel_idx);
    }
    if (channel_fp == 0) return false;   // no identity to bind to

    for (int i = 0; i < _n; i++) {
      if (_s[i].channel_idx == channel_idx) {
        _s[i].channel_fp = channel_fp;
        snprintf(_s[i].name, sizeof(_s[i].name), "%s", canon);
        return true;
      }
    }
    if (_n >= RIFT_SCOPE_SLOTS) return false;   // full; the caller says so
    _s[_n].channel_idx = channel_idx;
    _s[_n].channel_fp = channel_fp;
    snprintf(_s[_n].name, sizeof(_s[_n].name), "%s", canon);
    _n++;
    return true;
  }

  bool clear(uint8_t channel_idx) {
    for (int i = 0; i < _n; i++) {
      if (_s[i].channel_idx != channel_idx) continue;
      // Compacted rather than blanked in place: nameFor walks _n entries, so a
      // hole would be read as an entry for channel 0.
      memmove(&_s[i], &_s[i + 1], (size_t) (_n - i - 1) * sizeof(_s[0]));
      _n--;
      memset(&_s[_n], 0, sizeof(_s[0]));
      return true;
    }
    return false;
  }

  void reset() { _n = 0; memset(_s, 0, sizeof(_s)); }

  // No append(). The loader goes through set(), which validates the slot,
  // canonicalises the name and replaces rather than duplicating - a raw append
  // let a hand-edited or truncated settings file produce slot 250, or two entries
  // for slot 2 where nameFor found one and clear removed the other.
};

inline RiftScopeTable& riftScopes() {
  static RiftScopeTable t;
  return t;
}

// SHA256 of the name, truncated to 16 bytes - the same derivation upstream uses
// for a publicly-known region, so a RIFT node and a stock node given the same
// name arrive at the same key.
//
// Returns false when the channel has no scope, leaving dest untouched: the caller
// then falls back to the node default rather than sending under a key of zeroes,
// which is a different thing on the air.
inline bool riftScopeKeyFor(uint8_t channel_idx, uint32_t channel_fp, uint8_t dest[16]) {
  const char* name = riftScopes().nameFor(channel_idx, channel_fp);
  if (name == NULL || name[0] == 0 || dest == NULL) return false;

  // The stored name is already canonical - '#' included - which is what upstream
  // hashes. Hashing the user's text instead was the bug.
  SHA256 sha;
  sha.update(name, strlen(name));
  sha.finalize(dest, 16);
  return true;
}
