#pragma once

#include <stdint.h>
#include <string.h>

// A notification mute per channel.
//
// An arriving message does three separable things: it marks the conversation
// unread, it raises the preview popup over whatever screen is up, and it turns the
// display on. The third is the only notification this board has - there is no
// sounder and no vibration motor in this variant - and UITask argues at length
// that it should always fire, because a wake that was not needed costs a lit
// screen while a wake that was suppressed costs the message.
//
// That argument holds for a channel the user wants. It does not hold for a busy
// one they have decided is background: there the screen lights up all day and the
// popup takes whatever they were doing. So the choice is theirs per channel, and
// it is a choice rather than a heuristic - the reasoning UITask gives is exactly
// why this must not be inferred from traffic rates or link state.
//
// A mute suppresses the popup and the wake. It deliberately does NOT suppress the
// unread mark: the dot in the COMMS tab strip is then the only thing that says the
// channel had traffic, and removing it would make this "ignore" rather than
// "mute". A message arriving on a muted channel while the screen is already on
// still repaints, because mute is about not interrupting rather than not updating.
//
// Sparse, like RiftScopes, and for the same reason - but with more slots, because
// an entry here is five bytes rather than five plus a name, and muting is likely
// to be the commoner choice of the two.

#define RIFT_MUTE_SLOTS 16

// Slot and fingerprint. The reasoning is RiftScopes' and is worth reading there in
// full: a slot is a storage location and not an identity, so slot 2 muted, the
// channel deleted, and slot 2 later holding something else would silence a channel
// nobody chose to silence. Here that failure is quieter than the scope one and
// therefore worse to diagnose - nothing is misrouted, messages simply stop
// announcing themselves, and the unread dot still appears so the channel does not
// look broken.
//
// CMD_SET_CHANNEL can also replace a slot from the companion app without passing
// through the UI at all, which is why clearing on delete is not sufficient on its
// own and the fingerprint has to be checked on every read.
struct RiftChannelMute {
  uint8_t  channel_idx;
  uint32_t channel_fp;
};

class RiftMuteTable {
  RiftChannelMute _s[RIFT_MUTE_SLOTS];
  int _n;

public:
  RiftMuteTable() : _n(0) { memset(_s, 0, sizeof(_s)); }

  int count() const { return _n; }
  const RiftChannelMute& at(int i) const { return _s[i]; }

  // Whether this channel is muted. A fingerprint of 0 is "not recorded" on either
  // side and never matches, so an unidentifiable channel is treated as unmuted -
  // which is the safe direction: it announces itself, and the user can mute it
  // again. The opposite default would silence a channel by accident.
  bool isMuted(uint8_t channel_idx, uint32_t channel_fp) const {
    if (channel_fp == 0) return false;
    for (int i = 0; i < _n; i++) {
      if (_s[i].channel_idx != channel_idx) continue;
      if (_s[i].channel_fp == 0) return false;
      return _s[i].channel_fp == channel_fp;   // a different channel: not muted
    }
    return false;
  }

  // on == false clears the entry, which is what unmuting is: no entry means the
  // channel notifies, so there is nothing to store for the ordinary case.
  // A slot holds at most one entry, so this replaces rather than appends - which
  // is also what makes the settings loader safe to route through here.
  bool set(uint8_t channel_idx, uint32_t channel_fp, bool on) {
#ifdef MAX_GROUP_CHANNELS
    if (channel_idx >= MAX_GROUP_CHANNELS) return false;
#endif
    if (!on) { clear(channel_idx); return true; }
    if (channel_fp == 0) return false;   // no identity to bind the mute to

    for (int i = 0; i < _n; i++) {
      if (_s[i].channel_idx == channel_idx) {
        _s[i].channel_fp = channel_fp;   // re-bound if the slot changed hands
        return true;
      }
    }
    if (_n >= RIFT_MUTE_SLOTS) return false;   // full; the caller says so
    _s[_n].channel_idx = channel_idx;
    _s[_n].channel_fp = channel_fp;
    _n++;
    return true;
  }

  bool clear(uint8_t channel_idx) {
    for (int i = 0; i < _n; i++) {
      if (_s[i].channel_idx != channel_idx) continue;
      // Compacted rather than blanked in place: isMuted walks _n entries, so a
      // hole would read as an entry for channel 0 - which is Public.
      memmove(&_s[i], &_s[i + 1], (size_t) (_n - i - 1) * sizeof(_s[0]));
      _n--;
      memset(&_s[_n], 0, sizeof(_s[0]));
      return true;
    }
    return false;
  }

  void reset() { _n = 0; memset(_s, 0, sizeof(_s)); }

  // No append(), for RiftScopes' reason: the loader goes through set(), which
  // validates the slot and replaces rather than duplicating. A raw append let a
  // truncated settings file produce two entries for one slot, where the reader
  // found one and clear() removed the other.
};

inline RiftMuteTable& riftMutes() {
  static RiftMuteTable t;
  return t;
}
