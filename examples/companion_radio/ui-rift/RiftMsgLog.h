#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#if defined(ESP32)
  #include <SPIFFS.h>               // File and SPIFFS, for the two generations
#endif

#include <helpers/BaseChatMesh.h>   // MAX_TEXT_LEN, which is what one entry holds
#include <helpers/TxtDataHelpers.h> // StrHelper::strncpy

#include "RiftLogic.h"      // RiftConvKey, riftEvictIndex - the policy, with tests
#include "RiftEventLog.h"   // riftLogf, for the save timings and the retry lines

// The message history: what is held, how it is written, and what was delivered.
//
// Lifted out of UITask.cpp whole. That file was 10,989 lines and held the screens,
// this store, the RADAR scan and most of the coordination between them, which made
// every one of them harder to read than it is. Nothing here was edited on the way
// across - the point of the first extraction is that it can be checked by diffing,
// not by reasoning about it.
//
// It owns storage and says nothing about display: no DisplayDriver, no palette, no
// the_mesh. That is what made it the first piece to move.
//
// It does not own the unread state, which is RiftUnread in RiftLogic.h, nor the
// drafts, which belong to the COMMS screen. Those are the conversation model the
// review names as a third owner, and they are not this one.

// How many messages the history holds, in RAM and in the file.
//
// 264 bytes an entry, measured by building at 48 and at 96 and taking the
// difference - 12,672 bytes for 48 messages. Most of that is Entry's msg[161] and
// origin[62]; the rest is the parallel RiftConvKey array the unread tracker keeps,
// which is sized by this same constant. Unlike RIFT_PICKER_MAX below, this one is
// in .bss because msg_log is a file static, so the cost does show in the build's
// RAM figure: 60.9% at 48 against 64.8% at 96.
//
// Raised from 48, which was chosen before the device had been used daily and turned
// out to be about half a day of a busy Public channel.
//
// What does NOT scale is the save. Measured on the device across a day of real
// traffic: a save of 48 entries costs 140-368ms, of which the write is 1-2ms and
// the rest is SPIFFS.open() truncating plus the close. Doubling the content doubles
// the 1-2ms and leaves the rest where it is, so this number is bounded by RAM
// rather than by how long the main loop blocks - which is not what was expected
// before it was measured.
//
// 255 is the ceiling without a format change: the file header carries the count in
// a single byte. Growing and shrinking are both safe below that, because load()
// clamps to this value - an older file loads whole, and a larger one loses its
// oldest entries rather than failing.
#define RIFT_MSG_LOG_SIZE  96

// Where the message history lives, and how long a burst is allowed to settle
// before it is written.
//
// 20 seconds coalesces a conversation into one write while keeping the loss
// window on a power cut to something a user would describe as "the last thing I
// said" rather than "this evening". A clean shutdown flushes immediately, so the
// window only applies to power being pulled.
// The single file every build before generations wrote. No longer written, and
// deliberately not deleted once a generation exists: load() falls back to it when
// neither generation is usable, so it stays on as a third and older copy for the
// one case that would want it. A few KB of SPIFFS is a fair price for that.
#define RIFT_MSGLOG_PATH         "/rift_msgs.dat"
// no longer written; kept so a stale one from an older build can be removed
#define RIFT_MSGLOG_TMP          "/rift_msgs.new"
// The two generations. A save writes whichever one is not current; the reader takes
// the newer of the two that verify. See RiftLogic.h for why there are two.
#define RIFT_MSGLOG_GEN0         "/rift_msgs.0"
#define RIFT_MSGLOG_GEN1         "/rift_msgs.1"
#define RIFT_MSGLOG_FLUSH_MILLIS 20000
// The ceiling on how long the log may stay unwritten, whatever the traffic does.
// Chosen from the cost measured on the device rather than picked: at the full 48
// entries a save took 251ms and 380ms on two occasions in one session (SYSTEM's
// event log records any save over 50ms), so 380ms is the figure to budget, and it
// is 380ms with the SPI bus held away from the LoRa radio and no watchdog to catch
// an overrun. At 120s that is 0.32% of the time under sustained traffic, against
// 0.63% at 60s - and the exposure it bounds is two minutes of messages rather than
// one. Two minutes is an acceptable loss on a power cut; doubling the radio's
// blackout rate to halve it is not an obvious trade, so this takes the upper end of
// the 60-120s range the review suggested. The debounce above still decides the
// common case, where a burst ends and nothing has to wait for this at all.
#define RIFT_MSGLOG_MAX_UNSAVED_MILLIS 120000

// Shared in-memory message log. MeshCore keeps no message history of its own
// (DataStore holds identity/prefs/contacts/channels only, and MyMesh's offline
// queue is private raw protocol frames), so the UI owns this - same approach as
// ui-new, just shared between the popup and the COMMS terminal.

struct RiftMsgLog {
  struct Entry {
    uint32_t timestamp;
    char origin[62];
    // MAX_TEXT_LEN, not a round number: 78 silently truncated anything longer,
    // and MeshCore allows up to 160 characters
    char msg[MAX_TEXT_LEN + 1];
    bool outgoing;
    // delivery tracking, only meaningful for outgoing direct messages.
    // expected_ack == 0 means "no ACK possible" (channel sends, incoming) and
    // renders no delivery state at all.
    uint32_t expected_ack;
    uint32_t sent_at_ms;
    uint32_t timeout_ms;
    uint32_t trip_ms;
    bool delivered;
    // Not persisted. save() writes outgoing and delivered into its flags byte and
    // deliberately drops sent_at_ms and timeout_ms - see load() - so after a reboot
    // there is no deadline left to have passed. This only records whether the line
    // has already been written, within one session.
    bool timeout_logged;
    // Which conversation this belongs to, recorded where it is known rather than
    // recovered from origin[] afterwards. Eight bytes an entry, 384 for the log.
    RiftConvKey conv;
  };

  // Oldest at 0, newest at count-1. This was a ring with a head index, which is the
  // right shape when the only thing ever removed is the oldest. Per-conversation
  // eviction removes an entry from the middle, and a ring cannot do that without
  // moving head or leaving a hole that every reader then has to skip. A linear array
  // makes eviction one memmove and peek() arithmetic rather than modular. The cost
  // is moving up to 47 entries on a full log, once per message.
  Entry entries[RIFT_MSG_LOG_SIZE];
  int count = 0;

  // Which entry to drop when the log is full. The rule itself is riftEvictIndex() in
  // RiftLogic.h, so it is tested without a filesystem or a display; this only lifts
  // the keys out of the entries for it.
  int evictIndex() const {
    RiftConvKey keys[RIFT_MSG_LOG_SIZE];
    for (int i = 0; i < count; i++) keys[i] = entries[i].conv;
    return riftEvictIndex(keys, count);
  }

  Entry* add(uint32_t timestamp, const RiftConvKey& conv, const char* origin,
             const char* msg, bool outgoing) {
    markDirty();
    if (count >= RIFT_MSG_LOG_SIZE) {
      int drop = evictIndex();
      memmove(&entries[drop], &entries[drop + 1],
              (size_t) (RIFT_MSG_LOG_SIZE - drop - 1) * sizeof(Entry));
      count = RIFT_MSG_LOG_SIZE - 1;
    }

    Entry* p = &entries[count++];
    p->conv = conv;
    p->timestamp = timestamp;
    StrHelper::strncpy(p->origin, origin, sizeof(p->origin));
    StrHelper::strncpy(p->msg, msg, sizeof(p->msg));
    p->outgoing = outgoing;
    p->expected_ack = 0;
    p->sent_at_ms = 0;
    p->timeout_ms = 0;
    p->trip_ms = 0;
    p->delivered = false;
    p->timeout_logged = false;
    return p;
  }

  // The worst silent failure this firmware has. A direct message that never lands
  // looks exactly like one that did until the delivery label changes from "..." to
  // "no ack", and nothing recorded the moment it changed - so unless the user
  // happened to be looking at that row when it flipped, an undelivered message left
  // no trace at all. Swept rather than scheduled, because the deadline is per
  // message and is estimated at send time.
  void logTimeouts() {
    for (int i = 0; i < count; i++) {
      Entry* p = &entries[count - 1 - i];
      if (!p->outgoing || p->expected_ack == 0 || p->delivered) continue;
      if (p->timeout_logged || p->timeout_ms == 0) continue;
      // subtraction, matching deliveryLabel: sent_at + timeout would overflow at
      // the millis wrap and report a fresh send as long since timed out
      if ((uint32_t) millis() - p->sent_at_ms <= p->timeout_ms) continue;
      p->timeout_logged = true;
      char who[40];
      // "no ack" rather than "FAILED": the message may well have arrived and the
      // acknowledgement been lost, and the log should not claim more than it knows
      riftLogf("no ack from %s (%us)",
               riftOriginName(p->origin, who, sizeof(who)) ? who : "?",
               (unsigned) (p->timeout_ms / 1000u));
    }
  }

  // mark the pending outgoing message matching this ACK hash as delivered
  void markDelivered(uint32_t ack_hash, uint32_t trip_ms) {
    if (ack_hash == 0) return;
    for (int i = 0; i < count; i++) {
      Entry* p = &entries[count - 1 - i];
      if (p->expected_ack == ack_hash && !p->delivered) {
        p->delivered = true;
        p->trip_ms = trip_ms;
        // named here rather than in msgDelivered(), which has the round trip but
        // not the recipient
        char who[40];
        riftLogf("ack %s %ums",
                 riftOriginName(p->origin, who, sizeof(who)) ? who : "?",
                 (unsigned) trip_ms);
        markDirty();
        return;
      }
    }
  }

  // Everything belonging to one conversation, removed.
  //
  // The fingerprint already stops deleted-channel history being misattributed, so this
  // is not what makes the fix correct - it is what stops dead history occupying slots
  // in a 48-entry log that live conversations need. Called when a channel is deleted
  // from this device; a companion app overwriting a slot behind our back is handled by
  // the fingerprint instead, which is why that had to be the primary mechanism.
  // Drop everything, without writing anything.
  //
  // For the factory reset, and the "without writing" is the whole point: dirty is
  // cleared rather than left for the flush, so the timer in UITask::loop and the
  // save in UITask::shutdown both find nothing to do. A log saved after the format
  // would put the file straight back onto the filesystem that had just erased it.
  void clearAll() {
    count = 0;
    dirty = false;
  }

  int purgeConversation(const RiftConvKey& k) {
    int removed = 0;
    for (int i = 0; i < count; ) {
      if (!riftConvSame(entries[i].conv, k)) { i++; continue; }
      memmove(&entries[i], &entries[i + 1], (size_t) (count - i - 1) * sizeof(Entry));
      count--;
      removed++;
    }
    if (removed > 0) markDirty();
    return removed;
  }

  // 0 = newest, 1 = next older, ...
  const Entry* peek(int back) const {
    if (back < 0 || back >= count) return NULL;
    return &entries[count - 1 - back];
  }

  // How many bytes follow the kind byte in a stored record. One place, because a
  // writer and reader disagreeing about it would shift every field after it.
  static uint8_t convPayloadLen(uint8_t kind) {
    if (kind == RIFT_CONV_CHANNEL) return 5;   // slot + 4-byte fingerprint
    if (kind == RIFT_CONV_DM) return RIFT_CONV_PEER_LEN;
    return 0;
  }

  // ------------------------------------------------------------- persistence
  //
  // MeshCore stores no messages, so losing the log on every reboot was the
  // largest gap a field user actually noticed.
  //
  // Three things shape how this is written, and the first is not about wear:
  //
  // SPIFFS is also where the private identity lives, and RIFT disables key
  // export, so a corrupted filesystem costs the node identity permanently with
  // no way to recover it. This used to be the argument for writing to a
  // temporary file and renaming it over the real one. It was the wrong
  // conclusion twice over: the swap was not atomic, because SPIFFS.remove and
  // SPIFFS.rename are two operations with a window between them in which no log
  // exists at all - and guarding a filesystem against a power cut by performing
  // five write operations instead of two increases the exposure it was supposed
  // to reduce. The log is written in place, and load() is what makes that safe:
  // it keeps whatever records arrived and discards the rest, so an interrupted
  // write costs the newest messages rather than the file.
  //
  // It is debounced rather than written per message: a conversation arrives as a
  // burst, and coalescing a burst into one write is most of the wear saving
  // available. The cost is a loss window, stated plainly in RIFT_MSGLOG_FLUSH.
  //
  // And it blocks. There is no watchdog on the main loop and a blocking call
  // silently starves the radio, so the duration is measured rather than assumed
  // and shown on SYSTEM as `msglog:`.

  bool dirty = false;
  unsigned long dirty_at = 0;   // when, so the write can wait for the burst to end
  unsigned long first_dirty_at = 0;   // and when it first went dirty, for the deadline
  uint32_t last_save_ms = 0;    // how long the last write took, for SYSTEM
  // Phase breakdown - open / write / close. The total came out at 553ms for four
  // messages, which is about 200 bytes: far too little data for the volume to be
  // the cause, and long enough to starve LoRa. The cost was the number of
  // operations. Kept on SYSTEM because it is the only evidence of whether
  // removing four of them was enough.
  uint32_t t_open = 0, t_write = 0, t_close = 0;

  // dirty_at moves on every change; first_dirty_at is set only on the clean-to-dirty
  // transition, because the deadline measures from the oldest unsaved change and not
  // from the newest. The three places that clear dirty leave it stale on purpose - it
  // is read only while dirty, and the next transition sets it again.
  void markDirty() {
    if (!dirty) first_dirty_at = millis();
    dirty = true;
    dirty_at = millis();
  }

  // Layout: "RMSG", version, count, then records oldest-first. Strings are
  // length-prefixed rather than fixed - a typical message is a fraction of the
  // 160-byte maximum, and the file is ~3KB instead of ~12KB because of it.
  //
  // Version 2 adds the conversation key. Version 1 is still read: its entries load
  // with the conversation unknown, which places them by the existing name match and
  // is exactly as good as it was before. A history is not worth discarding to save
  // one branch in the reader.
  // Version 3 adds the channel fingerprint. Versions 1 and 2 are still read.
  //
  // A version 2 channel entry carries a slot and no fingerprint, so it cannot prove
  // which channel it belonged to - the whole point of the field. It loads with the
  // fingerprint left at zero, which riftConvSame() treats as "matches on the slot
  // alone": exactly as good as it was when it was written, and no better. For an
  // unchanged channel the behaviour is identical; for a reused slot it is the old bug,
  // confined to entries that predate the fix and age out of the log.
  //
  // Computing the fingerprint from the channel currently in the slot was the tempting
  // alternative and is the one thing that must not be done: on a reused slot it would
  // stamp old history with the new channel's identity and make the misattribution
  // permanent.
  static const uint8_t FILE_VERSION = 3;
  static const uint8_t FILE_V1_FIXED = 11;   // record header before the key existed

  // Retry accounting. save() leaves dirty set when it fails, and the flush
  // condition is "dirty and the debounce has elapsed" - which stays true forever
  // once it has. A persistent SPIFFS failure therefore retried on every single
  // loop iteration, at ~553ms a go, which is a device that does nothing but
  // hammer flash and starve the radio. Back off instead, and show the count.
  uint8_t save_failures = 0;
  unsigned long retry_at = 0;
  // Which generation is on the flash and what its sequence number is. -1 and 0 mean
  // none has been written yet, which is also the state after a migration from the
  // legacy single file, so the first save lands in slot 0.
  int8_t   gen_slot = -1;
  uint32_t gen_seq = 0;

  static const char* genPath(int slot) {
    return slot == 0 ? RIFT_MSGLOG_GEN0 : RIFT_MSGLOG_GEN1;
  }

  // both in RiftLogic.h, so the backoff is tested without a filesystem
  bool dueToSave(unsigned long now) const {
    return riftShouldFlush(dirty, (uint32_t) now, (uint32_t) dirty_at,
                           RIFT_MSGLOG_FLUSH_MILLIS, save_failures, (uint32_t) retry_at,
                           (uint32_t) first_dirty_at, RIFT_MSGLOG_MAX_UNSAVED_MILLIS);
  }

  bool save() {
    // Whichever generation is not the current one. That is the whole of the fix:
    // SPIFFS.open(path, "w") empties its target before the new copy exists, so
    // doing it to the live history is why a failed first write - or power going
    // inside the 251-380ms a save of 48 entries costs on the device - used to leave
    // no history at all rather than the previous one.
    const int slot = (gen_slot < 0) ? 0 : riftOtherGeneration(gen_slot);
    const uint32_t seq = riftNextGenSeq(gen_seq);

    bool ok = saveInner(genPath(slot), seq);
    if (ok) {
      // Only now is the new generation the one to read, and only now may the next
      // save target the other slot.
      gen_slot = (int8_t) slot;
      gen_seq = seq;
      save_failures = 0;
      // only the slow ones. A save that costs nothing is not news, and a line per
      // save would push everything else out of a 48-line ring within an evening.
      if (last_save_ms >= 50) {
        // The phases as well as the total. There is no watchdog on the main loop, so
        // a slow save is the number that decides whether the write has to be broken
        // up, and open/write/close is what says which part was slow. They are on the
        // readings screen too, but that shows only the most recent save; a ring entry
        // can still be read after the next one has replaced it.
        riftLogf("save %d msg %ums gen%d o%u w%u c%u", count, (unsigned) last_save_ms,
                 slot, (unsigned) t_open, (unsigned) t_write, (unsigned) t_close);
      }
    } else {
      // Both in RiftLogic.h, and separate on purpose: the counter saturating must
      // not stop the deadline moving. See riftNextSaveFailures() for what that cost.
      save_failures = riftNextSaveFailures(save_failures);
      retry_at = riftNextRetryAt((uint32_t) millis(), save_failures);
      // The log line is limited on its own account rather than by the counter. At
      // the 60s ceiling a line per retry is a line a minute into a 48-line ring,
      // which would bury everything else within the hour - and after the fourth
      // identical failure the line has stopped being news.
      if (save_failures <= 4) {
        riftLogf("SAVE FAILED (%u), retry in %us", (unsigned) save_failures,
                 (unsigned) (riftSaveBackoffMillis(save_failures) / 1000u));
      }
    }
    return ok;
  }

  bool saveInner(const char* path, uint32_t seq) {
#if defined(ESP32)
    unsigned long began = millis();
    // Accumulated over exactly the bytes written before the trailer, which is what
    // the trailer then vouches for.
    uint32_t crc = riftCrc32Init();
    uint32_t payload_len = 0;

    File f = SPIFFS.open(path, "w");
    t_open = (uint32_t) (millis() - began);
    if (!f) return false;
    unsigned long t0 = millis();

    // Staged rather than written field by field. Every f.write() crosses the VFS
    // and SPIFFS layers, and the old shape made three calls per record - so a
    // 200-byte file cost 13 calls. One call per bufferful costs 1.
    uint8_t buf[512];
    size_t used = 0;
    bool ok = true;
    // The flush below assumes a whole record always fits in an empty buffer, so
    // a record is never split across two writes.
    static_assert(12 + RIFT_CONV_PEER_LEN + sizeof(Entry::origin) + sizeof(Entry::msg)
                  <= sizeof(buf), "staging buffer cannot hold one record");

    const uint8_t hdr[6] = { 'R', 'M', 'S', 'G', FILE_VERSION, (uint8_t) count };
    memcpy(buf, hdr, sizeof(hdr));
    used = sizeof(hdr);

    // oldest first, so loading can just replay add()
    for (int i = count - 1; i >= 0; i--) {
      const Entry* p = peek(i);
      if (p == NULL) continue;

      uint8_t olen = (uint8_t) strnlen(p->origin, sizeof(p->origin) - 1);
      uint8_t mlen = (uint8_t) strnlen(p->msg, sizeof(p->msg) - 1);
      uint8_t flags = (p->outgoing ? 1 : 0) | (p->delivered ? 2 : 0);
      // The key is stored variable-length for the same reason the strings are: a
      // channel needs one byte of it and an unknown none, so a fixed eight would add
      // ~350 bytes to a ~3KB file, and the file size is what a save costs.
      uint8_t clen = convPayloadLen(p->conv.kind);

      size_t need = 12 + (size_t) clen + (size_t) olen + (size_t) mlen;
      if (used + need > sizeof(buf)) {
        if (f.write(buf, used) != used) { ok = false; break; }
        crc = riftCrc32Update(crc, buf, used);
        payload_len += used;
        used = 0;
      }

      uint8_t* r = buf + used;
      memcpy(&r[0], &p->timestamp, 4);
      r[4] = flags;
      memcpy(&r[5], &p->trip_ms, 4);
      r[9] = olen;
      r[10] = mlen;
      r[11] = p->conv.kind;
      if (p->conv.kind == RIFT_CONV_CHANNEL) {
        r[12] = p->conv.channel_idx;
        memcpy(&r[13], &p->conv.channel_fp, 4);
      }
      else if (p->conv.kind == RIFT_CONV_DM) memcpy(&r[12], p->conv.peer, RIFT_CONV_PEER_LEN);
      memcpy(&r[12 + clen], p->origin, olen);
      memcpy(&r[12 + clen + olen], p->msg, mlen);
      used += need;
    }
    if (ok && used > 0) {
      if (f.write(buf, used) != used) {
        ok = false;
      } else {
        crc = riftCrc32Update(crc, buf, used);
        payload_len += used;
      }
    }
    // The trailer last, and writing it is the commit. A save interrupted before
    // this point leaves a generation the reader refuses, and the other generation -
    // never touched by this write - is still the one it reads.
    if (ok) {
      uint8_t tr[RIFT_MSGLOG_TRAILER_LEN];
      riftMsgLogTrailerWrite(tr, seq, payload_len, riftCrc32Final(crc));
      if (f.write(tr, sizeof(tr)) != sizeof(tr)) ok = false;
    }
    t_write = (uint32_t) (millis() - t0);

    t0 = millis();
    f.close();
    t_close = (uint32_t) (millis() - t0);

    if (!ok) {
      // The partial file is kept rather than removed, and now it costs nothing to
      // keep: it has no trailer, so the reader refuses it and reads the other
      // generation instead. gen_slot is not moved, so the retry writes this same
      // slot again and the good generation stays where it is.
      //
      // dirty stays set, so the backoff retries and a later save writes this
      // generation whole.
      return false;
    }

    last_save_ms = (uint32_t) (millis() - began);
    dirty = false;
    return true;
#else
    (void) path;
    return false;
#endif
  }

#if defined(ESP32)
  // The trailer and the file size only, so choosing between the generations costs
  // eighteen bytes a slot instead of reading either payload.
  bool readTrailer(const char* path, uint32_t* seq, uint32_t* len, uint32_t* crc) {
    File f = SPIFFS.open(path, "r");
    if (!f) return false;
    const uint32_t size = (uint32_t) f.size();
    if (size < RIFT_MSGLOG_TRAILER_LEN || !f.seek(size - RIFT_MSGLOG_TRAILER_LEN)) {
      f.close();
      return false;
    }
    uint8_t tr[RIFT_MSGLOG_TRAILER_LEN];
    const bool got = f.read(tr, sizeof(tr)) == sizeof(tr);
    f.close();
    return got && riftMsgLogTrailerParse(tr, size, seq, len, crc);
  }

  // Checked before a single entry is added, so a generation that fails leaves the
  // log empty and the other one can still be tried. Streamed a bufferful at a time
  // rather than read whole: the payload runs to about 3KB and there is no 3KB to
  // spare at 60.8% RAM.
  bool crcMatches(const char* path, uint32_t len, uint32_t want) {
    File f = SPIFFS.open(path, "r");
    if (!f) return false;
    uint32_t crc = riftCrc32Init();
    uint8_t buf[256];
    uint32_t left = len;
    while (left > 0) {
      const size_t n = (left < sizeof(buf)) ? (size_t) left : sizeof(buf);
      if (f.read(buf, n) != n) { f.close(); return false; }
      crc = riftCrc32Update(crc, buf, n);
      left -= n;
    }
    f.close();
    return riftCrc32Final(crc) == want;
  }
#endif

  void load() {
#if defined(ESP32)
    uint32_t seq[2] = { 0, 0 }, len[2] = { 0, 0 }, want[2] = { 0, 0 };
    bool ok[2] = { false, false };
    for (int s = 0; s < 2; s++) ok[s] = readTrailer(genPath(s), &seq[s], &len[s], &want[s]);

    const int newest = riftPickNewerGeneration(ok[0], seq[0], ok[1], seq[1]);
    if (newest >= 0) {
      // Newest first, then the other one: a generation can carry a trailer that
      // parses and still fail its CRC, and the second copy is what that case is
      // for. Reading the older history is the right answer there - it is the
      // newest one that is known to be whole.
      for (int t = 0; t < 2; t++) {
        const int s = (t == 0) ? newest : riftOtherGeneration(newest);
        if (!ok[s]) continue;
        if (!crcMatches(genPath(s), len[s], want[s])) {
          riftLogf("msglog gen%d seq%u: bad CRC", s, (unsigned) seq[s]);
          continue;
        }
        loadPayload(genPath(s));
        gen_slot = (int8_t) s;
        gen_seq = seq[s];
        return;
      }
      riftLogf("msglog: both generations unreadable");
    }

    // No usable generation, which on the first boot after this change is the normal
    // case and not a fault. gen_slot stays -1, so the first save writes slot 0, and
    // the legacy file stays where it is as the older copy of last resort.
    loadPayload(RIFT_MSGLOG_PATH);

    // Reading a generation is not a change and loadPayload() clears dirty for that
    // reason. Reading the *legacy* file is different: the history now exists only in
    // the format being replaced, and nothing else would move it across until the
    // next message happened to arrive - which on a quiet mesh is hours, all of them
    // with no crash-safe copy. So the migration finishes itself, one debounce after
    // boot. Only when there is something to migrate: a device with no history has
    // nothing to gain from an empty generation, and its first message writes one.
    if (count > 0) markDirty();
#endif
  }

  // Reads one file that has already been established as complete, or the legacy
  // single file. Unchanged from when there was only one: the payload format, its
  // 6-byte header and all of its version handling are the same bytes as before, so
  // generations wrap the old format rather than replacing it.
  void loadPayload(const char* path) {
#if defined(ESP32)
    File f = SPIFFS.open(path, "r");
    if (!f) return;

    uint8_t hdr[6];
    if (f.read(hdr, sizeof(hdr)) != sizeof(hdr)
        || hdr[0] != 'R' || hdr[1] != 'M' || hdr[2] != 'S' || hdr[3] != 'G'
        || (hdr[4] < 1 || hdr[4] > FILE_VERSION)) {
      f.close();
      return;   // absent, truncated or a format we do not know - start empty
    }
    bool has_conv = (hdr[4] >= 2);

    int n = hdr[5];
    if (n > RIFT_MSG_LOG_SIZE) n = RIFT_MSG_LOG_SIZE;

    for (int i = 0; i < n; i++) {
      uint8_t rec[12];
      size_t fixed = has_conv ? sizeof(rec) : FILE_V1_FIXED;
      if (f.read(rec, fixed) != fixed) break;   // truncated: keep what we have

      uint32_t ts, trip;
      memcpy(&ts, &rec[0], 4);
      memcpy(&trip, &rec[5], 4);
      uint8_t flags = rec[4], olen = rec[9], mlen = rec[10];

      RiftConvKey conv = riftConvUnknown();
      if (has_conv) {
        uint8_t payload[RIFT_CONV_PEER_LEN];
        // From the file's own version, not from this build's: a version 2 channel
        // record is 1 byte where version 3 is 5, and reading 5 would swallow the
        // start of the origin string and shift every field after it.
        uint8_t clen = (rec[11] == RIFT_CONV_CHANNEL && hdr[4] < 3) ? 1
                                                                    : convPayloadLen(rec[11]);
        if (clen > 0 && f.read(payload, clen) != clen) break;
        if (rec[11] == RIFT_CONV_CHANNEL) {
          // A version 3 record carries the fingerprint and is trusted. A version 2 one
          // carries only the slot, and a slot is not an identity: delete the channel
          // that was in slot 2 and create another, and the new one inherits the old
          // one's history. That is a private conversation shown under someone else's
          // name, which is worse than showing it under none.
          //
          // So a legacy record becomes unknown rather than a channel. It is not lost:
          // an unknown conversation falls back to matching the name in its origin
          // string, which is the channel name as it was when the message arrived - so
          // legacy history groups under the channel it actually came from, and a
          // channel that no longer exists simply has no conversation to appear in.
          if (clen >= 5) {
            uint32_t fp = 0;
            memcpy(&fp, &payload[1], 4);
            conv = riftConvChannel(payload[0], fp);
          }
        }
        else if (rec[11] == RIFT_CONV_DM) conv = riftConvDM(payload);
        // an unrecognised kind - a file from a newer build - stays unknown rather
        // than being guessed at
      }

      char origin[62], msg[MAX_TEXT_LEN + 1];
      if (olen >= sizeof(origin) || mlen >= sizeof(msg)) break;   // corrupt length
      if (f.read((uint8_t*) origin, olen) != olen) break;
      if (f.read((uint8_t*) msg, mlen) != mlen) break;
      origin[olen] = 0;
      msg[mlen] = 0;

      Entry* p = add(ts, conv, origin, msg, (flags & 1) != 0);   // clears dirty below
      // Delivery state that survives: whether it landed, and how long it took.
      // expected_ack, sent_at_ms and timeout_ms are millis-based and meaningless
      // now, so they stay zero - which reads as "no ack expected" rather than as
      // a send still waiting. A message that was pending when the power went is
      // something this device can no longer know the fate of, and saying "..."
      // forever would be a claim it cannot support.
      p->delivered = (flags & 2) != 0;
      p->trip_ms = trip;
    }
    f.close();

    // Restoring is not a change. add() marks the log dirty because that is what
    // it means for a new message, and load() reuses it - so a history just read
    // back correctly was written out again twenty seconds after every boot.
    dirty = false;
    dirty_at = 0;
#else
    (void) path;
#endif
  }
};

