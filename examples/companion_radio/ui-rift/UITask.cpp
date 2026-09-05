#include "UITask.h"
#include "RiftLogic.h"
#include "RiftEventLog.h"
#include "RiftRxLog.h"
#include "RiftRepeater.h"
#include "RiftScopes.h"
#include "RiftScreenDump.h"
#include <helpers/sensors/LPPDataHelpers.h>   // LPP_* type codes, for the telemetry labels
#include <helpers/TxtDataHelpers.h>
#include <helpers/UTF8Helpers.h>
#include "../MyMesh.h"
#include "target.h"
#include <stdarg.h>

// RiftLogic.h mirrors these so it can be compiled without MeshCore for the native
// tests. If upstream ever renumbers them, fail here rather than silently applying
// the wrong policy.
static_assert(RIFT_ADV_CHAT == ADV_TYPE_CHAT, "advert type drift");
static_assert(RIFT_ADV_REPEATER == ADV_TYPE_REPEATER, "advert type drift");
static_assert(RIFT_ADV_ROOM == ADV_TYPE_ROOM, "advert type drift");
static_assert(RIFT_ADV_SENSOR == ADV_TYPE_SENSOR, "advert type drift");


#ifdef RIFT_RADAR
  #include <WiFi.h>
  #include <BLEDevice.h>
  #include <BLEScan.h>
  #include <BLEAdvertisedDevice.h>

  #ifndef RIFT_WIFI_DWELL_MILLIS
    #define RIFT_WIFI_DWELL_MILLIS 120    // per channel; ~1.6s for a full sweep
  #endif
  #ifndef RIFT_BLE_DWELL_SECS
    #define RIFT_BLE_DWELL_SECS 3
  #endif
  #ifndef RIFT_SCAN_GAP_MILLIS
    #define RIFT_SCAN_GAP_MILLIS 400      // breathing room between sweeps
  #endif
  #ifndef RIFT_RF_AGE_MILLIS
    #define RIFT_RF_AGE_MILLIS 45000      // forget contacts not heard for 45s
  #endif
  #ifndef RIFT_SCAN_STOP_GRACE_MILLIS
    #define RIFT_SCAN_STOP_GRACE_MILLIS 700   // let a scan wind down before deinit
  #endif
#endif

#ifndef RIFT_VERSION
  #define RIFT_VERSION "0.0.0-dev"
#endif

#ifndef AUTO_OFF_MILLIS
  #define AUTO_OFF_MILLIS     15000   // 15 seconds
#endif
#define BOOT_SCREEN_MILLIS   2500   // 2.5 seconds

#ifdef PIN_STATUS_LED
#define LED_ON_MILLIS     20
#define LED_ON_MSG_MILLIS 200
#define LED_CYCLE_MILLIS  4000
#endif

#define LONG_PRESS_MILLIS   1200

#if UI_HAS_JOYSTICK
  #define PRESS_LABEL "press Enter"
#else
  #define PRESS_LABEL "long press"
#endif

// The first slot is the wordmark as well as the home destination - logo as home.
// The RIFT_NAV_MESH constant keeps its name in code; only the label changed.
static const char* NAV_LABELS[RIFT_NAV_COUNT] = { "RIFT", "NODES", "RADAR", "COMMS", "SYSTEM" };

// channel the COMMS screen starts on; any configured channel can be selected
// from the target picker at runtime
#ifndef RIFT_PUBLIC_CHANNEL_IDX
  #define RIFT_PUBLIC_CHANNEL_IDX 0
#endif

// The T-Deck keyboard has no ESC or dedicated back key, so backspace doubles as
// "one level back" wherever there is no text to delete. UIScreen.h has no
// constant for it - the raw byte is what the co-processor sends.
#define RIFT_KEY_BACK       8

// How close together two presses of the same vowel must be to mean "offer me its
// Nordic forms" rather than two letters.
//
// A double tap rather than a long press, because this keyboard cannot report one:
// it sends a single event per press and nothing while the key is down. Measured,
// not assumed - see TDeckKeyboard::poll().
//
// 400ms with a 100ms poll leaves room for a comfortable double tap. The cost is
// that a genuine double vowel typed quickly opens the picker; backspace cancels
// it and leaves the two letters as typed. Modern Norwegian has almost none - the
// spelling reform turned "aa" into "å" - but proper names like Haakon and Aage
// still do.
#define RIFT_DOUBLETAP_MILLIS  400

#define RIFT_MSG_LOG_SIZE  48

// Where the message history lives, and how long a burst is allowed to settle
// before it is written.
//
// 20 seconds coalesces a conversation into one write while keeping the loss
// window on a power cut to something a user would describe as "the last thing I
// said" rather than "this evening". A clean shutdown flushes immediately, so the
// window only applies to power being pulled.
// How many heard nodes NODES can hold, and how many hop columns it draws. Up here
// rather than beside that screen because SYSTEM's diagnostics read them too, and
// SYSTEM is defined first.
#define RIFT_CONST_MAX 96

// AdvertPath::path_len is Packet's raw encoding, not a hop count: bits 6-7 carry
// the hash size minus one and bits 0-5 the number of hops (Mesh.cpp:449). Reading
// it as a plain count worked only while path_hash_mode was 0 - at 2-byte hashes a
// two-hop route reads as 66 hops.
static inline uint8_t riftHopCount(uint8_t path_len) { return mesh::Packet::pathHashCount(path_len); }
static inline uint8_t riftHashSize(uint8_t path_len) { return mesh::Packet::pathHashSize(path_len); }

#define RIFT_MSGLOG_PATH         "/rift_msgs.dat"
// no longer written; kept so a stale one from an older build can be removed
#define RIFT_MSGLOG_TMP          "/rift_msgs.new"
#define RIFT_MSGLOG_FLUSH_MILLIS 20000
#define RIFT_CHAR_W         6   // Adafruit GFX classic font cell at setTextSize(1)
#define RIFT_LINE_H        12   // row pitch used throughout this codebase

// The CP437 middle dot, built from its byte rather than written as an escape. This
// file is UTF-8, so a typed dot is two bytes and the panel draws two unrelated
// blocks - the same trap the full block 0xDB fell into, and the first attempt at
// this line fell into it again: the escape was interpreted on the way into the file
// and the macro ended up holding U+00FA as two bytes. A byte array cannot be
// mis-encoded by anything between here and the compiler. Used as a %s argument, not
// concatenated, because it is an array and not a literal.
static const char RIFT_DOT[] = { (char) 0xFA, 0 };

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
  uint32_t last_save_ms = 0;    // how long the last write took, for SYSTEM
  // Phase breakdown - open / write / close. The total came out at 553ms for four
  // messages, which is about 200 bytes: far too little data for the volume to be
  // the cause, and long enough to starve LoRa. The cost was the number of
  // operations. Kept on SYSTEM because it is the only evidence of whether
  // removing four of them was enough.
  uint32_t t_open = 0, t_write = 0, t_close = 0;

  void markDirty() { dirty = true; dirty_at = millis(); }

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

  // both in RiftLogic.h, so the backoff is tested without a filesystem
  bool dueToSave(unsigned long now) const {
    return riftShouldFlush(dirty, (uint32_t) now, (uint32_t) dirty_at,
                           RIFT_MSGLOG_FLUSH_MILLIS, save_failures, (uint32_t) retry_at);
  }

  bool save(const char* path) {
    bool ok = saveInner(path);
    if (ok) {
      save_failures = 0;
      // only the slow ones. A save that costs nothing is not news, and a line per
      // save would push everything else out of a 48-line ring within an evening.
      if (last_save_ms >= 50) riftLogf("save %d msg %ums", count, (unsigned) last_save_ms);
    } else if (save_failures < 255) {
      save_failures++;
      retry_at = millis() + riftSaveBackoffMillis(save_failures);
      riftLogf("SAVE FAILED (%u), retry in %us", (unsigned) save_failures,
               (unsigned) (riftSaveBackoffMillis(save_failures) / 1000u));
    }
    return ok;
  }

  bool saveInner(const char* path) {
#if defined(ESP32)
    unsigned long began = millis();

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
    if (ok && used > 0 && f.write(buf, used) != used) ok = false;
    t_write = (uint32_t) (millis() - t0);

    t0 = millis();
    f.close();
    t_close = (uint32_t) (millis() - t0);

    if (!ok) {
      // The partial file is kept, not removed. It used to be removed on the grounds
      // that its header claims a count the file does not hold - but load() already
      // stops at the first record that does not arrive and keeps what did, so a
      // short file costs the newest messages and nothing else. Deleting it destroyed
      // data the reader was built to recover, and if the power went before the retry
      // the whole history was gone rather than its tail.
      //
      // dirty stays set, so the backoff will retry and a later successful save
      // rewrites the file whole.
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

  void load(const char* path) {
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

static RiftMsgLog msg_log;

// RiftUnread lives in RiftLogic.h: it is array bookkeeping with no display and no
// filesystem in it, so it belongs where it can be tested. Its eviction policy was
// documented as least-recently-active and implemented as something else, which is
// exactly the class of mistake a native test catches and a build does not.
static RiftUnread msg_unread;

// The conversation key for a channel slot, with the fingerprint taken from the
// channel actually sitting in it.
//
// An empty slot returns unknown rather than a slot-only key. Unknown never equals
// unknown, so nothing matches it - which is right: an empty slot has no conversation,
// and returning a key with fingerprint 0 would have made it a wildcard matching every
// entry ever stored against that slot.
//
// The key length mirrors setChannel(): 16 bytes unless the upper half is non-zero, in
// which case 32. Same test upstream uses to decide 128 against 256 bit, so the
// fingerprint is taken over the real key rather than over trailing padding that some
// path might leave uninitialised.
static RiftConvKey riftChannelConv(uint8_t idx) {
  ChannelDetails ch;
  if (!the_mesh.getChannel(idx, ch) || ch.name[0] == 0) return riftConvUnknown();

  static const uint8_t zeroes[16] = { 0 };
  size_t klen = (memcmp(&ch.channel.secret[16], zeroes, 16) == 0) ? 16 : 32;
  return riftConvChannel(idx, riftChannelFingerprint(ch.channel.secret, klen));
}


// Break text into lines at a pixel width, calling emit() per line (NULL just
// counts). DisplayDriver::printWordWrap() is only a default that forwards to
// print(), and ST7789NativeDisplay doesn't override it, so GFX would wrap to
// x=0 rather than to our left margin - hence doing it ourselves.
static int wrapText(const char* text, int max_px, int y, DisplayDriver* display, int x) {
  int max_chars = max_px / RIFT_CHAR_W;
  if (max_chars < 1) max_chars = 1;

  int lines = 0;
  int len = strlen(text);
  int pos = 0;

  while (pos < len) {
    int take = len - pos;
    if (take > max_chars) {
      take = max_chars;
      // step back to the last space so words stay intact
      int brk = take;
      while (brk > 0 && text[pos + brk] != ' ') brk--;
      if (brk > 0) take = brk;
    }

    if (display != NULL) {
      char line[64];
      int n = take < (int) sizeof(line) - 1 ? take : (int) sizeof(line) - 1;
      memcpy(line, &text[pos], n);
      line[n] = 0;
      display->setCursor(x, y + lines * RIFT_LINE_H);
      display->print(line);
    }
    lines++;

    pos += take;
    while (pos < len && text[pos] == ' ') pos++;   // swallow the break space
  }

  return lines == 0 ? 1 : lines;
}

// evenly spread directions for the radial plots (cos/sin * 100), shared by the
// RADAR scatter and the CONSTELLATION topology view

// UTF-8 to what the display can actually draw.
//
// DisplayDriver::translateUTF8ToBlocks() replaces every non-ASCII byte with a
// solid block, so Nordic text arriving from other MeshCore clients rendered as
// gibberish. The Adafruit classic font is CP437, which does carry most of the
// letters we need - and the two it lacks are synthesized by the display driver.
// riftTranslateUTF8 now lives in RiftLogic.h so it can be tested; it was static
// here, and had already shipped drawing every unmappable character as a cent
// sign. The comment on it there records why.

// Single-line text editor for the settings fields. Deliberately not shared with
// the COMMS compose line: that one works, is tested, and has its own 160-char
// capacity and tail-scrolling - refactoring it here would risk a regression for
// no user-visible gain.
struct RiftTextInput {
  char buf[68];       // fits a 44-char base64 PSK and a 32-char name
  int len;
  int cap;
  // Renders as dots instead of characters. For the repeater password, which is
  // somebody else's shared secret and is typed in the field where the screen is
  // the most public part of the device.
  bool mask;

  void begin(const char* initial, int capacity) {
    cap = (capacity < (int) sizeof(buf) - 1) ? capacity : (int) sizeof(buf) - 1;
    StrHelper::strncpy(buf, initial ? initial : "", sizeof(buf));
    len = strlen(buf);
    if (len > cap) { len = cap; buf[len] = 0; }
    mask = false;
  }

  // returns true if the key was consumed as text editing
  bool handleKey(char c) {
    if (c == RIFT_KEY_BACK) {
      if (len > 0) { buf[--len] = 0; return true; }
      return false;   // nothing to delete - let the caller treat it as "back"
    }
    if (c >= 32 && c < 127 && len < cap) {
      buf[len++] = c;
      buf[len] = 0;
      return true;
    }
    return false;
  }

  void render(DisplayDriver& display, int x, int y, int max_px) {
    int max_chars = (max_px - 2 * RIFT_CHAR_W) / RIFT_CHAR_W;

    display.setColor(UIColor::primary_txt);
    display.setCursor(x, y);
    display.print("> ");
    if (mask) {
      // One dot per character actually held, capped at what fits. Never the
      // characters themselves, and never a fixed width that would hide whether
      // the keyboard is registering at all.
      int n = len;
      if (max_chars > 0 && n > max_chars) n = max_chars;
      for (int i = 0; i < n; i++) display.print(RIFT_DOT);
    } else {
      const char* shown = buf;
      if (len > max_chars && max_chars > 0) shown = buf + (len - max_chars);
      display.print(shown);
    }
    display.print("_");
  }
};

// Bottom navigation hint row shared by every RIFT nav screen.
// Nav bar - y 226..239, per the design handoff. Label centres are given rather
// than computed: 64px columns would centre SYSTEM at 288, which clips against the
// right edge, so the spec nudges the outer two inward.
static const int NAV_CENTRE_X[RIFT_NAV_COUNT] = { 20, 81, 145, 209, 270 };

#define NAV_RULE_Y 226

// The marker under a nav label, sized to the label rather than to a 64px column.
//
// The labels were nudged off the even grid so SYSTEM would not clip against the
// right edge. The marker was left on it, so every underline ran three quarters of
// a label-width to the right of the word it marked - 12px under RIFT, 15 under the
// middle three, 18 under SYSTEM, where it began four pixels right of the label's
// first glyph and ran off the screen edge. Each one reached further under its
// right-hand neighbour than under its own word, which is visible in a photograph
// and not only in the arithmetic.
//
// A shorter marker is also better at the job it was given. It exists because the
// grey step between active and inactive vanishes in sunlight, and a 64px bar under
// a 30px word is ambiguous about which word it belongs to - which is exactly what
// sunlight was going to add anyway.
static void navMarkerSpan(int i, int* x, int* w) {
  *w = (int) strlen(NAV_LABELS[i]) * RIFT_CHAR_W + 8;
  *x = NAV_CENTRE_X[i] - *w / 2;
}

// The one window pattern every list with a cursor uses, from the September
// design round (design/redesign-2026-09/00-SYSTEM.md section 4). A 1px track in
// rule at x 319 down the visible list, and a 2px thumb in fg at x 318 whose
// position and length are the window's share of the whole. Drawn only when the
// list is longer than the window, so a list that fits shows nothing. Text in a
// row stops at x 314 to leave it room. Not a touch target: the track is 1px and
// cannot be hit, so scrolling is always a drag on the rows.
//
// first/total/view are in whatever unit the list scrolls by - rows for a
// stepping list, pixels for COMMS - as long as all three agree.
static void riftDrawThumb(DisplayDriver& display, int y0, int len, long first, long total, long view,
                          int x_track = 319) {
  if (total <= view || total <= 0 || len <= 0) return;
  display.setColor(rift_pal.rule);
  display.fillRect(x_track, y0, 1, len);
  long th = (view * len + total / 2) / total;
  if (th < 6) th = 6;
  if (th > len) th = len;
  long ty = (first * len + total / 2) / total;
  if (ty + th > len) ty = len - th;
  if (ty < 0) ty = 0;
  display.setColor(rift_pal.fg);
  display.fillRect(x_track - 1, y0 + (int) ty, 2, (int) th);
}

static void renderNavBar(DisplayDriver& display, int curr_idx) {
  const int y_rule = NAV_RULE_Y;

  display.setColor(rift_pal.rule);
  display.fillRect(0, y_rule, display.width(), 1);

  // Active tab carried by colour alone before this; in sunlight the grey step
  // between active and inactive disappears. The underline is the second cue.
  display.setColor(rift_pal.accent);
  int mark_x, mark_w;
  navMarkerSpan(curr_idx, &mark_x, &mark_w);
  display.fillRect(mark_x, y_rule, mark_w, 2);

  display.setTextSize(1);
  for (int i = 0; i < RIFT_NAV_COUNT; i++) {
    // The wordmark slot gets the brand colour, but only while it is the active
    // tab, and by the accent's two documented roles: legible as text at 6.0:1 on
    // black, fill-only at 3.5:1 on white.
    //
    // Only while active, because an accent fill *is* the active signal in this
    // design - a permanent orange chip on one tab would read as selected from
    // every screen and undermine the one thing the underline says. Inactive, RIFT
    // is dim like the other four.
    if (i == RIFT_NAV_MESH && i == curr_idx) {
      if (rift_day_mode) {
        int chip_x, chip_w;
        navMarkerSpan(i, &chip_x, &chip_w);
        display.setColor(rift_pal.accent);
        display.fillRect(chip_x, y_rule + 2, chip_w, 12);
        display.setColor(rift_pal.on_accent);
      } else {
        display.setColor(rift_pal.accent);
      }
    } else {
      display.setColor(i == curr_idx ? rift_pal.fg : rift_pal.dim);
    }
    display.drawTextCentered(NAV_CENTRE_X[i], 228, NAV_LABELS[i]);
  }

  // Straight from the unread model, which is the only thing that tracks this now.
  //
  // It used to come from a counter the message preview owned, and the two could
  // disagree in both directions. The bad case was easy to reach: with COMMS open on a
  // channel, a message arriving in that same channel marked the model, the popup was
  // correctly suppressed because the message was already on screen - and so nothing
  // ever dismissed the popup, which was the only thing that cleared the counter. The
  // dot stayed lit permanently for a message that had been read.
  // fg, not accent: unread is a shape (present or absent), and the accent is
  // reserved for active tab, selection, warning and the wordmark after the
  // September design round. Vertically centred on the label row.
  if (msg_unread.any()) {
    display.setColor(rift_pal.fg);
    display.fillRect(240, 231, 3, 3);
  }

  // Battery, moved down from the title bar into space that was already here:
  // SYSTEM is centred at 270 and ends at 287, leaving 32px to the right edge, and
  // "100%" is 24px. SYSTEM's marker now ends at 291 rather than running to the
  // screen edge, and sits at y 226..227 while this text occupies 228..236, so they
  // do not touch on either axis.
  //
  // mid rather than fg: this is chrome, and fg would make it compete with the
  // active nav label. accent when low, which is the one case worth interrupting.
  // snprintf, and not because the values can overflow it today - the percentage
  // is clamped to 0..100 where it is set. The compiler cannot see that clamp, and
  // said so; a bound that depends on a clamp somewhere else is one edit away from
  // being wrong.
  char batt[8];
  if (curr_idx == RIFT_NAV_SYSTEM) {
    // SYSTEM has two pages and nowhere else to say which one you are on. The
    // battery is on the other four screens, so nothing is lost by borrowing the
    // slack here for the page number.
    snprintf(batt, sizeof(batt), "%d/2", rift_system_page + 1);
    display.setColor(rift_pal.mid);
  } else {
    snprintf(batt, sizeof(batt), "%d%%", rift_nav_batt_pct);
    display.setColor(rift_nav_batt_pct <= 15 ? rift_pal.accent : rift_pal.mid);
  }
  display.drawTextRightAlign(display.width() - 2, 228, batt);
}

#ifndef BATT_MIN_MILLIVOLTS
  #define BATT_MIN_MILLIVOLTS 3000
#endif
#ifndef BATT_MAX_MILLIVOLTS
  #define BATT_MAX_MILLIVOLTS 4200
#endif

// Where a screen is, in one line at the top of its own body.
//
// This replaced a filled 16px title bar that carried the wordmark, the battery
// and this text. The first two moved to the nav bar, so all the chrome is on one
// edge; what was left was a band whose only content was context, and on the
// screens where that context was just the screen name it was saying nothing the
// nav bar did not already say.
//
// Screens with nothing to add do not call this at all. Eleven of the fourteen old
// call sites did have something: which step of SYSTEM's channel flow you are in,
// which channel or contact COMMS is aimed at, how many nodes NODES has heard,
// whether RADAR is scanning. Those are worth a line; a band was not.
//
// Same position as the old subtitle, minus the fill - so it reads as the screen's
// heading rather than as chrome sitting above it.
static void renderHeading(DisplayDriver& display, const char* text) {
  display.setTextSize(1);
  display.setColor(rift_pal.mid);
  display.drawTextLeftAlign(2, 2, text);
}

// RGB565 straight from the design handoff. Night and day are the same geometry
// with a swapped colour table.
//
// Two rules from the spec that the values encode: nothing darker than #6E6E6E on
// black survives 6x8 with no antialiasing, and de-emphasis inverts with the field
// - on white, dim has to be *darker*, not lighter. The accent is one value in
// both modes but changes role: on black it is legible as text at 6.0:1, on white
// it is 3.5:1 and may only be a fill - never text, and never the ground under
// white text either, which is the half of this rule that was missing until
// September. Contrast is symmetric, so white on the accent is the same 3.5:1 that
// forbids the accent on white, and thirteen surfaces drew the row the user is
// acting on as the least readable row on the screen. The ink for an accent fill
// is on_accent, which is 6.01:1 and the same value in both palettes.
static const RiftPalette RIFT_NIGHT = {
  /* bg     */ 0x0000,   // #000000
  /* bar    */ 0x18C3,   // #1A1A1A
  /* fg     */ 0xFFFF,   // #FFFFFF
  /* mid    */ 0x9CD3,   // #9A9A9A
  /* dim    */ 0x8C51,   // #8A8A8A
  /* rule   */ 0x738E,   // #707070
  /* accent */ 0xFA00,   // #FF4100
  /* acc_tx */ 0xFA00,   // legible as text on black
  /* on_acc */ 0x0000,   // ink inside an accent fill: 6.01:1, and mode-independent
  /* ok     */ 0x3E40    // #39C800
};
static const RiftPalette RIFT_DAY = {
  0xFFFF, 0xEF5D, 0x0000, 0x5ACB, 0x6B4D, 0x8C51, 0xFA00,
  /* acc_tx */ 0x2104,   // #202020 - the accent itself is unreadable as text here
  // Same value as night's. The accent does not change between modes, so its ink
  // must not either: bg would have been the obvious choice and it is the wrong
  // one, because bg follows the field and turns white here. That is the bug this
  // role exists to close, and the repeater menu had exactly it.
  /* on_acc */ 0x0000,
  // #428610, not the night green. #39CB00 is 2.16:1 on white - below even the 3:1
  // non-text floor - and COMMS draws "ACK 1.2s" in it, so in day mode the one label
  // that says a message arrived was the least readable thing on the screen. Found by
  // computing the palette's contrast rather than by looking at it, which is the only
  // way this kind of failure gets noticed: it looks like a colour choice.
  //
  // #428610 was the lightest green at that hue clearing 4.5:1 on white, and it
  // sat OKLab 0.053 from channel colour 2 - the closest pair in the palette, so in
  // day mode a channel name and "delivered" were nearly one colour. The September
  // design round moved it to #2A6400: 7.10:1 on white, 0.104 and 0.124 from the
  // two channel greens, still 137 degrees of hue. A day value need not work on
  // black, which is what lets it go darker than the channel band. Per-mode,
  // exactly as acc_tx already is. tools/palette-check.py prints the figures.
  /* ok     */ 0x2B20
};

RiftPalette rift_pal = RIFT_NIGHT;
bool rift_day_mode = false;

// ---- screen dump, see RiftScreenDump.h ----
//
// The request is recorded here and acted on from UITask::loop(): the parser runs
// inside the mesh loop, and the frame it wants does not exist until the UI has
// drawn it. Two steps - switch and redraw, then stream - so what goes out is the
// screen that was asked for, freshly composed, overlays and alert box included.
static volatile bool s_dump_pending = false;
static volatile int  s_dump_nav = -1;

void riftRequestScreenDump(int nav) {
  s_dump_nav = nav;
  s_dump_pending = true;
}

// Raw on the port, outside the companion framing: a frame holds 184 bytes and
// this is 153600. Nothing else writes to Serial from this loop iteration, so the
// bytes cannot interleave with a push frame.
static void riftStreamFrame() {
#ifdef RIFT_DISPLAY
  const uint16_t* fb = display.frameBuffer();
  if (fb == NULL) return;   // no canvas: the driver is drawing straight to the panel
  const uint16_t w = (uint16_t) display.width(), h = (uint16_t) display.height();
  const uint8_t hdr[8] = { (uint8_t) (w & 0xFF), (uint8_t) (w >> 8),
                           (uint8_t) (h & 0xFF), (uint8_t) (h >> 8), 16, 0, 0, 0 };
  Serial.write((const uint8_t*) "RIFTSCRN", 8);
  Serial.write(hdr, sizeof(hdr));
  Serial.write((const uint8_t*) fb, (size_t) w * h * 2);
  // A tail of zeros after the frame. One capture in four arrived 256 bytes
  // short - the size of the USB CDC endpoint buffer - with flush() making no
  // difference, so the last packet was sitting in the peripheral waiting for
  // more data behind it. The pad is that data; the host reads the frame length
  // it was told and ignores the rest.
  static const uint8_t pad[64] = { 0 };
  for (int i = 0; i < 8; i++) Serial.write(pad, sizeof(pad));
  Serial.flush();
#endif
}
bool rift_screen_always_on = false;

#ifdef RIFT_RADAR
// Which radios RADAR sweeps. Three states rather than two switches: with both off
// the screen has nothing to do, and leaving RADAR already powers everything down -
// so a fourth state would be a way to reach a dead screen and nothing else.
//
// It shortens the sweep as well as the list, which is the other half of why it was
// asked for: one radio per cycle instead of two.
#define RIFT_SRC_BOTH 0
#define RIFT_SRC_WIFI 1
#define RIFT_SRC_BLE  2
uint8_t rift_radar_src = RIFT_SRC_BOTH;
static inline bool riftScanWifi() { return rift_radar_src != RIFT_SRC_BLE; }
static inline bool riftScanBle()  { return rift_radar_src != RIFT_SRC_WIFI; }
#endif

#ifdef RIFT_RADAR
// ------------------------------------------------------------- proximity watch
//
// A handful of RF devices marked from RADAR, and an alert when one of them turns
// up. Declared here rather than beside the scan tables because the settings file
// below has to write it.
//
// Two limits are worth stating where the code is, not only in the README:
//
// The key is a hardware address, and modern BLE devices rotate theirs every few
// minutes for exactly the reason this feature exists. Marking a phone or a watch
// will stop matching when it next re-randomises. It holds for Wi-Fi access points,
// whose BSSID is stable, and for BLE devices with a static address - many beacons,
// tags and headphones.
//
// And RADAR tears the radios down when you leave the screen, deliberately: one
// antenna shared with LoRa and no watchdog on the main loop. So an alert fires only
// while RADAR is open. Making it fire in the background is a separate decision with
// a real cost to the mesh, and it has not been taken.
// Twelve rather than four. Four was enough when a watch was only an arrival alert,
// but a watch now also carries the name you gave the device - and "remember which is
// which" does not work with four slots. Each entry is about forty bytes, so this
// costs a few hundred, and the settings loader already clamps a longer file to
// whatever this is. An older build reading a newer file keeps the first four.
#define RIFT_WATCH_MAX 12
// How long without a sighting before a device counts as gone. A passive scan only
// sees a device when it chooses to transmit, and a BLE beacon can be quiet for tens
// of seconds, so this is generous - a shorter window would report it leaving and
// arriving repeatedly while it sat still.
#define RIFT_WATCH_GONE_MILLIS   90000UL
// Minimum between two alerts for the same device, so one that sits at the edge of
// range cannot alert on every sweep.
#define RIFT_WATCH_REARM_MILLIS 300000UL

struct RfWatch {
  uint8_t key[6];
  bool    is_wifi;
  char    name[24];        // display only; the key is the identity
  bool    present;         // last known state, so only the transition alerts
  unsigned long last_alert;
};
static RfWatch rf_watch[RIFT_WATCH_MAX];
static int rf_watch_count = 0;

static int rfWatchFind(const uint8_t* key, bool is_wifi) {
  for (int i = 0; i < rf_watch_count; i++) {
    if (rf_watch[i].is_wifi == is_wifi && memcmp(rf_watch[i].key, key, 6) == 0) return i;
  }
  return -1;
}
#endif
#ifdef RIFT_SPEAKER
#include <helpers/ui/TDeckSpeaker.h>
static TDeckSpeaker rift_speaker;

// Whether alerts make a sound. Off would be the wrong default for a feature whose
// whole point is being noticed while the screen is dark, but a field device that
// beeps is not always wanted, so it is a setting.
bool rift_sound_on = true;

// Four patterns, still meant to be told apart without looking - but they no longer
// insist equally, which was the complaint. A message is a thing you will deal with
// when you look; a device arriving is the one thing here you might want to act on
// now, and it is the only alert that fires when you are not already reading.
//
// So the rising three-note went to proximity, and the messages dropped an octave
// and most of their volume. Frequency alone did not get there: a low note at full
// scale is still an interruption, which is why play() takes a gain.
//
// A channel message is one note and a DM is two, and the DM is the higher pair -
// enough shape to tell them apart, little enough to ignore.
//
// No sound for an acknowledgement. You are looking at the screen when you send, and
// the delivery state is already there - a tone would be telling you something you
// are already reading.
// Lengthened from 45/55 and 50 ms. A note that short is heard as a click with a
// pitch rather than as a tone - about 8 ms of it was the ramp at each end, and a
// single 50 ms beep was the "short, interrupted" channel alert. These are still
// brief: 160 ms for a direct message, 130 for a channel.
//
// The queue holds 320 ms, so each of these is still handed to the DMA engine in
// one pass and cannot be affected by what the main loop does next.
static const TDeckSpeaker::Step SND_DM[]   = { {523,70}, {659,90} };
static const TDeckSpeaker::Step SND_CHAN[] = { {440,130} };
static const TDeckSpeaker::Step SND_PROX[] = { {880,70}, {1175,70}, {1568,110} };

#define SND_GAIN_MSG   45     // messages: present, not insistent
#define SND_GAIN_PROX 100     // the one alert worth interrupting for

static void riftPlay(const TDeckSpeaker::Step* seq, int n, uint8_t gain) {
  if (rift_sound_on) rift_speaker.play(seq, n, gain);
}
#endif

uint16_t rift_msg_wakes = 0;
uint32_t rift_last_wake_ms = 0;

// Four bytes on SPIFFS: magic, version, flags. Small enough that the write cost
// that dominates the message log does not apply, and it only happens when a
// setting is actually changed.
#define RIFT_SETTINGS_PATH "/rift.cfg"
#define RIFT_SETTINGS_MAGIC0 'R'
#define RIFT_SETTINGS_MAGIC1 'S'
// RIFT_SETTINGS_VERSION and the flag layout live in RiftLogic.h, where the migration
// has tests rather than an argument.

void riftLoadSettings() {
#if defined(ESP32)
  bool migrated = false;
  File f = SPIFFS.open(RIFT_SETTINGS_PATH, "r");
  if (!f) return;
  uint8_t b[4];
  RiftSettings st;
  if (f.read(b, sizeof(b)) == sizeof(b)
      && b[0] == RIFT_SETTINGS_MAGIC0 && b[1] == RIFT_SETTINGS_MAGIC1
      && riftDecodeSettings(b[2], b[3], &st)) {
    riftApplyPalette(st.day_mode);
    rift_screen_always_on = st.always_on;
    migrated = st.migrated;
#ifdef RIFT_SPEAKER
    rift_sound_on = st.sound_on;
#endif
#ifdef RIFT_RADAR
    rift_radar_src = st.radar_src;
    uint8_t nw = 0;
    // Reset, not append. Called once today, which is the only reason writing
    // rf_watch[4..7] past a four-element array has not already happened.
    rf_watch_count = 0;
    if (f.read(&nw, 1) == 1) {
      if (nw > RIFT_WATCH_MAX) nw = RIFT_WATCH_MAX;
      for (int i = 0; i < nw; i++) {
        RfWatch w;
        memset(&w, 0, sizeof(w));
        uint8_t flag = 0;
        // a truncated record is dropped whole rather than half-read, so a file cut
        // short by a failed write cannot produce a watch with a key and no name
        if (f.read(w.key, 6) != 6) break;
        if (f.read(&flag, 1) != 1) break;
        if (f.read((uint8_t*) w.name, sizeof(w.name)) != (int) sizeof(w.name)) break;
        w.name[sizeof(w.name) - 1] = 0;
        w.is_wifi = (flag != 0);
        // present starts false on purpose: a device that was in range when the
        // node was switched off should announce itself again, not be assumed there
        rf_watch[rf_watch_count++] = w;
      }
    }
#endif

    // Channel scopes. Outside the RADAR guard because a scope has nothing to do
    // with the radar, and written outside it too - the two sides have to agree
    // about the file layout for a given build, and they do.
    {
      RiftScopeTable& sc = riftScopes();
      sc.reset();
      uint8_t ns = 0;
      if (f.read(&ns, 1) == 1) {
        if (ns > RIFT_SCOPE_SLOTS) ns = RIFT_SCOPE_SLOTS;
        for (int i = 0; i < ns; i++) {
          uint8_t idx = 0;
          char nm[RIFT_SCOPE_NAME_MAX];
          // Dropped whole rather than half-read, like a watch record: a file cut
          // short by a failed write must not produce a channel scoped to rubbish,
          // which would send its traffic somewhere nobody is listening.
          uint32_t fp = 0;
          if (f.read(&idx, 1) != 1) break;
          if (f.read((uint8_t*) &fp, 4) != 4) break;
          if (f.read((uint8_t*) nm, sizeof(nm)) != (int) sizeof(nm)) break;
          nm[sizeof(nm) - 1] = 0;
          // Through set() rather than a raw append: it validates the slot,
          // canonicalises the name and replaces rather than duplicating. A
          // hand-edited or truncated file could otherwise produce slot 250, or two
          // entries for slot 2 where the lookup found one and clear removed the
          // other.
          sc.set(idx, fp, nm);
        }
      }
    }
  }
  f.close();

  // Rewritten at the new version, once, and only when the file that was read was an
  // older one. It costs a flash write during boot, which is why it is conditional:
  // doing it unconditionally would put a write in every boot to save a branch here.
  //
  // After f.close(), because the save reopens the same path for writing.
  if (migrated) {
    riftSaveSettings();
    riftLogf("settings migrated to v%d", (int) RIFT_SETTINGS_VERSION);
  }
#endif
}

void riftSaveSettings() {
#if defined(ESP32)
  File f = SPIFFS.open(RIFT_SETTINGS_PATH, "w");
  if (!f) return;
  // Encoded by the same function the reader decodes with, so the two cannot disagree
  // about a bit position - which is the failure that would silently move every
  // setting at once.
  RiftSettings st;
  st.day_mode  = rift_day_mode;
  st.always_on = rift_screen_always_on;
  st.migrated  = false;
#ifdef RIFT_RADAR
  st.radar_src = rift_radar_src;
#else
  st.radar_src = RIFT_SETTINGS_SRC_BOTH;
#endif
#ifdef RIFT_SPEAKER
  st.sound_on = rift_sound_on;
#else
  st.sound_on = false;
#endif
  // Every write is checked. This file used to be four bytes of flags, where a failed
  // write was hard to care about; it now carries up to twelve watch identities and
  // their names, so a partial write loses real state. Not staged through a temporary
  // and renamed, for the same reason the message log stopped doing that: remove and
  // rename are two operations on SPIFFS with a window between them holding no file at
  // all, and it buys a guarantee it does not deliver.
  //
  // Nothing is deleted on failure. The reader stops when the file runs out, so a short
  // file keeps its flags and loses only the watches that were not written - which is a
  // better outcome than defaults, and the log line says it happened.
  bool ok = true;
  uint8_t b[4] = { RIFT_SETTINGS_MAGIC0, RIFT_SETTINGS_MAGIC1,
                   RIFT_SETTINGS_VERSION, riftEncodeSettings(st) };
  ok = ok && (f.write(b, sizeof(b)) == sizeof(b));
#ifdef RIFT_RADAR
  // Appended rather than versioned. The reader below stops when the file runs out,
  // so a settings file written before this existed still loads its flags - bumping
  // the version would have discarded a working day/night choice to add a feature.
  uint8_t nw = (uint8_t) rf_watch_count;
  ok = ok && (f.write(&nw, 1) == 1);
  for (int i = 0; i < rf_watch_count && ok; i++) {
    ok = ok && (f.write(rf_watch[i].key, 6) == 6);
    uint8_t flag = rf_watch[i].is_wifi ? 1 : 0;
    ok = ok && (f.write(&flag, 1) == 1);
    ok = ok && (f.write((const uint8_t*) rf_watch[i].name, sizeof(rf_watch[i].name))
                  == sizeof(rf_watch[i].name));
  }
#endif

  // Channel scopes, appended after the watches for the same reason they were
  // appended after the flags: a file written before this existed still loads
  // everything ahead of it, and the reader stops when the file runs out.
  {
    RiftScopeTable& sc = riftScopes();
    uint8_t ns = (uint8_t) sc.count();
    ok = ok && (f.write(&ns, 1) == 1);
    for (int i = 0; i < sc.count() && ok; i++) {
      uint8_t idx = sc.at(i).channel_idx;
      uint32_t fp = sc.at(i).channel_fp;
      ok = ok && (f.write(&idx, 1) == 1);
      ok = ok && (f.write((const uint8_t*) &fp, 4) == 4);
      ok = ok && (f.write((const uint8_t*) sc.at(i).name, RIFT_SCOPE_NAME_MAX)
                    == RIFT_SCOPE_NAME_MAX);
    }
  }

  f.close();
  if (!ok) riftLogf("SETTINGS WRITE FAILED");
#endif
}
int rift_nav_batt_pct = 0;
// Which SYSTEM page is showing, so the nav bar can say 1/2 in the slack where the
// battery percentage normally sits. Only SYSTEM writes it; the other four screens
// never look at it, and the percentage is still on all of them.
int rift_system_page = 0;

void riftApplyPalette(bool day) {
  rift_day_mode = day;
  rift_pal = day ? RIFT_DAY : RIFT_NIGHT;

  // keep the shared roles pointing at the same table
  UIColor::window_bkg    = rift_pal.bg;
  UIColor::title_bkg     = rift_pal.bar;
  UIColor::title_txt     = rift_pal.fg;
  UIColor::primary_txt   = rift_pal.fg;
  UIColor::secondary_txt = rift_pal.mid;
  UIColor::warning_txt   = rift_pal.accent;
  // corp_blue is not blue on this driver - ST7789NativeDisplay assigns it
  // ST77XX_ORANGE and calls it the RIFT accent. The name is upstream's; the boot
  // screen still reads it, so it stays pointed at the accent.
  UIColor::corp_blue     = rift_pal.accent;
  // popup_bkg/popup_txt are deliberately not assigned. Nothing in ui-rift reads
  // them any more: the alert box draws from rift_pal directly, like every other
  // RIFT surface, rather than through a shared role whose one option was `bar` -
  // a band two percent from its own background, which is not a popup fill.
}

RiftBootMark rift_boot_marks[RIFT_BOOT_MARKS];
uint8_t rift_boot_mark_count = 0;

void riftBootMark(const char* name) {
  if (rift_boot_mark_count >= RIFT_BOOT_MARKS) return;
  rift_boot_marks[rift_boot_mark_count].name = name;
  rift_boot_marks[rift_boot_mark_count].at_ms = millis();
  rift_boot_mark_count++;
}

// MeshCore's version string carries a build suffix; the boot screen only wants
// the part before the dash.
static void riftShortMeshCoreVersion(char* out, size_t out_sz) {
  const char* ver = FIRMWARE_VERSION;
  const char* dash = strchr(ver, '-');
  size_t len = dash ? (size_t)(dash - ver) : strlen(ver);
  if (len >= out_sz) len = out_sz - 1;
  memcpy(out, ver, len);
  out[len] = 0;
}

// The wordmark: RIFT with the accent lying in a shallow diagonal cut through the
// letters. Drawn in two places now - the boot screen and the RIFT tab - so the
// geometry lives here instead of being written twice and drifting.
//
// The slope is the design's: y 96 at x=0 falling to y 48 at x=319, about -8.8
// degrees. Kept as an integer rise over run so both callers round the staircase
// identically; a float would put the two marks a pixel apart at some x and nowhere
// would say why.
#define RIFT_SEAM_RISE  12
#define RIFT_SEAM_RUN   78

// The seam's y at px, given that it passes through (ax, ay).
static inline int riftSeamY(int px, int ax, int ay) {
  return ay - (px - ax) * RIFT_SEAM_RISE / RIFT_SEAM_RUN;
}

// The seam from x0 to x1 inclusive, h pixels tall, in whatever colour is set.
//
// Drawn as horizontal runs rather than one rect per column. At this slope the seam
// only steps down once every six or seven pixels, so a per-column loop issued six
// times as many SPI window setups as the pixels needed - which cost nothing on a
// boot screen drawn once, and would cost it on a tab that redraws on a timer over
// the same bus as the radio. The runs are the same staircase the per-column loop
// produced, because the integer division that makes the staircase is unchanged.
static void riftDrawSeam(DisplayDriver& display, int x0, int x1, int ax, int ay, int h) {
  if (x1 < x0) return;
  int run_start = x0;
  int run_y = riftSeamY(x0, ax, ay);
  for (int px = x0 + 1; px <= x1; px++) {
    int y = riftSeamY(px, ax, ay);
    if (y == run_y) continue;
    display.fillRect(run_start, run_y, px - run_start, h);
    run_start = px;
    run_y = y;
  }
  display.fillRect(run_start, run_y, x1 - run_start + 1, h);
}

// The wordmark at (x, y), setTextSize(3), with the seam laid through it from seam_x0
// to seam_x1. The cut's position is derived from the wordmark rather than passed in,
// so it sits in the same place relative to the letters wherever the mark is drawn.
//
// DEVIATION FROM THE SPEC, deliberately, and it applies to both callers. The recipe
// asks for two text draws offset 3px so the halves shear along the seam. That cannot
// be composed with this driver: it has fillRect and drawRect and no clipping, so
// whichever text is drawn second spills across the seam, and the blank that removes
// the spill also removes the half it was meant to keep - in either order. The
// rendering was produced in a browser, where clipping exists. What is here is the
// same cut with the same accent in the same gap, crossing no letter stem, without
// the lateral shear. Adding it needs a hand-pixelled bitmap or a clip the driver
// lacks.
static void riftDrawWordmark(DisplayDriver& display, int x, int y, ColorVal txt,
                             int seam_x0, int seam_x1) {
  const int ax = x - 2, ay = y + 14;   // the cut, relative to the glyphs

  display.setColor(txt);
  display.setTextSize(3);
  display.drawTextLeftAlign(x, y, "RIFT");

  // The gap. 4px, so the 2px accent has a pixel of air either side - which is what
  // keeps it reading as a seam rather than as a line struck through the glyphs.
  // Bounded to the glyphs plus a margin rather than the whole seam: past them it
  // would be blanking background with background.
  display.setColor(rift_pal.bg);
  riftDrawSeam(display, ax, x + 74, ax, ay - 1, 4);

  display.setColor(rift_pal.accent);
  riftDrawSeam(display, seam_x0, seam_x1, ax, ay, 2);
}

void riftDrawBootScreen(DisplayDriver& display, const char* status) {
  const int x = 32;                  // left margin, matching the design
  // fg, not a hardcoded 0xFFFF. This screen paints on rift_pal.bg like every other,
  // so in day mode the wordmark and the status line were the same colour as the
  // field and the boot screen showed two straplines and no name. The RIFT tab has
  // always passed rift_pal.fg for the same mark; this instance was the one that did
  // not, behind a comment noting the shared palette has no white - true, and it has
  // no need of one, because what this wants is the colour of text on the field.
  const ColorVal ink = rift_pal.fg;

  // Wordmark 2c. The seam runs from the left edge to the middle and stops: edge to
  // edge read as a rule with the wordmark sitting on it, where stopping halfway makes
  // the wordmark the thing the line is part of - and it leaves the right half of the
  // screen for the strapline underneath rather than putting a diagonal above it.
  //
  // The RIFT tab makes the opposite choice, and for the same reason: there is nothing
  // to its right, so its seam leaves the screen instead of stopping in mid-air.
  riftDrawWordmark(display, x, 78, ink, 0, display.width() / 2 - 1);

  // wide tracking, as drawn - Adafruit GFX has no letter-spacing control
  display.setColor(UIColor::secondary_txt);
  display.setTextSize(1);
  display.drawTextLeftAlign(x, 118, "R A D I O  I N T E L L I G E N C E");
  // "&", not "$". design/handoff.md gives the two straplines as
  // "RADIO INTELLIGENCE" and "& FIELD TERMINAL" - the ampersand is what joins
  // them into what the name stands for. It arrived here as a dollar sign, which
  // on a screen whose two lines are otherwise identically tracked reads as a
  // shell prompt on one of them, and it is the first thing anyone sees at boot.
  display.drawTextLeftAlign(x, 132, "&  F I E L D  T E R M I N A L");

  if (status != NULL) {
    display.setColor(ink);
    display.drawTextLeftAlign(x, 188, status);
    display.setColor(UIColor::secondary_txt);
    display.drawTextRightAlign(display.width() - x, 188, "1-2 min - not a hang");
  }

  // our version, and the MeshCore it is built on - a fork should say both
  char mc[12];
  riftShortMeshCoreVersion(mc, sizeof(mc));
  char line[48];
  sprintf(line, "RIFT %s - MeshCore %s", RIFT_VERSION, mc);
  display.setColor(UIColor::secondary_txt);
  display.drawTextLeftAlign(x, 212, line);
}

class RiftSplashScreen : public RiftScreen {
  UITask* _task;
  unsigned long dismiss_after;

public:
  RiftSplashScreen(UITask* task) : _task(task) {
    // millis() is near zero here, so this deadline cannot straddle the wrap
    dismiss_after = millis() + BOOT_SCREEN_MILLIS;
  }

  int render(DisplayDriver& display) override {
    // identical to what main.cpp drew during setup(), so the boot sequence does
    // not visibly change shape once the UI task takes over
    riftDrawBootScreen(display, NULL);
    return 200;
  }

  void poll() override {
    if (riftDue((uint32_t) millis(), dismiss_after)) {
      RIFT_MARK("home");
      // First line in the log, so it is never empty and every later timestamp has
      // something to be read against. The slowest phase is the one that mattered
      // here once: a 243-second boot was four minutes of I2C timeouts, and this is
      // the number that would have said so on the first look rather than the fifth.
      {
        int worst = 0;
        uint32_t worst_ms = 0;
        for (int i = 0; i < rift_boot_mark_count; i++) {
          uint32_t prev = (i == 0) ? 0 : rift_boot_marks[i - 1].at_ms;
          uint32_t gap = rift_boot_marks[i].at_ms - prev;
          if (gap > worst_ms) { worst = i; worst_ms = gap; }
        }
        if (rift_boot_mark_count > 0) {
          riftLogf("boot %ums, slowest %s +%ums",
                   (unsigned) rift_boot_marks[rift_boot_mark_count - 1].at_ms,
                   rift_boot_marks[worst].name, (unsigned) worst_ms);
        }
        // The reset reason is on SYSTEM, but only for the boot you are in. In the
        // log it survives the next one, which is what makes a repeating fault
        // legible - a single panic and a panic every ten minutes look identical
        // from one reading.
        esp_reset_reason_t rr = esp_reset_reason();
        const char* why;
        switch (rr) {
          case ESP_RST_POWERON:   why = "power on"; break;
          case ESP_RST_SW:        why = "sw restart"; break;
          case ESP_RST_PANIC:     why = "PANIC"; break;
          case ESP_RST_INT_WDT:   why = "int wdt"; break;
          case ESP_RST_TASK_WDT:  why = "task wdt"; break;
          case ESP_RST_WDT:       why = "wdt"; break;
          case ESP_RST_BROWNOUT:  why = "BROWNOUT"; break;
          case ESP_RST_DEEPSLEEP: why = "deep sleep"; break;
          case ESP_RST_EXT:       why = "ext reset"; break;
          default:                why = "unknown"; break;
        }
        riftLogf("last reset: %s", why);
        // Logged because an unset clock is not an error anywhere and yet it emptied
        // three screens. Standalone is RIFT's normal case, so this is the normal
        // state, not an edge case.
        uint32_t clk = the_mesh.getRTCClock()->getCurrentTime();
        if (clk < 1600000000u) riftLogf("clock unset (%u)", (unsigned) clk);
        else                   riftLogf("clock %02u:%02u", (unsigned) ((clk / 3600u) % 24u),
                                        (unsigned) ((clk / 60u) % 60u));
      }
      _task->gotoHomeScreen();
    }
  }
};

// MESH: home dashboard - mesh/battery status, node count, link stats, radar ping.
class RiftMeshScreen : public RiftScreen {
  UITask* _task;
  NodePrefs* _node_prefs;
  int _tick;
  // recorded at render so a tap hits the box actually drawn, like the COMMS tabs
  // Three actions on this screen now, so their hit boxes are an array and the
  // selected one is tracked for the trackball. Discovery stays index 0, which is
  // what Enter does with nothing selected - the behaviour this screen already had.
  enum HomeBtn { BTN_DISCOVER, BTN_ADVERT_NEAR, BTN_ADVERT_MESH, BTN_COUNT };
  int _btn_x0[BTN_COUNT] = { 0, 0, 0 };
  int _btn_x1[BTN_COUNT] = { 0, 0, 0 };
  int _btn_sel = BTN_DISCOVER;

public:
  RiftMeshScreen(UITask* task, NodePrefs* node_prefs)
     : _task(task), _node_prefs(node_prefs), _tick(0) { }

  int render(DisplayDriver& display) override {

    // The headline is mesh receive activity, which is the question this screen
    // exists to answer. It used to be the USB/BLE companion link - honest about
    // what it measured, but a standalone node with a busy mesh around it read
    // STANDBY forever, and that is the case RIFT is built for.
    //
    // The link has not been dropped, only demoted to a labelled row below.
    //
    // Longest state is "NO SIGNAL": 9 chars at size 3 is 162px from x=2, which
    // clears both the screen edge and the radar box (x >= 160, but y >= 80).
    unsigned long since = millis() - the_mesh.getLastRxMillis();
    int activity = riftMeshActivity(the_mesh.hasHeardMesh(), (uint32_t) since);

    const char* state;
    ColorVal state_col;
    switch (activity) {
      case RIFT_MESH_ACTIVE: state = "ACTIVE";    state_col = rift_pal.ok;     break;
      case RIFT_MESH_IDLE:   state = "IDLE";      state_col = rift_pal.fg;     break;
      case RIFT_MESH_QUIET:  state = "QUIET";     state_col = rift_pal.mid;    break;
      default:               state = "NO SIGNAL"; state_col = rift_pal.accent; break;
    }

    // The protocol, not the screen name - the nav bar carries that. LoRa
    // handhelds are dominated by Meshtastic and a T-Deck looks like one, so
    // saying which mesh stack this is answers a question people actually ask.
    // It is also something the firmware knows for certain, being compiled in.
    //
    // Lowercase, against the all-caps convention of every other piece of chrome
    // here. A URL in capitals reads worse and less like an address. Deliberate.
    //
    // In mid for now, following the label it replaced. MeshCore's brand blue is a
    // separate open item: it needs the real value and a contrast check in both
    // palettes, and a saturated blue is likely to fail on black the way the accent
    // fails on white.
    // Up 14px from where the title bar used to end. This screen has no heading -
    // the nav bar says RIFT - so the masthead sits at the very top instead.
    display.setTextSize(1);
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(2, 4, "meshcore.io");
    display.setTextSize(3);
    display.setColor(state_col);
    display.drawTextLeftAlign(2, 16, state);

    // The wordmark, top right, on the same baseline as the state headline.
    //
    // This corner was empty: the headline is 162px at its longest, the three rows
    // under it are shorter still, and the radar box does not start until y=80 - so
    // roughly 150x76 of the first screen anyone sees carried nothing. The mark is the
    // one on the boot screen at the same size, not a second logo, which is the whole
    // point of a wordmark.
    //
    // Size 3 rather than larger, and in fg rather than the accent, because the
    // headline is the information on this screen and the mark is identity. The accent
    // is already spoken for here - it carries the seam, the radar blip and the
    // NO SIGNAL state - and a mark competing with a state that means "your radio is
    // not hearing anything" would be the wrong thing to win.
    //
    // The seam leaves the right edge instead of stopping short, which is the opposite
    // of the boot screen and for the same reason: there it stops halfway to keep the
    // right half for the strapline, and here there is nothing to its right, so an end
    // in mid-air would be the arbitrary choice. It begins in mid-air on the left at
    // x=196 rather than crossing the screen, because continuing would put a diagonal
    // through the headline and then the LAST RX row.
    riftDrawWordmark(display, 240, 16, rift_pal.fg, 196, display.width() - 1);

    // Show the age and the count, not just the verdict. Every hardware problem
    // on this project was settled by putting the real value on screen, and a
    // threshold is exactly the kind of judgement that is worth being able to
    // check against the number it was derived from.
    char row[40];
    display.setTextSize(1);
    display.setColor(rift_pal.fg);
    if (the_mesh.hasHeardMesh()) {
      char age[RIFT_AGE_BUF_LEN];
      riftFormatAge((uint32_t) since, age, sizeof(age));
      sprintf(row, "LAST RX %s AGO", age);
    } else {
      // nothing since boot points at frequency, SF or antenna - not at a quiet
      // network - so it is worth saying which of the two this is. Same label as
      // the live row, so the eye finds the fact in the same place.
      strcpy(row, "LAST RX none since boot");
    }
    display.drawTextLeftAlign(2, 62, row);

    display.setColor(rift_pal.mid);
    sprintf(row, "RX %u PACKETS", (unsigned) the_mesh.getRxCount());
    display.drawTextLeftAlign(2, 74, row);

    // The companion link, still reported, now labelled for what it measures.
    // hasConnection() returns AbstractUITask::_connected, which only the serial
    // interface sets.
    const char* link = _task->hasConnection() ? "CONNECTED"
                     : (the_mesh.getBLEPin() != 0 ? "PAIRING" : "STANDBY");
    sprintf(row, "USB/BLE %s", link);
    display.drawTextLeftAlign(2, 90, row);

    // radar box, right of the state. Same mechanism as before - three nested
    // rects and one blip at eight discrete positions - just moved off centre.
    int cx = 210, cy = 120;
    display.setColor(rift_pal.rule);
    display.drawRect(cx - 50, cy - 40, 100, 80);
    display.setColor(rift_pal.mid);
    display.drawRect(cx - 33, cy - 27, 66, 54);
    display.drawRect(cx - 16, cy - 13, 32, 26);

    static const int8_t dx[8] = { 0, 35, 50, 35, 0, -35, -50, -35 };
    static const int8_t dy[8] = { -40, -28, 0, 28, 40, 28, 0, -28 };
    int pos = _tick % 8;
    display.setColor(rift_pal.accent);
    display.fillRect(cx + dx[pos] - 2, cy + dy[pos] - 2, 4, 4);
    _tick++;

    char tmp[40];
    display.setTextSize(1);
    display.setColor(rift_pal.fg);
    // "NODES 256" answered neither question: 256 is how many contacts are stored,
    // which says nothing about whether any of them are around right now. Stored comes
    // from the persisted contact table, heard from the session path cache.
    // With the denominator, and in accent when the table is full. "256 STORED" gave
    // nothing to measure against, so a device that had quietly stopped accepting nodes
    // looked identical to one with room - and the symptom of the difference is a NODES
    // screen that stops changing, which reads as a radio or antenna fault.
    // Accent while the table is actually full, which is the state you can act on by
    // deleting a contact. Having once been full is history and does not belong in a
    // colour that means "attend to this".
    sprintf(tmp, "%d/%d STORED %s %d HEARD", the_mesh.getNumContacts(),
            the_mesh.getContactsCapacity(), RIFT_DOT, the_mesh.getPathCacheUsed());
    if (the_mesh.contactsFullNow()) display.setColor(rift_pal.accent);
    display.drawTextLeftAlign(2, 170, tmp);

    // Two numbers with their names. "LINK -80 / -4" was the one place on the
    // screen that needed prior knowledge to read. "--" until something has been
    // heard: the radio's last reading is nothing, not zero.
    if (the_mesh.hasHeardMesh()) {
      sprintf(tmp, "RSSI %.0f  SNR %.0f", radio_driver.getLastRSSI(), radio_driver.getLastSNR());
    } else {
      strcpy(tmp, "RSSI --  SNR --");
    }
    display.drawTextRightAlign(316, 170, tmp);

    // The radio line, unless a tropo opening is running - in which case that is
    // the more interesting thing this screen can say, and the frequency has not
    // changed since the last time it was read.
    {
      RiftTropo& tr = riftTropoState();
      if (tr.active) {
        display.setColor(rift_pal.accent);
        sprintf(tmp, "TROPO OPEN %s peak %d hops", RIFT_DOT, (int) tr.peak_hops);
      } else {
        display.setColor(rift_pal.mid);
        sprintf(tmp, "%.3fMHz  SF%d  %ddBm", _node_prefs->freq, _node_prefs->sf, _node_prefs->tx_power_dbm);
      }
      display.drawTextCentered(display.width() / 2, 182, tmp);
    }

    // The one action on this screen, drawn as a button because it is one - an
    // outlined box with the accent, rather than a line of hint text that happens to
    // be pressable. Both ENTER and a tap on it start a round.
    //
    // Zero-hop is in the label on purpose. It asks direct neighbours only, and a
    // name that implied it searched the mesh would be a promise it cannot keep.
    //
    // Both adverts moved here from the SYSTEM action list. They are the two
    // things you do when a node cannot hear you, which is a question this screen
    // already answers - it is the one showing how many nodes are stored and heard
    // - and reaching them meant leaving it for a menu two pages deep.
    //
    // "0-HOP" and the near/mesh split stay in the labels. An advert to direct
    // neighbours and an advert flooded through the mesh are different amounts of
    // airtime and different promises, and a button that hid which one it sent
    // would be the wrong kind of tidy.
    {
      const char* labels[BTN_COUNT];
      labels[BTN_DISCOVER]    = the_mesh.isDiscovering() ? "DISCOVERING..." : "DISCOVER 0-HOP";
      labels[BTN_ADVERT_NEAR] = "ADVERT NEAR";
      labels[BTN_ADVERT_MESH] = "ADVERT MESH";

      int w[BTN_COUNT], total = 0;
      for (int i = 0; i < BTN_COUNT; i++) {
        w[i] = (int) strlen(labels[i]) * RIFT_CHAR_W + 12;
        total += w[i];
      }
      const int gap = 8;
      int bx = (display.width() - total - gap * (BTN_COUNT - 1)) / 2;

      for (int i = 0; i < BTN_COUNT; i++) {
        bool sel = (i == _btn_sel);
        bool busy = (i == BTN_DISCOVER && the_mesh.isDiscovering());
        // The selected button is filled rather than only outlined: in sunlight the
        // difference between two accent outlines disappears, and which one Enter
        // will press has to survive that.
        // The accent fills the selected button and no longer outlines the other
        // two. It used to outline all three, so it framed everything and filled
        // one, and the fill was carrying the whole of "this is the one Enter will
        // press" while the accent said "these are buttons" three times. rule draws
        // a button; the accent says which.
        if (sel) {
          display.setColor(rift_pal.accent);
          display.fillRect(bx, DISCOVER_BTN_Y, w[i], 14);
        } else {
          display.setColor(rift_pal.rule);
          display.drawRect(bx, DISCOVER_BTN_Y, w[i], 14);
        }
        display.setColor(sel ? rift_pal.on_accent : (busy ? rift_pal.accent : rift_pal.fg));
        display.drawTextCentered(bx + w[i] / 2, DISCOVER_BTN_Y + 3, labels[i]);
        _btn_x0[i] = bx;
        _btn_x1[i] = bx + w[i];
        bx += w[i] + gap;
      }

      // One line for the selected button, carrying the warning the SYSTEM list
      // used to attach to these actions. Moving them here would otherwise have
      // quietly dropped the only place that said a neighbours-only advert is not
      // enough before a first direct message.
      const char* note = NULL;
      switch (_btn_sel) {
        case BTN_ADVERT_NEAR: note = "direct RF only - use MESH before a first DM"; break;
        case BTN_ADVERT_MESH: note = "reaches nodes past direct range - more airtime"; break;
        default:              note = "asks direct neighbours only"; break;
      }
      display.setColor(rift_pal.dim);
      display.drawTextCentered(display.width() / 2, DISCOVER_BTN_Y + 16, note);
    }

    renderNavBar(display, RIFT_NAV_MESH);
    // while a round is open the button label and the result count both move, so
    // refresh faster than the blip alone would need
    return the_mesh.isDiscovering() ? 300 : 700;
  }

  static const int DISCOVER_BTN_Y = 198;

  // Left and right already move between screens, so the row of buttons is walked
  // with up and down. They did nothing here before.
  bool handleInput(char c) override {
    if (c == KEY_ENTER) { press(_btn_sel); return true; }
    if (c == KEY_UP)   { if (_btn_sel > 0) _btn_sel--; return true; }
    if (c == KEY_DOWN) { if (_btn_sel + 1 < BTN_COUNT) _btn_sel++; return true; }
    if (c == KEY_NEXT || c == KEY_RIGHT) { _task->cycleNavScreen(1); return true; }
    if (c == KEY_PREV || c == KEY_LEFT) { _task->cycleNavScreen(-1); return true; }
    return false;
  }

  bool handleTouch(int x, int y) override {
    if (y < DISCOVER_BTN_Y - 2 || y > DISCOVER_BTN_Y + 14) return false;
    for (int i = 0; i < BTN_COUNT; i++) {
      if (_btn_x0[i] <= 0) continue;                     // not drawn yet
      if (x < _btn_x0[i] || x > _btn_x1[i]) continue;
      _btn_sel = i;      // a tap also moves the selection, so Enter repeats it
      press(i);
      return true;
    }
    return false;
  }

  void press(int which) {
    switch (which) {
      case BTN_ADVERT_NEAR: {
        // Other nodes cannot decrypt a direct message from a node they have never
        // heard an advert from - they look the sender up in their contacts and
        // silently drop it - so this is a prerequisite for two-way DMs.
        _task->notify(UIEventType::ack);
        bool ok = the_mesh.advert();
        riftLogf("advert neighbours: %s", ok ? "sent" : "FAILED");
        _task->showAlert(ok ? "Advert sent (direct)" : "Advert failed", 1200);
        break;
      }
      case BTN_ADVERT_MESH: {
        // reaches nodes beyond direct RF range, which is what they need before
        // they can decrypt a DM from us
        _task->notify(UIEventType::ack);
        bool ok = the_mesh.advertFlood();
        riftLogf("advert whole mesh: %s", ok ? "sent" : "FAILED");
        _task->showAlert(ok ? "Advert flooded!" : "Advert failed", 1200);
        break;
      }
      default:
        _task->startRepeaterDiscovery();
        break;
    }
  }
};

// SYSTEM: live keyboard/trackball diagnostics - proves the Step 2 input
// drivers work end-to-end. Real settings/diagnostics content lands later.
class RiftSystemScreen : public RiftScreen {
  UITask* _task;

  // A small action menu rather than hidden letter shortcuts - discoverable, and
  // it leaves the printable keys free for the text fields.
  enum Mode { MENU, EDIT_NAME, CH_NAME, CH_KEY_CHOICE, CH_KEY_ENTRY, CH_SHOW_KEY,
              CH_DELETE, CH_DELETE_CONFIRM, LOG, SET_TIME, RXLOG,
              SCOPE_PICK, SCOPE_ENTRY };
  // The two advert actions used to head this list. They live on the home screen
  // now, as buttons beside DISCOVER - that screen is the one showing how many
  // nodes are stored and heard, so it is where you already are when the answer
  // is "send an advert". Moved rather than copied: the same action reachable from
  // two places is two code paths that drift.
  enum Item { IT_NAME, IT_CHANNEL, IT_DELCHANNEL, IT_SCOPE,
              IT_PATHMODE, IT_SCREEN, IT_SOUND, IT_DAYMODE, IT_SETTIME, IT_LOG, IT_RXLOG,
              IT_COUNT };

  int _log_scroll = 0;   // 0 = pinned to the newest line
  int _drag_residual = 0;   // finger travel not yet worth a row

  // 0 = actions, 1 = readings. Two pages rather than two columns with scrollbars:
  // neither column got more room that way, and the right one is the one that grows
  // every time a diagnostic is added.
  int _page = 0;
  int _rx_scroll = 0;    // 0 = pinned to the newest packet

  // Where each action row was drawn. The selected row grows a warning line beneath
  // it, so the rows below it move - a fixed pitch selected the wrong action, which
  // on this screen means activating Delete channel instead of Add channel.
  int _item_y[IT_COUNT];

  // most labels are fixed; the path-mode row shows its current value
  void itemLabel(int i, char* buf, size_t len) {
    static const char* FIXED[] = {
      "Edit node name",
      "Add channel",
      "Delete channel",
      "Channel scope",
    };
    if (i == IT_RXLOG) {
      // Both totals in the label, so the split is visible without opening it. A node
      // that has heard thousands and sent nothing is a node whose transmit path is
      // broken, and that was previously indistinguishable from a quiet one.
      snprintf(buf, len, "View air log (%u rx, %u tx)",
               (unsigned) riftRxLog().total, (unsigned) riftRxLog().total_tx);
    } else if (i == IT_SETTIME) {
      // the current value in the label, so a wrong clock is visible without having
      // to open the editor to find out
      uint32_t now = the_mesh.getRTCClock()->getCurrentTime();
      if (now < 1600000000u) {
        StrHelper::strncpy(buf, "Set time & date (unset)", len);
      } else {
        int y, mo, d, h, mi;
        riftCivilFromEpoch(now, &y, &mo, &d, &h, &mi);
        snprintf(buf, len, "Set time (%02d:%02d)", h, mi);
      }
    } else if (i == IT_LOG) {
      snprintf(buf, len, "View log (%d)", riftLog().count);
    } else if (i == IT_SCREEN) {
      // A charger that does not enumerate as a USB host reads as battery, so the
      // display cannot tell it is powered. This is the switch that does not
      // depend on detecting anything.
      snprintf(buf, len, "Screen: %s", rift_screen_always_on ? "always on" : "auto");
    } else if (i == IT_SOUND) {
#ifdef RIFT_SPEAKER
      // Named for what it does rather than for the hardware: there is no buzzer on
      // this board, the tones come out of the I2S amplifier, and the user does not
      // need to know that to decide.
      snprintf(buf, len, "Alert sound: %s", rift_sound_on ? "on" : "off");
#else
      snprintf(buf, len, "Alert sound: unavailable");
#endif
    } else if (i == IT_DAYMODE) {
      // the design binds this to backlight level; this panel's backlight is on or
      // off with no level to read, so it is an explicit choice instead
      snprintf(buf, len, "Display: %s", rift_day_mode ? "day" : "night");
    } else if (i == IT_PATHMODE) {
      uint8_t mode = the_mesh.getNodePrefs()->path_hash_mode;
      snprintf(buf, len, "Path hash size: %d byte%s", mode + 1, mode == 0 ? "" : "s");
    } else {
      StrHelper::strncpy(buf, FIXED[i], len);
    }
  }

  // shared by render() and handleTouch(), so touch targets follow the layout.
  // Up 14 from 34 when the title bar went: this screen has no heading, because
  // the nav bar already says SYSTEM, and the strip it left behind was empty.
  static const int MENU_TOP = 20;

public:
  // True while a text field is open or a generated key is on screen. A message
  // popup over either of those loses half-typed input or a secret being read.
  bool isModal() const override { return _mode != MENU; }
private:

  Mode _mode;
  int _sel;
  RiftTextInput _edit;
  char _ch_name[32];       // held while the key is being chosen
  char _ch_key[48];        // generated key, shown so it can be typed elsewhere
  int  _del_sel = 0;       // index into _del_idx while choosing what to delete
  int  _del_count = 0;
  uint8_t _del_idx[MAX_GROUP_CHANNELS];   // slot numbers, so the list can skip gaps
public:
  // Called when the user navigates away. Leaving by trackball or keyboard went
  // through this screen's own handling, but a tap on the nav bar changes screen
  // from UITask without asking, and the generated key stayed in _ch_key - so
  // coming back to SYSTEM redisplayed a secret the user had already dismissed.
  //
  // A message popup is not navigating away, and must not reach here: it used to,
  // and wiped the key mid-read. That is now riftScreenTransition()'s job rather
  // than a condition at the call site.
  void onLeave() override {
    memset(_ch_key, 0, sizeof(_ch_key));
    memset(_edit.buf, 0, sizeof(_edit.buf));
    _edit.len = 0;
    _mode = MENU;
  }
private:
  int _key_choice;         // 0 = generate, 1 = enter existing

  void activate() {
    switch (_sel) {
      case IT_PATHMODE: {
        // 0..2 maps to 1..3 bytes per path hash. Bigger hashes mean fewer
        // collisions in a busy mesh, at slightly larger packets - and only
        // affects flood-sent traffic, since direct sends carry a stored path.
        NodePrefs* prefs = the_mesh.getNodePrefs();
        prefs->path_hash_mode = (prefs->path_hash_mode + 1) % 3;
        the_mesh.savePrefs();
        char msg[40];
        sprintf(msg, "Path hash: %d byte%s", prefs->path_hash_mode + 1,
                prefs->path_hash_mode == 0 ? "" : "s");
        _task->showAlert(msg, 1400);
        break;
      }

      case IT_DAYMODE:
        riftApplyPalette(!rift_day_mode);
        riftSaveSettings();
        _task->showAlert(rift_day_mode ? "Day mode" : "Night mode", 1200);
        break;

      case IT_SCREEN:
        rift_screen_always_on = !rift_screen_always_on;
        riftSaveSettings();
        _task->showAlert(rift_screen_always_on ? "Screen stays on" : "Screen sleeps", 1400);
        break;

      case IT_SOUND:
#ifdef RIFT_SPEAKER
        rift_sound_on = !rift_sound_on;
        riftSaveSettings();
        // The proximity tone, not the message one: this exists to prove the speaker
        // works, so it should be the most audible pattern rather than the most
        // discreet. A quiet beep failing to be heard proves nothing.
        if (rift_sound_on) riftPlay(SND_PROX, (int) (sizeof(SND_PROX) / sizeof(SND_PROX[0])), SND_GAIN_PROX);
        _task->showAlert(rift_sound_on ? "Sound on" : "Sound off", 1200);
        riftLogf("sound %s", rift_sound_on ? "on" : "off");
#else
        _task->showAlert("No speaker in this build", 1400);
#endif
        break;

      case IT_DELCHANNEL:
        _del_sel = 0;
        collectDeletable();
        _mode = CH_DELETE;
        break;

      case IT_SCOPE:
        collectScopable();
        _del_sel = 0;
        _mode = SCOPE_PICK;
        break;

      case IT_LOG:
        _log_scroll = 0;   // open at the newest, which is what you came to see
        _mode = LOG;
        break;

      case IT_RXLOG:
        _rx_scroll = 0;
        _mode = RXLOG;
        break;

      case IT_SETTIME: {
        // Prefilled with the current reading rather than blank: correcting four
        // digits of a wrong year is less work than typing sixteen characters, and
        // it shows the format by example instead of only describing it.
        uint32_t now = the_mesh.getRTCClock()->getCurrentTime();
        char initial[20];
        if (now < 1600000000u) {
          StrHelper::strncpy(initial, "2026-01-01 12:00", sizeof(initial));
        } else {
          int y, mo, d, h, mi;
          riftCivilFromEpoch(now, &y, &mo, &d, &h, &mi);
          snprintf(initial, sizeof(initial), "%04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
        }
        _edit.begin(initial, 16);
        _mode = SET_TIME;
        break;
      }

      case IT_NAME:
        _edit.begin(the_mesh.getNodeName(), sizeof(((NodePrefs*)0)->node_name) - 1);
        _mode = EDIT_NAME;
        break;

      case IT_CHANNEL:
        // one shorter than the buffer allows: addGroupChannelHashtag() prepends
        // '#', and the normalised string is what the key is derived from, so a
        // name that only fits without the hash would key a different channel
        _edit.begin("", sizeof(_ch_name) - 2);
        _mode = CH_NAME;
        break;
    }
  }

  void commitName() {
    if (_edit.len == 0) {
      _task->showAlert("Name can't be empty", 1200);
      return;
    }
    NodePrefs* prefs = the_mesh.getNodePrefs();
    StrHelper::strncpy(prefs->node_name, _edit.buf, sizeof(prefs->node_name));
    the_mesh.savePrefs();
    _mode = MENU;
    // the new name is what outgoing channel messages are signed with, and what
    // other nodes will show once they hear the next advert
    _task->showAlert("Name saved - send advert", 1500);
  }

  // 0 = hashtag (key derived from name), 1 = random private key, 2 = paste key
  void finishChannel(int kind) {
    int idx;
    switch (kind) {
      case 0:  idx = the_mesh.addGroupChannelHashtag(_ch_name); _ch_key[0] = 0; break;
      case 1:  idx = the_mesh.addGroupChannelRandom(_ch_name, _ch_key, sizeof(_ch_key)); break;
      default: idx = the_mesh.addGroupChannelFromBase64(_ch_name, _edit.buf); _ch_key[0] = 0; break;
    }

    if (idx < 0) {
      // either the pasted key wasn't a valid 128/256-bit base64 value, or every
      // channel slot is taken
      _task->showAlert(kind == 2 ? "Bad key or no slot" : "No free channel slot", 1800);
      _mode = MENU;
      return;
    }

    // deletion was logged and addition was not, which left the log unable to
    // explain a slot number appearing or a channel colour changing
    static const char* KIND[] = { "hashtag", "random key", "pasted key" };
    riftLogf("channel + slot%d %s (%s)", idx, _ch_name,
             KIND[(kind < 0 || kind > 2) ? 0 : kind]);

    if (kind == 1) {
      _mode = CH_SHOW_KEY;   // let the user read the key off the screen
    } else {
      _mode = MENU;
      _task->showAlert("Channel added", 1500);
    }
  }

  int renderChannelName(DisplayDriver& display) {
    renderHeading(display, "NEW CHANNEL");
    display.setTextSize(1);
    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, 30, "Channel name:");
    display.drawTextLeftAlign(4, 96, "A '#' is added for hashtag channels.");
    _edit.render(display, 4, 54, display.width() - 8);
    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, 84, "ENTER next   BACKSPACE delete / back");
    renderNavBar(display, RIFT_NAV_SYSTEM);
    return 1000;
  }

  int renderKeyChoice(DisplayDriver& display) {
    renderHeading(display, "CHANNEL KEY");
    display.setTextSize(1);

    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, 30, "A channel is shared by its key.");

    const char* opts[3] = { "Hashtag - open topic", "Private - new random key",
                            "Private - paste a key" };
    int y = 56;
    for (int i = 0; i < 3; i++, y += RIFT_LINE_H) {
      bool sel = (i == _key_choice);
      display.setColor(sel ? UIColor::title_txt : UIColor::secondary_txt);
      display.setCursor(4, y);
      display.print(sel ? "> " : "  ");
      display.print(opts[i]);
    }

    // the difference that matters is secrecy, not encryption - all three are
    // AES encrypted on air
    if (_key_choice == 0) {
      display.setColor(UIColor::warning_txt);
      display.drawTextLeftAlign(4, y + 8, "Key comes from the name: anyone who");
      display.drawTextLeftAlign(4, y + 20, "knows it can read. Not secret.");
    } else if (_key_choice == 1) {
      display.setColor(UIColor::secondary_txt);
      display.drawTextLeftAlign(4, y + 8, "Secret. Key is shown so you can");
      display.drawTextLeftAlign(4, y + 20, "enter it on your other nodes.");
    } else {
      display.setColor(UIColor::secondary_txt);
      display.drawTextLeftAlign(4, y + 8, "Joins a private channel someone");
      display.drawTextLeftAlign(4, y + 20, "else created.");
    }

    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, y + 40, "up/down select  ENTER ok  BACKSPACE back");

    renderNavBar(display, RIFT_NAV_SYSTEM);
    return 1000;
  }

  int renderKeyEntry(DisplayDriver& display) {
    renderHeading(display, "CHANNEL KEY");
    display.setTextSize(1);
    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, 30, "Paste the base64 key (24 or 44 chars):");
    _edit.render(display, 4, 54, display.width() - 8);
    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, 90, "ENTER add   BACKSPACE delete / back");
    renderNavBar(display, RIFT_NAV_SYSTEM);
    return 1000;
  }

  // Every channel, Public included. A region owning its own Public is exactly the
  // case a scope exists for, and unlike deletion there is nothing irreversible
  // about giving one a name.
  void collectScopable() {
    _del_count = 0;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      ChannelDetails ch;
      if (!the_mesh.getChannel(i, ch) || ch.name[0] == 0) continue;
      _del_idx[_del_count++] = (uint8_t) i;
    }
    if (_del_sel >= _del_count) _del_sel = _del_count > 0 ? _del_count - 1 : 0;
  }

  int renderScopeList(DisplayDriver& display) {
    renderHeading(display, "CHANNEL SCOPE");
    display.setTextSize(1);

    if (_del_count == 0) {
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(4, 40, "No channels.");
      display.drawTextLeftAlign(4, 64, "BACKSPACE back");
      renderNavBar(display, RIFT_NAV_SYSTEM);
      return 1000;
    }

    int y = 34;
    char tmp[72];
    for (int i = 0; i < _del_count && y < 176; i++, y += RIFT_LINE_H) {
      ChannelDetails ch;
      if (!the_mesh.getChannel(_del_idx[i], ch)) continue;
      char nm[sizeof(ch.name)];
      riftTranslateUTF8(nm, ch.name, sizeof(nm));

      const char* sc = riftScopes().nameFor(_del_idx[i], riftChannelConv(_del_idx[i]).channel_fp);
      // Truncated to the column, because "%-16s" only pads - a 20-character channel
      // name pushed the scope four cells right and the second column stopped being
      // one. A name that does not fit loses its tail; a column that does not hold
      // loses every row below it.
      char col[17];
      StrHelper::strncpy(col, nm, sizeof(col));
      snprintf(tmp, sizeof(tmp), "%-16s %s", col, sc ? sc : "(node default)");

      if (i == _del_sel) {
        display.setColor(rift_pal.accent);
        display.fillRect(0, y - 2, display.width(), 12);
        display.setColor(rift_pal.on_accent);
      } else {
        display.setColor(rift_pal.fg);
      }
      display.drawTextLeftAlign(4, y, tmp);
    }

    display.setColor(rift_pal.dim);
    display.drawTextLeftAlign(4, 190, "A scope keeps a channel inside a region.");
    display.drawTextLeftAlign(4, 202, "ENTER edit   BACKSPACE back");
    renderNavBar(display, RIFT_NAV_SYSTEM);
    return 1000;
  }

  int renderScopeEntry(DisplayDriver& display) {
    renderHeading(display, "CHANNEL SCOPE");
    display.setTextSize(1);
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(4, 40, "Region name. Empty clears it back to");
    display.drawTextLeftAlign(4, 52, "the node default.");
    display.drawTextLeftAlign(4, 76, "Anyone using the same name reaches the");
    display.drawTextLeftAlign(4, 88, "same region - the key is the name.");
    _edit.render(display, 4, 116, display.width() - 8);
    display.setColor(rift_pal.dim);
    display.drawTextLeftAlign(4, 202, "ENTER save   BACKSPACE delete / back");
    renderNavBar(display, RIFT_NAV_SYSTEM);
    return 1000;
  }

  // Slot 0 is Public and is not offered - see MyMesh::removeChannel().
  void collectDeletable() {
    _del_count = 0;
    for (int i = 1; i < MAX_GROUP_CHANNELS; i++) {
      ChannelDetails ch;
      if (!the_mesh.getChannel(i, ch) || ch.name[0] == 0) continue;
      _del_idx[_del_count++] = (uint8_t) i;
    }
    if (_del_sel >= _del_count) _del_sel = _del_count > 0 ? _del_count - 1 : 0;
  }

  int renderDeleteList(DisplayDriver& display) {
    renderHeading(display, "DELETE CHANNEL");
    display.setTextSize(1);

    if (_del_count == 0) {
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(4, 40, "No channels to delete.");
      display.drawTextLeftAlign(4, 64, "Public cannot be removed - it is the");
      display.drawTextLeftAlign(4, 76, "only channel a new node can talk on.");
      display.drawTextLeftAlign(4, 100, "BACKSPACE back");
      renderNavBar(display, RIFT_NAV_SYSTEM);
      return 1000;
    }

    int y = 34;
    for (int i = 0; i < _del_count && y < 180; i++, y += RIFT_LINE_H) {
      ChannelDetails ch;
      if (!the_mesh.getChannel(_del_idx[i], ch)) continue;
      char nm[sizeof(ch.name)];
      riftTranslateUTF8(nm, ch.name, sizeof(nm));

      if (i == _del_sel) {
        display.setColor(rift_pal.accent);
        display.fillRect(0, y - 2, display.width(), 12);
        display.setColor(rift_pal.on_accent);
      } else {
        display.setColor(rift_pal.fg);
      }
      display.drawTextLeftAlign(4, y, nm);
    }

    display.setColor(rift_pal.dim);
    display.drawTextLeftAlign(4, 196, "up/down choose, ENTER delete, BACKSPACE back");
    renderNavBar(display, RIFT_NAV_SYSTEM);
    return 1000;
  }

  int renderDeleteConfirm(DisplayDriver& display) {
    renderHeading(display, "DELETE CHANNEL");
    display.setTextSize(1);

    ChannelDetails ch;
    char nm[sizeof(ch.name)] = "";
    if (_del_sel < _del_count && the_mesh.getChannel(_del_idx[_del_sel], ch)) {
      riftTranslateUTF8(nm, ch.name, sizeof(nm));
    }

    display.setColor(rift_pal.fg);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "Delete \"%s\"?", nm);
    display.drawTextLeftAlign(4, 44, tmp);

    display.setColor(rift_pal.mid);
    // Said as the code behaves. This promised the messages would stay while the
    // handler purged them, which is the one kind of wrong a confirmation screen
    // must never be - the user agreed to something else than what happens.
    //
    // The purge is kept rather than the promise: entries written before channel
    // identity existed cannot prove which channel they belong to, and leaving
    // them would let them attach to whatever occupies the slot next.
    display.drawTextLeftAlign(4, 70, "This channel's messages are deleted");
    display.drawTextLeftAlign(4, 82, "too. The key is gone from this device");
    display.drawTextLeftAlign(4, 94, "- rejoining needs it again.");

    display.setColor(rift_pal.accent);
    display.drawTextLeftAlign(4, 124, "ENTER deletes");
    display.setColor(rift_pal.dim);
    display.drawTextLeftAlign(4, 140, "BACKSPACE cancels");

    renderNavBar(display, RIFT_NAV_SYSTEM);
    return 1000;
  }

  int renderShowKey(DisplayDriver& display) {
    renderHeading(display, "CHANNEL ADDED");
    display.setTextSize(1);

    display.setColor(UIColor::title_txt);
    char tmp[48];
    sprintf(tmp, "\"%s\" created", _ch_name);
    display.drawTextLeftAlign(4, 30, tmp);

    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, 52, "Enter this key on your other nodes");
    display.drawTextLeftAlign(4, 64, "to join the same channel:");

    display.setColor(UIColor::primary_txt);
    wrapText(_ch_key, display.width() - 8, 88, &display, 4);

    display.setColor(UIColor::warning_txt);
    display.drawTextLeftAlign(4, 130, "Write it down - not shown again.");

    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, 156, "ENTER done");

    renderNavBar(display, RIFT_NAV_SYSTEM);
    return 1000;
  }

  // Newest at the bottom, the way a terminal reads, and scrolled by line rather
  // than by page: the interesting thing is usually the last few lines and their
  // order relative to each other.
  int renderLog(DisplayDriver& display) {
    renderHeading(display, "LOG");
    display.setTextSize(1);

    const int TOP = 30, BOTTOM = 198;
    const int TEXT_X = 50;
    const int TEXT_W = display.width() - TEXT_X - 4;
    const int PER_ROW = TEXT_W / RIFT_CHAR_W;

    if (riftLog().count == 0) {
      display.setColor(rift_pal.dim);
      display.drawTextLeftAlign(4, TOP, "nothing logged yet");
    }

    // Clamped here, and this was a regression: the first version of this screen
    // clamped and the rewrite for wrapped lines dropped it. With no clamp, paging
    // past the oldest line left _log_scroll beyond count, peek() returned NULL on
    // the first iteration, and the screen went blank with no way to tell that it
    // was scroll position rather than an empty log.
    if (_log_scroll > riftLog().count - 1) _log_scroll = riftLog().count - 1;
    if (_log_scroll < 0) _log_scroll = 0;

    // Long lines wrap to a second row rather than being cut at the right edge.
    // They were ellipsized, which lost the end of every message - and a log line
    // whose message is missing is the half that mattered. Entries are therefore
    // one or two rows tall, so the loop walks upward by each entry's own height
    // the way the COMMS history does, instead of assuming a fixed pitch.
    int y = BOTTOM - RIFT_LINE_H;
    for (int i = 0; ; i++) {
      const RiftEventLog::Line* l = riftLog().peek(_log_scroll + i);
      if (l == NULL) break;

      // Translated before it is measured, and that ordering is the point. The log
      // carries real message text, so it carries UTF-8 - and this screen was
      // measuring and splitting raw bytes, which both drew Nordic characters as
      // rubbish and could cut a two-byte sequence in half at the wrap. COMMS has
      // always translated first; this did not, because it was written as a
      // diagnostic that only ever held ASCII, and then message text was added to it.
      //
      // After translation one byte is one glyph, so a byte count is a column count
      // and the wrap arithmetic below is correct by construction. The buffer is
      // larger than the source because an emoji translates to a short ASCII word.
      char shown[RIFT_LOG_TEXT * 2];
      riftTranslateUTF8(shown, l->text, sizeof(shown));
      int len = (int) strlen(shown);
      int split = 0;                       // 0 = fits on one row
      if (len > PER_ROW) {
        // break on the last space that still fits, so a word is not cut in half;
        // a single long token with no space falls back to a hard split
        split = PER_ROW;
        for (int k = PER_ROW; k > PER_ROW / 2; k--) {
          if (shown[k] == ' ') { split = k; break; }
        }
      }

      int rows = split > 0 ? 2 : 1;
      int top_y = y - (rows - 1) * RIFT_LINE_H;
      if (top_y < TOP) break;              // ran out of room going up

      char stamp[12];
      snprintf(stamp, sizeof(stamp), "%u.%u", (unsigned) (l->at_ms / 1000u),
               (unsigned) ((l->at_ms % 1000u) / 100u));
      display.setColor(rift_pal.dim);
      display.drawTextRightAlign(44, top_y, stamp);

      // Failures in the accent so they can be found by scanning rather than by
      // reading. Upper case is the marker the log lines themselves use.
      // "no ack" joins FAILED as a line worth finding by scanning rather than by
      // reading. Lower case because it is not a failure this device can be sure of.
      bool bad = (strstr(shown, "FAILED") != NULL) || (strstr(shown, "no ack") != NULL);
      display.setColor(bad ? rift_pal.accent : rift_pal.fg);

      if (rows == 1) {
        display.drawTextLeftAlign(TEXT_X, top_y, shown);
      } else {
        char head[sizeof(shown)];
        int n = split;
        memcpy(head, shown, n);
        head[n] = 0;
        display.drawTextLeftAlign(TEXT_X, top_y, head);
        // the continuation is indented, so a wrapped line cannot be mistaken for
        // two separate events at the same timestamp
        const char* rest = shown + split;
        while (*rest == ' ') rest++;
        display.drawTextEllipsized(TEXT_X + RIFT_CHAR_W * 2, top_y + RIFT_LINE_H,
                                   TEXT_W - RIFT_CHAR_W * 2, rest);
      }

      y = top_y - RIFT_LINE_H;
    }

    display.setColor(rift_pal.rule);
    display.fillRect(0, 202, display.width(), 1);
    display.setColor(rift_pal.dim);
    display.drawTextLeftAlign(4, 208, "up/down line  L/R page  ENTER back");

    char foot[28];
    if (riftLog().dropped > 0) {
      snprintf(foot, sizeof(foot), "%d +%u lost", riftLog().count,
               (unsigned) riftLog().dropped);
    } else {
      snprintf(foot, sizeof(foot), "%d", riftLog().count);
    }
    display.drawTextRightAlign(display.width() - 2, 208, foot);

    renderNavBar(display, RIFT_NAV_SYSTEM);
    return 1000;
  }

  // Every packet the radio hears, live.
  //
  // The event log says what happened to messages; this says what arrived on the air,
  // including everything not addressed to this node - other people's traffic, adverts,
  // acks, routed packets passing through. It is the difference between "my messages
  // are not arriving" and "this radio is not hearing anything", which are two
  // problems and were previously one symptom.
  int renderRxLog(DisplayDriver& display) {
    renderHeading(display, "AIR LOG");
    display.setTextSize(1);

    RiftRxLog& log = riftRxLog();
    char tmp[64];

    // Two totals, beside the heading rather than on the header row. Received and sent
    // are different questions - a node that has heard 4000 and sent 3 is in a very
    // different situation from one that has sent 400 - and the pair is wide enough
    // that on the header row it ran into the TYPE column.
    display.setColor(rift_pal.mid);
    snprintf(tmp, sizeof(tmp), "%u rx  %u tx", (unsigned) log.total,
             (unsigned) log.total_tx);
    display.drawTextRightAlign(318, 2, tmp);

    // drawn at the same x as the data below it, not as one right-aligned string:
    // a header that does not sit over its column is worse than no header
    display.drawTextLeftAlign(54, 20, "TYPE");
    display.drawTextLeftAlign(110, 20, "RT HOP");
    // One header over both value columns, because no per-column header is true for
    // both directions: a receive fills them with RSSI and SNR, a transmit with the
    // air time it spent. Naming all three is honest where naming two would have left
    // half the rows sitting under the wrong word.
    display.drawTextRightAlign(262, 20, "RSSI SNR / AIR");
    display.drawTextRightAlign(316, 20, "LEN");

    display.setColor(rift_pal.rule);
    display.fillRect(0, 32, display.width(), 1);

    if (log.count == 0) {
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(2, 40, "Nothing on the air yet.");
      display.setColor(rift_pal.dim);
      display.drawTextLeftAlign(2, 56, "Every packet heard and every packet sent,");
      display.drawTextLeftAlign(2, 68, "not only the ones meant for this node.");
      renderNavBar(display, RIFT_NAV_SYSTEM);
      return 400;
    }

    if (_rx_scroll > log.count - 1) _rx_scroll = log.count - 1;
    if (_rx_scroll < 0) _rx_scroll = 0;

    const int TOP = 38, BOTTOM = 196;
    int rows = (BOTTOM - TOP) / RIFT_LINE_H;
    int y = TOP;
    for (int i = 0; i < rows; i++, y += RIFT_LINE_H) {
      const RiftRxLog::Entry* e = log.peek(_rx_scroll + i);
      if (e == NULL) break;

      // seconds since boot, like the event log, so the two can be read against each
      // other - and monotonic, so it holds with no clock set
      display.setColor(rift_pal.dim);
      snprintf(tmp, sizeof(tmp), "%u.%u", (unsigned) (e->at_ms / 1000u),
               (unsigned) ((e->at_ms % 1000u) / 100u));
      display.drawTextRightAlign(40, y, tmp);

      bool tx   = (e->dir != RIFT_AIR_RX);
      bool fail = (e->dir == RIFT_AIR_TXFAIL);

      // Direction as a glyph, and a failed send as a third glyph rather than as a
      // colour on the second. Colour alone disappears in sunlight, and a send that
      // never reached the air is the row you most need to find - it is a different
      // fault from one that was sent and never acknowledged, which is the distinction
      // the message log already makes between "no ack" and "failed".
      display.setColor(fail ? rift_pal.accent : (tx ? rift_pal.ok : rift_pal.mid));
      display.drawTextLeftAlign(44, y, fail ? "!" : (tx ? ">" : "<"));

      uint8_t pt = riftHeaderPayloadType(e->header);
      display.setColor(rift_pal.fg);
      display.drawTextLeftAlign(54, y, riftPayloadTypeName(pt));

      display.setColor(rift_pal.mid);
      if (e->path_len == 0xFF) {
        // no path recorded, which is not 63 hops
        snprintf(tmp, sizeof(tmp), "%s  -", riftRouteTypeName(riftHeaderRouteType(e->header)));
      } else {
        snprintf(tmp, sizeof(tmp), "%s %uh", riftRouteTypeName(riftHeaderRouteType(e->header)),
                 (unsigned) riftHopCount(e->path_len));
      }
      display.drawTextLeftAlign(110, y, tmp);

      if (tx) {
        // Air time, with its unit, so the value labels itself whatever the shared
        // header says. A failed row prints "-" rather than "0ms": the send-timeout
        // path in Dispatcher never adds to the air-time total, so zero there is the
        // absence of a measurement and not a measurement of zero.
        display.setColor(fail ? rift_pal.accent : rift_pal.mid);
        if (fail) {
          strcpy(tmp, "-");
        } else {
          snprintf(tmp, sizeof(tmp), "%ums", (unsigned) e->air_ms);
        }
        display.drawTextRightAlign(262, y, tmp);
      } else {
        // RSSI and SNR are what say whether a packet was comfortable or marginal, and
        // a marginal one that decoded is the interesting case
        display.setColor(rift_pal.fg);
        snprintf(tmp, sizeof(tmp), "%d", (int) e->rx.rssi);
        display.drawTextRightAlign(206, y, tmp);
        // Formatted from the magnitude with an explicit sign. Dividing a negative by
        // four truncates toward zero, so an SNR of -0.5 came out as "0.5" - the sign
        // vanished for exactly the marginal packets this column exists to show.
        int q = (int) e->rx.snr4;
        bool neg = q < 0;
        if (neg) q = -q;
        snprintf(tmp, sizeof(tmp), "%s%d.%d", neg ? "-" : "", q / 4, (q % 4) * 25 / 10);
        display.drawTextRightAlign(262, y, tmp);
      }

      display.setColor(rift_pal.mid);
      snprintf(tmp, sizeof(tmp), "%u", (unsigned) e->len);
      display.drawTextRightAlign(316, y, tmp);
    }

    display.setColor(rift_pal.rule);
    display.fillRect(0, 200, display.width(), 1);
    display.setColor(rift_pal.dim);
    display.drawTextLeftAlign(2, 206, "up/down  L/R page  ENTER back");

    // Remaining transmit budget, which this screen is now the right place for: the
    // air time column above says what each packet cost, and this says what is left to
    // spend. Measured on hardware, an advert is 777ms - so at the duty cycle this
    // radio enforces, one flood advert is a large fraction of the whole allowance,
    // and "why will it not send" has an answer here rather than nowhere.
    //
    // Accent below 100ms, which is MIN_TX_BUDGET_RESERVE_MS - the floor under which
    // nothing can transmit at all.
    //
    // It is a floor and not the whole rule. Dispatcher also requires at least half the
    // next packet's estimated airtime (MIN_TX_BUDGET_AIRTIME_DIV), so a 777ms advert is
    // already being deferred at around 389ms while this figure still reads as normal.
    // The threshold cannot be exact here because the size of the next packet is not
    // known yet; what the number does say truthfully is how much allowance is left,
    // and that below 100ms the answer to "why will it not send" is "it is waiting".
    unsigned long budget = the_mesh.getRemainingTxBudget();
    char bb[20];
    if (budget >= 1000) {
      snprintf(bb, sizeof(bb), "TX %lu.%lus", budget / 1000, (budget % 1000) / 100);
    } else {
      snprintf(bb, sizeof(bb), "TX %lums", budget);
    }
    display.setColor(budget < 100 ? rift_pal.accent : rift_pal.mid);
    display.drawTextRightAlign(290, 206, bb);

    display.setColor(rift_pal.mid);
    display.drawTextRightAlign(318, 206, _rx_scroll == 0 ? "live" : "held");

    renderNavBar(display, RIFT_NAV_SYSTEM);
    // Faster than the other screens because this one is meant to be watched. Still
    // coarse enough that the full redraw does not fight the radio for the SPI bus,
    // which is the thing that would make the log it is showing get shorter.
    return 400;
  }

  int renderSetTime(DisplayDriver& display) {
    renderHeading(display, "SET TIME");
    display.setTextSize(1);

    // The format line stays: it is the input specification, not a description of
    // it, and the field is unusable without knowing the shape. The notes about
    // local time and about impossible dates being refused rather than corrected
    // moved to the README - both were explanation, and this screen is meant to be
    // readings and actions.
    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, 34, "YYYY-MM-DD HH:MM   local time");

    _edit.render(display, 4, 62, display.width() - 8);

    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, 94, "ENTER set   BACKSPACE delete / back");

    renderNavBar(display, RIFT_NAV_SYSTEM);
    return 1000;
  }

  int renderEditName(DisplayDriver& display) {
    renderHeading(display, "NODE NAME");
    display.setTextSize(1);

    _edit.render(display, 4, 74, display.width() - 8);

    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, 104, "ENTER save   BACKSPACE delete / back");

    renderNavBar(display, RIFT_NAV_SYSTEM);
    return 1000;
  }

public:
  RiftSystemScreen(UITask* task) : _task(task), _mode(MENU), _sel(0), _key_choice(0) {
    _ch_name[0] = 0;
    _ch_key[0] = 0;
    // -1 so a tap arriving before the first render hits nothing, rather than
    // matching whatever the uninitialised array happened to contain
    for (int i = 0; i < IT_COUNT; i++) _item_y[i] = -1;
  }

  int render(DisplayDriver& display) override {
    switch (_mode) {
      case EDIT_NAME:      return renderEditName(display);
      case CH_NAME:        return renderChannelName(display);
      case CH_KEY_CHOICE:  return renderKeyChoice(display);
      case CH_KEY_ENTRY:   return renderKeyEntry(display);
      case CH_SHOW_KEY:    return renderShowKey(display);
      case SCOPE_PICK:     return renderScopeList(display);
      case SCOPE_ENTRY:    return renderScopeEntry(display);
      case CH_DELETE:      return renderDeleteList(display);
      case CH_DELETE_CONFIRM: return renderDeleteConfirm(display);
      case LOG:            return renderLog(display);
      case SET_TIME:       return renderSetTime(display);
      case RXLOG:          return renderRxLog(display);
      default: break;
    }

    rift_system_page = _page;
    return (_page == 0) ? renderActions(display) : renderReadings(display);
  }

  int renderActions(DisplayDriver& display) {
    display.setTextSize(1);

    char tmp[72];

    // Full width now. The actions and the readings used to share the screen as two
    // columns, which meant neither got more room as the readings grew - and the
    // readings are the half that grows. They are two pages instead.
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(2, 2, "ACTIONS");
    display.drawTextRightAlign(318, 2, "PAGE 1/2 - RIGHT: READINGS");
    display.setColor(rift_pal.rule);
    display.fillRect(0, 16, display.width(), 1);

    int y = MENU_TOP;
    for (int i = 0; i < IT_COUNT; i++) {
      itemLabel(i, tmp, sizeof(tmp));
      bool sel = (i == _sel);
      if (sel) {
        display.setColor(rift_pal.accent);
        display.fillRect(0, y - 2, display.width(), 12);
        display.setColor(rift_pal.on_accent);
      } else {
        display.setColor(rift_pal.fg);
      }
      display.drawTextLeftAlign(4, y, tmp);
      _item_y[i] = y - 2;              // the fill starts 2px above the text
      y += RIFT_LINE_H;

      // One line under the selected action, and only for the four that cost
      // something you cannot get back. These are not descriptions - they are the
      // warnings that used to be spread over five lines of prose, attached to the
      // action they belong to and shown only when it is the one selected.
      if (sel) {
        const char* note = NULL;
        switch (i) {
          case IT_CHANNEL:    note = "#hashtag channels are not secret"; break;
          case IT_DELCHANNEL: note = "the key is lost with it"; break;
          default: break;
        }
        if (note != NULL) {
          display.setColor(rift_pal.mid);
          display.drawTextLeftAlign(4, y, note);
          y += RIFT_LINE_H;
        }
      }
    }

    // The five-line note explaining neighbours-vs-whole-mesh used to sit here.
    // Removed at the user's request after seeing the screen: it was the largest
    // block of prose in a UI that is otherwise readings and actions, and the menu
    // had grown to nine items and needed the room. The explanation is not lost -
    // it moved to the README, which is where a first-time question gets asked.

    // ---- footer, left column only ----
    // This was a full-width band with a rule at y=178. The diagnostics column
    // has grown to fifteen rows, which puts its last row at y=188, so BOOT and
    // SLOWEST were drawn underneath the footer text and neither was readable.
    // The footer only ever described the left column, so it moves there, into
    // the space below the note, and the right column gets the full height.
    display.setColor(rift_pal.rule);
    display.fillRect(0, 196, display.width(), 1);
    // Back to the full name now that the row is 320px wide rather than 158: it was
    // shortened to "KEY EXPORT" only because that plus "DISABLED" came to 156px in
    // a 158px column with no gap between them.
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(2, 202, "PRIVATE KEY EXPORT");
    display.setColor(rift_pal.ok);
    display.drawTextRightAlign(318, 202, "DISABLED");
    display.setColor(rift_pal.dim);
    display.drawTextLeftAlign(2, 214, "up/down select, ENTER activates");
    display.setColor(rift_pal.mid);
    sprintf(tmp, "v%s", RIFT_VERSION);
    display.drawTextRightAlign(318, 214, tmp);

    renderNavBar(display, RIFT_NAV_SYSTEM);
    return 1000;
  }

  // One group heading: the label, and a rule under it that stops at the column edge
  // so the two columns read as two lists rather than one wrapped one.
  void group(DisplayDriver& display, const char* label, int x, int y) {
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(x, y, label);
    display.setColor(rift_pal.rule);
    display.fillRect(x, y + 10, (x < 160) ? 154 : 152, 1);
    // Back to the label colour, because the first row under a heading draws
    // straight after this and three of them ("NODE", "TROPO", "FREE HEAP") were
    // coming out in the rule grey.
    display.setColor(rift_pal.mid);
  }

  // Page 2 - readings. Grouped rather than one long column: seventeen label/value
  // rows in a single list is a wall, and the groups say which subsystem a row
  // belongs to, which is the first thing you want when a value looks wrong.
  int renderReadings(DisplayDriver& display) {
    display.setTextSize(1);
    char tmp[72];
    static const int GROUP_TOP = 20;
    int CX = 2, CR = 158;
    int y = GROUP_TOP;

    display.setColor(rift_pal.rule);
    display.fillRect(160, 2, 1, 224);

    group(display, "DEVICE", CX, 4);

    
    // 14px further up than before, which this column can use: it grows downward
    // as boot timings are appended and was the closest to running out of room.
    y = GROUP_TOP;
    display.drawTextLeftAlign(CX, y, "NODE");
    display.setColor(rift_pal.fg);
    // Truncated, then right-aligned like every other value on this page. It is the
    // only row here taking free text - node_name is 32 bytes and the column is 156px
    // - so a long name ran through its own label and off the left edge. Cut rather
    // than left-aligned, because a value that changes alignment when it grows reads
    // as a different kind of row.
    {
      char nm[20];
      StrHelper::strncpy(nm, the_mesh.getNodeName(), sizeof(nm));
      display.drawTextRightAlign(CR, y, nm);
    }
    y += RIFT_LINE_H;

#ifdef RIFT_SPEAKER
    // A verdict, not a number to interpret. It counts the times the DMA engine had
    // played everything it was given while a tone was still running, which is the
    // stutter itself rather than a proxy for it.
    //
    // The first version of this row showed the gap between speaker passes, and it
    // read "0ms max" - which it would have read however bad the audio was, because
    // a queue big enough to take a whole tone in one pass leaves no second pass to
    // measure against. A measurement that cannot fail is not a measurement.
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "AUDIO");
    {
      // Worst passes, not the underrun count. A tone written in one pass is one
      // the DMA engine took whole; anything above that means writes were being
      // refused, which is what every bad-sounding alert has had in common.
      uint32_t mp = rift_speaker.maxPasses();
      uint32_t un = rift_speaker.underruns();
      display.setColor((mp > 2 || un > 0) ? rift_pal.accent : rift_pal.ok);
      if (un > 0)      snprintf(tmp, sizeof(tmp), "%u underruns", (unsigned) un);
      else if (mp > 2) snprintf(tmp, sizeof(tmp), "%u passes worst", (unsigned) mp);
      else             snprintf(tmp, sizeof(tmp), "ok, %u pass", (unsigned) mp);
      display.drawTextRightAlign(CR, y, tmp);
    }
    y += RIFT_LINE_H;

    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "AUDIO FRAMES");
    display.setColor(rift_speaker.isPresent() ? rift_pal.fg : rift_pal.accent);
    if (rift_speaker.isPresent()) {
      snprintf(tmp, sizeof(tmp), "%u", (unsigned) rift_speaker.framesWritten());
    } else {
      snprintf(tmp, sizeof(tmp), "driver not started");
    }
    display.drawTextRightAlign(CR, y, tmp);
    y += RIFT_LINE_H;
#endif

#ifdef RIFT_INPUT_KEYBOARD
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "KEYBOARD");
    // status is the only thing colour carries here, and only pass or fail
    // "lost" and "not found" are different faults: one keyboard was never there, the
    // other stopped answering and is being re-probed every few seconds.
    display.setColor(rift_keyboard.isPresent() ? rift_pal.ok : rift_pal.accent);
    display.drawTextRightAlign(CR, y,
                               rift_keyboard.isPresent() ? "ok"
                               : (rift_keyboard.wasLost() ? "lost, retrying" : "not found"));
    y += RIFT_LINE_H;

    // Two values, because they can differ and the difference is the interesting
    // part. lastKeyCode() is what the UI received, i.e. after TDeckKeyboard::poll()
    // discards everything above 127; lastSeen() is the raw byte the co-processor
    // sent, recorded before that filter. A key that reports "0 / 180" is being
    // thrown away rather than not existing - which is the case for the arrow keys,
    // and is how to find out whether SYM emits anything at all.
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "LAST KEY");
    display.setColor(rift_pal.fg);
    sprintf(tmp, "%d / %d", _task->lastKeyCode(), (int) rift_keyboard.lastSeen());
    display.drawTextRightAlign(CR, y, tmp);
    y += RIFT_LINE_H;

    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "I2C BUS");
    display.setColor(rift_pal.fg);
    tmp[0] = 0;
    for (int i = 0; i < rift_keyboard.seenCount() && i < 4; i++) {
      char hex[6];
      sprintf(hex, "%s%02X", i ? " " : "", rift_keyboard.seenAddr(i));
      strcat(tmp, hex);
    }
    if (rift_keyboard.seenCount() == 0) strcpy(tmp, "empty");
    display.drawTextRightAlign(CR, y, tmp);
    y += RIFT_LINE_H;
#endif

#ifdef RIFT_INPUT_TOUCH
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "TOUCH");
    if (rift_touch.isPresent()) {
      display.setColor(rift_pal.fg);
      // mapped, then the raw pair it came from. The raw pair is how the
      // calibration flags in TDeckTouch.h are set: touch each corner and read
      // it here. A corner that maps to 0/319 and 0/239 is calibrated.
      snprintf(tmp, sizeof(tmp), "%d,%d (%d,%d)", _task->lastTouchX(), _task->lastTouchY(),
               rift_touch.rawX(), rift_touch.rawY());
    } else {
      display.setColor(rift_pal.accent);
      strcpy(tmp, "not found");
    }
    display.drawTextRightAlign(CR, y, tmp);
    y += RIFT_LINE_H;
#endif

#if ENV_INCLUDE_GPS == 1
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "GPS");
    // accent because GPS varies between units and people ask about it, not
    // because anything is wrong
    if (_task->hasGPSHardware()) {
      LocationProvider* nmea = sensors.getLocationProvider();
      display.setColor(rift_pal.fg);
      if (nmea != NULL && nmea->isValid()) {
        snprintf(tmp, sizeof(tmp), "%s, %ld sat", _task->getGPSState() ? "on" : "off",
                 (long) nmea->satellitesCount());
      } else {
        sprintf(tmp, "%s, no fix", _task->getGPSState() ? "on" : "off");
      }
    } else {
      display.setColor(rift_pal.accent);
      strcpy(tmp, "not found");
    }
    display.drawTextRightAlign(CR, y, tmp);
    y += RIFT_LINE_H;
#endif

    // Added after an unset clock silently emptied NODES, RADAR and the HOPS row
    // below: every advert was being cached with recv_timestamp 0, which the readers
    // took for an empty slot. Nothing on screen said the clock was unset, and it is
    // the normal state of a standalone node - no companion app to set it, and no GPS
    // fix. It also explains message timestamps reading 00:00.
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "CLOCK");
    {
      uint32_t now = the_mesh.getRTCClock()->getCurrentTime();
      // 1600000000 is September 2020. Anything below it is not a clock that was
      // ever set, whatever it counted up to since boot.
      if (now < 1600000000u) {
        display.setColor(rift_pal.accent);
        strcpy(tmp, "unset");
      } else {
        display.setColor(rift_pal.fg);
        snprintf(tmp, sizeof(tmp), "%02u:%02u", (unsigned) ((now / 3600u) % 24u),
                 (unsigned) ((now / 60u) % 60u));
      }
      display.drawTextRightAlign(CR, y, tmp);
      y += RIFT_LINE_H;
    }

    // Contacts, with the capacity and whether anything has been turned away.
    //
    // Being full is not a state the table reports - allocateContactSlot() returns NULL
    // and MeshCore told a companion app, which a standalone device does not have. So
    // this row and the accent on it are the only way the device can say it. FULL rather
    // than a number, because the count already says the number and what matters is that
    // adverts are being refused.
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "CONTACTS");
    {
      int used = the_mesh.getNumContacts(), cap = the_mesh.getContactsCapacity();
      bool full = the_mesh.contactsFullNow();
      display.setColor(full ? rift_pal.accent : rift_pal.fg);
      if (full) {
        snprintf(tmp, sizeof(tmp), "%d/%d FULL", used, cap);
      } else if (the_mesh.contactsEverRefused()) {
        // room now, but nodes were turned away earlier - which is why the mesh may
        // look smaller than it is, and is not the same claim as FULL
        snprintf(tmp, sizeof(tmp), "%d/%d was full", used, cap);
      } else {
        snprintf(tmp, sizeof(tmp), "%d/%d", used, cap);
      }
      display.drawTextRightAlign(CR, y, tmp);
      y += RIFT_LINE_H;
    }

    // Occupancy and pressure on the path cache. The size is a guess until it is
    // measured against a real mesh, and an eviction count is what says whether the
    // number is too small - a cache at 16/16 with evictions climbing is a mesh this
    // screen cannot show all of, which is a different problem from a layout that
    // cannot fit it.
    display.setColor(rift_pal.mid);
    group(display, "MESH", CX, y + 4);
    y += RIFT_LINE_H + 6;

    // Both halves of the tropo signal, so it can be judged rather than trusted.
    // deep is what the detector counted in the current window; peak is how far
    // the deepest packet had travelled. A peak that never rises above the local
    // mesh depth means the threshold is set for a mesh this node is not in.
    //
    // In the MESH column rather than beside the hardware readings: it is a
    // statement about the network, and the left column is one row from its
    // footer.
    display.drawTextLeftAlign(CX, y, "TROPO");
    {
      RiftTropo& tr = riftTropoState();
      display.setColor(tr.active ? rift_pal.accent : rift_pal.fg);
      // base is the deepest ordinary path in this window, which is what makes the
      // peak mean anything: "peak 21, base 6" is an opening, "peak 21, base 19"
      // is a mesh where 20 was simply the wrong threshold.
      snprintf(tmp, sizeof(tmp), "%s %ud p%u b%u/%u",
               tr.active ? "OPEN" : "ok", (unsigned) tr.deep_count,
               (unsigned) tr.peak_hops, (unsigned) tr.base_hops,
               (unsigned) tr.seen_count);
      display.drawTextRightAlign(CR, y, tmp);
    }
    y += RIFT_LINE_H;
    display.setColor(rift_pal.mid);

    display.drawTextLeftAlign(CX, y, "PATH CACHE");
    {
      int used = the_mesh.getPathCacheUsed(), size = the_mesh.getPathCacheSize();
      unsigned evicted = the_mesh.getPathEvictions();
      display.setColor(evicted > 0 ? rift_pal.accent : rift_pal.fg);
      // "evict", not "lost". An eviction is an event, not a node: the same node can
      // be evicted, heard again, and evicted again, so the count exceeds the number
      // of nodes no longer held. Saying "lost" claimed the second thing while
      // measuring the first, and only separate identity tracking could say it.
      if (evicted > 0) snprintf(tmp, sizeof(tmp), "%d/%d, %u evict", used, size, evicted);
      else             snprintf(tmp, sizeof(tmp), "%d/%d", used, size);
      display.drawTextRightAlign(CR, y, tmp);
      y += RIFT_LINE_H;
    }

    // How long the last message-log write blocked for, and how many messages it
    // held. There is no watchdog on the main loop, so a blocking call starves the
    // radio silently - this is the number that says whether the debounce is
    // enough or whether the write has to be broken up.
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "MSGLOG");
    display.setColor(rift_pal.fg);
    sprintf(tmp, "%d msg %ums", msg_log.count, (unsigned) msg_log.last_save_ms);
    display.drawTextRightAlign(CR, y, tmp);
    y += RIFT_LINE_H;

    // open / write / close, so the 553ms can be attributed rather than guessed
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "  phases");
    display.setColor(rift_pal.fg);
    sprintf(tmp, "%u %u %u", (unsigned) msg_log.t_open, (unsigned) msg_log.t_write,
                             (unsigned) msg_log.t_close);
    display.drawTextRightAlign(CR, y, tmp);
    y += RIFT_LINE_H;

    display.setColor(rift_pal.mid);
    // second column. The right one is where new diagnostics land, because it is
    // the one with room - the left two groups are a fixed set.
    CX = 166; CR = 318;
    group(display, "RUNTIME", CX, 4);
    y = GROUP_TOP;

    display.drawTextLeftAlign(CX, y, "FREE HEAP");
    display.setColor(rift_pal.fg);
    sprintf(tmp, "%uK", (unsigned) (ESP.getFreeHeap() / 1024));
    display.drawTextRightAlign(CR, y, tmp);
    y += RIFT_LINE_H;

    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "EXT POWER");
    display.setColor(rift_pal.fg);
    sprintf(tmp, "%s %umV", board.isExternalPowered() ? "yes" : "no",
            (unsigned) _task->getBattMilliVolts());
    display.drawTextRightAlign(CR, y, tmp);
    y += RIFT_LINE_H;

    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "MSG WAKE");
    display.setColor(rift_pal.fg);
    if (rift_msg_wakes == 0) {
      strcpy(tmp, "none yet");
    } else {
      uint32_t age = (uint32_t) millis() - rift_last_wake_ms;
      if (age < 60000u)        sprintf(tmp, "%u, %us ago", rift_msg_wakes, (unsigned) (age / 1000u));
      else if (age < 3600000u) sprintf(tmp, "%u, %um ago", rift_msg_wakes, (unsigned) (age / 60000u));
      else                     sprintf(tmp, "%u, %uh ago", rift_msg_wakes, (unsigned) (age / 3600000u));
    }
    display.drawTextRightAlign(CR, y, tmp);
    y += RIFT_LINE_H;

    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "LAST RESET");
    esp_reset_reason_t reason = esp_reset_reason();
    const char* rr;
    switch (reason) {
      case ESP_RST_POWERON:   rr = "power on"; break;
      case ESP_RST_SW:        rr = "sw restart"; break;
      case ESP_RST_PANIC:     rr = "PANIC"; break;
      case ESP_RST_INT_WDT:   rr = "int wdt"; break;
      case ESP_RST_TASK_WDT:  rr = "task wdt"; break;
      case ESP_RST_WDT:       rr = "wdt"; break;
      case ESP_RST_DEEPSLEEP: rr = "deep sleep"; break;
      case ESP_RST_EXT:       rr = "ext reset"; break;
      // The colour test below already named it; the switch did not, so the one
      // reset a battery device most needs to name read "unknown".
      case ESP_RST_BROWNOUT:  rr = "BROWNOUT"; break;
      default:                rr = "unknown"; break;
    }
    display.setColor((reason == ESP_RST_PANIC || reason == ESP_RST_BROWNOUT)
                     ? rift_pal.accent : rift_pal.fg);
    display.drawTextRightAlign(CR, y, rr);
    y += RIFT_LINE_H;

    // Two rows that would have named the 0.9.1 fault on sight. The I2S master
    // clock was routed to GPIO0, so the trackball button read as held from the
    // first loop and every boot entered CLI rescue - a mode in which everything
    // on screen works and the USB companion protocol does not. Neither fact was
    // visible anywhere. The raw pin level is shown rather than the debounced
    // state, because the raw level is the thing that was wrong.
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "USB SERIAL");
    if (the_mesh.isCLIRescue()) {
      display.setColor(rift_pal.accent);
      display.drawTextRightAlign(CR, y, "RESCUE CLI");
    } else {
      display.setColor(rift_pal.fg);
      display.drawTextRightAlign(CR, y, "companion");
    }
    y += RIFT_LINE_H;

#ifdef PIN_USER_BTN
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "BOOT BTN");
    {
      int level = digitalRead(PIN_USER_BTN);
      // active low: a held (or driven) pin reads 0
      display.setColor(level == LOW ? rift_pal.accent : rift_pal.fg);
      sprintf(tmp, "%s gpio%d=%d", level == LOW ? "DOWN" : "up", (int) PIN_USER_BTN, level);
      display.drawTextRightAlign(CR, y, tmp);
    }
    y += RIFT_LINE_H;
#endif

    if (rift_boot_mark_count > 0) {
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(CX, y, "BOOT");
      display.setColor(rift_pal.fg);
      sprintf(tmp, "%ums", (unsigned) rift_boot_marks[rift_boot_mark_count - 1].at_ms);
      display.drawTextRightAlign(CR, y, tmp);
      y += RIFT_LINE_H;

      int worst = 0;
      uint32_t worst_ms = 0;
      for (int i = 0; i < rift_boot_mark_count; i++) {
        uint32_t prev = (i == 0) ? 0 : rift_boot_marks[i - 1].at_ms;
        uint32_t gap = rift_boot_marks[i].at_ms - prev;
        if (gap > worst_ms) { worst = i; worst_ms = gap; }
      }
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(CX, y, "SLOWEST");
      display.setColor(rift_pal.fg);
      sprintf(tmp, "%s %u", rift_boot_marks[worst].name, (unsigned) worst_ms);
      display.drawTextRightAlign(CR, y, tmp);
      y += RIFT_LINE_H;
    }

    // ---- event log, the one group that is not label/value
    group(display, "EVENT LOG", CX, y + 4);
    y += RIFT_LINE_H + 6;
    {
      display.setColor(rift_pal.fg);
      snprintf(tmp, sizeof(tmp), "%d lines", riftLog().count);
      display.drawTextLeftAlign(CX, y, tmp);
      if (riftLog().dropped > 0) {
        display.setColor(rift_pal.mid);
        snprintf(tmp, sizeof(tmp), "+%u lost", (unsigned) riftLog().dropped);
        display.drawTextRightAlign(CR, y, tmp);
      }
      y += RIFT_LINE_H;

      // The two newest lines, so the log answers its own question from here in the
      // common case and only has to be opened when it does not.
      display.setColor(rift_pal.dim);
      for (int k = 1; k >= 0; k--) {
        const RiftEventLog::Line* l = riftLog().peek(k);
        if (l == NULL) continue;
        char shown[RIFT_LOG_TEXT * 2];
        riftTranslateUTF8(shown, l->text, sizeof(shown));
        display.drawTextEllipsized(CX, y, CR - CX, shown);
        y += RIFT_LINE_H;
      }

      display.setColor(rift_pal.mid);   // a key hint, not a warning
      display.drawTextLeftAlign(CX, y, "ENTER: open log");
    }

    display.setColor(rift_pal.dim);
    display.drawTextLeftAlign(2, 214, "left: actions");
    display.setColor(rift_pal.mid);
    snprintf(tmp, sizeof(tmp), "PAGE 2/2 " "%s" " v%s", RIFT_DOT, RIFT_VERSION);
    display.drawTextRightAlign(318, 214, tmp);

    renderNavBar(display, RIFT_NAV_SYSTEM);
    return 1000;
  }

  // Dragging, which this screen did not have while NODES, RADAR and COMMS all got
  // it. That is the recurring shape of a mistake in this codebase - a fix applied
  // to one screen and not its siblings - and it landed on the longest lists in the
  // firmware: 128 event-log lines and 96 packet rows, reachable only by trackball.
  //
  // Each mode moves the thing it already moves with up and down, so nothing here
  // invents a second idea of scrolling.
  bool handleDrag(int dy) override {
    int steps = riftDragSteps(&_drag_residual, dy, RIFT_DRAG_PITCH);
    if (steps == 0) return false;

    // The log views scroll a window; dragging down goes back through history,
    // which is the same direction the arrow keys take. Clamped by the renderers
    // themselves, as they already do for the trackball.
    if (_mode == LOG)   { _log_scroll -= steps; if (_log_scroll < 0) _log_scroll = 0; return true; }
    if (_mode == RXLOG) { _rx_scroll  -= steps; if (_rx_scroll < 0) _rx_scroll = 0; return true; }

    // The pickers move a cursor.
    if (_mode == CH_DELETE || _mode == SCOPE_PICK) {
      if (_del_count == 0) return false;
      int n = _del_sel + steps;
      if (n < 0) n = 0;
      if (n >= _del_count) n = _del_count - 1;
      if (n == _del_sel) return false;
      _del_sel = n;
      return true;
    }

    if (_mode == MENU && _page == 0) {
      int n = _sel + steps;
      if (n < 0) n = 0;
      if (n >= IT_COUNT) n = IT_COUNT - 1;
      if (n == _sel) return false;
      _sel = n;
      return true;
    }
    return false;
  }

  bool handleTouch(int x, int y) override {
    (void) x;
    if (_mode != MENU) return false;
    // Page 2 has no actions. Without this a tap on the readings page was mapped to
    // an action row and activated - and depending on where the finger landed that
    // was Delete channel or an advert to the whole mesh, from a screen that gives
    // no sign of being interactive at all.
    if (_page != 0) return false;

    // Hit-tested against where the rows were actually drawn. The full width now,
    // because the rows are 320px wide: the old x > 158 bound was left over from the
    // two-column layout and made the right half of every row - the half showing the
    // setting's current value - dead to touch.
    for (int i = 0; i < IT_COUNT; i++) {
      if (_item_y[i] < 0) continue;
      if (y >= _item_y[i] && y < _item_y[i] + 12) {
        _sel = i;
        activate();
        return true;
      }
    }
    return false;
  }

  bool handleInput(char c) override {
    if (_mode == EDIT_NAME) {
      if (c == KEY_ENTER) { commitName(); return true; }
      if (_edit.handleKey(c)) return true;       // consumed as text
      if (c == RIFT_KEY_BACK || c == KEY_CANCEL) { _mode = MENU; return true; }
      return true;   // don't let stray keys navigate away mid-edit
    }

    if (_mode == CH_NAME) {
      if (c == KEY_ENTER) {
        if (_edit.len == 0) { _task->showAlert("Name can't be empty", 1200); return true; }
        StrHelper::strncpy(_ch_name, _edit.buf, sizeof(_ch_name));
        _key_choice = 0;
        _mode = CH_KEY_CHOICE;
        return true;
      }
      if (_edit.handleKey(c)) return true;
      if (c == RIFT_KEY_BACK || c == KEY_CANCEL) { _mode = MENU; return true; }
      return true;
    }

    if (_mode == CH_KEY_CHOICE) {
      if (c == KEY_UP) { _key_choice = (_key_choice + 2) % 3; return true; }
      if (c == KEY_DOWN) { _key_choice = (_key_choice + 1) % 3; return true; }
      if (c == KEY_ENTER) {
        if (_key_choice == 2) {
          _edit.begin("", 44);   // 44 base64 chars == 256-bit key
          _mode = CH_KEY_ENTRY;
        } else {
          finishChannel(_key_choice);
        }
        return true;
      }
      if (c == RIFT_KEY_BACK || c == KEY_CANCEL) {
        // Back to the name with the name in it. Choosing "paste a key" re-seeds
        // the editor for the key, so returning without this showed an empty
        // field and "Name can't be empty" for a name that had been typed.
        _edit.begin(_ch_name, sizeof(_ch_name) - 2);
        _mode = CH_NAME;
        return true;
      }
      return true;
    }

    if (_mode == CH_KEY_ENTRY) {
      if (c == KEY_ENTER) { finishChannel(2); return true; }
      if (_edit.handleKey(c)) return true;
      if (c == RIFT_KEY_BACK || c == KEY_CANCEL) { _mode = CH_KEY_CHOICE; return true; }
      return true;
    }

    if (_mode == SCOPE_PICK) {
      if (c == RIFT_KEY_BACK || c == KEY_CANCEL) { _mode = MENU; return true; }
      if (_del_count == 0) return true;
      if (c == KEY_UP)   { _del_sel = (_del_sel + _del_count - 1) % _del_count; return true; }
      if (c == KEY_DOWN) { _del_sel = (_del_sel + 1) % _del_count; return true; }
      if (c == KEY_ENTER) {
        const char* cur = riftScopes().nameFor(_del_idx[_del_sel],
                                              riftChannelConv(_del_idx[_del_sel]).channel_fp);
        _edit.begin(cur ? cur : "", RIFT_SCOPE_NAME_MAX - 1);
        _mode = SCOPE_ENTRY;
        return true;
      }
      return true;
    }

    if (_mode == SCOPE_ENTRY) {
      if (c == KEY_ENTER) {
        uint8_t idx = _del_idx[_del_sel];
        uint32_t fp = riftChannelConv(idx).channel_fp;
        if (_edit.len == 0) {
          // Empty is how a channel goes back to the node default, which is why
          // the entry screen says so rather than refusing an empty field.
          riftScopes().clear(idx);
          riftLogf("scope cleared on channel %d", (int) idx);
          _task->showAlert("Scope cleared", 1200);
        } else if (!riftScopeNameValid(_edit.buf)) {
          // Says which rule was broken, because "invalid" for a name that looks
          // fine is the least helpful thing a field can say. A space is the one
          // people will try: upstream's RegionMap does not accept it.
          _task->showAlert(strchr(_edit.buf, ' ') != NULL
                             ? "No spaces in a region name"
                             : (_edit.buf[0] == '$' ? "Private regions not supported"
                                                    : "Not a valid region name"), 1900);
          return true;
        } else if (!riftScopes().set(idx, fp, _edit.buf)) {
          _task->showAlert(fp == 0 ? "Channel has no key yet" : "No scope slots left", 1600);
          return true;
        } else {
          // Logged canonical, which is what will be hashed - logging what was
          // typed would hide the one transformation that matters.
          const char* saved = riftScopes().nameFor(idx, fp);
          riftLogf("scope on channel %d: %s", (int) idx, saved ? saved : "?");
          _task->showAlert("Scope saved", 1200);
        }
        riftSaveSettings();
        _mode = SCOPE_PICK;
        return true;
      }
      if (_edit.handleKey(c)) return true;
      if (c == RIFT_KEY_BACK || c == KEY_CANCEL) { _mode = SCOPE_PICK; return true; }
      return true;   // no stray key navigates away mid-edit
    }

    if (_mode == CH_DELETE) {
      if (c == RIFT_KEY_BACK || c == KEY_CANCEL) { _mode = MENU; return true; }
      if (_del_count == 0) return true;
      if (c == KEY_UP)   { _del_sel = (_del_sel + _del_count - 1) % _del_count; return true; }
      if (c == KEY_DOWN) { _del_sel = (_del_sel + 1) % _del_count; return true; }
      // deliberately a second screen rather than an immediate delete: the key is
      // not recoverable from here and the list is one keypress from the menu
      if (c == KEY_ENTER) { _mode = CH_DELETE_CONFIRM; return true; }
      return true;
    }

    if (_mode == SET_TIME) {
      if (c == KEY_ENTER) {
        uint32_t epoch = 0;
        if (!riftParseCivil(_edit.buf, &epoch)) {
          _task->showAlert("Need YYYY-MM-DD HH:MM", 2000);
          return true;
        }
        // Set directly, including backwards. The companion protocol refuses a time
        // earlier than the current one, which is right for an automatic sync and
        // wrong here: correcting a clock that is running ahead is the main reason
        // to type one in by hand.
        the_mesh.getRTCClock()->setCurrentTime(epoch);
        riftLogf("clock set to %s", _edit.buf);
        _task->showAlert("Clock set", 1500);
        _mode = MENU;
        return true;
      }
      if (_edit.handleKey(c)) return true;
      // Only an explicit back leaves; the other editors swallow stray keys too. A
      // trackball nudge mid-entry used to discard the half-typed time.
      if (c == RIFT_KEY_BACK || c == KEY_CANCEL) _mode = MENU;
      return true;
    }

    if (_mode == RXLOG) {
      // Scrolling away from the newest freezes the view - the footer says "held" -
      // because a list that reflows under the eye while packets arrive cannot be read.
      if (c == KEY_UP)    { _rx_scroll++; return true; }
      if (c == KEY_DOWN)  { if (_rx_scroll > 0) _rx_scroll--; return true; }
      if (c == KEY_LEFT)  { _rx_scroll += 13; return true; }
      if (c == KEY_RIGHT) { _rx_scroll = _rx_scroll > 13 ? _rx_scroll - 13 : 0; return true; }
      _mode = MENU;
      return true;
    }

    if (_mode == LOG) {
      // Read-only, so every key that is not a scroll means "done". Rows are added
      // and removed at the newest end, so scrolling up means going further back.
      // Not wrapped like the menu lists: those are selections where wrapping is a
      // shortcut, this is a position in a history where wrapping from the oldest
      // line to the newest would read as the log having jumped.
      if (c == KEY_UP)   { _log_scroll++; return true; }
      if (c == KEY_DOWN) { if (_log_scroll > 0) _log_scroll--; return true; }
      // left/right page, because the ring is 128 lines and 14 fit on screen - by
      // line alone the far end is nine presses away. render() clamps the offset,
      // so overshooting the oldest line is harmless.
      if (c == KEY_LEFT)  { _log_scroll += 14; return true; }
      if (c == KEY_RIGHT) { _log_scroll = _log_scroll > 14 ? _log_scroll - 14 : 0; return true; }
      _mode = MENU;
      return true;
    }

    if (_mode == CH_DELETE_CONFIRM) {
      if (c == KEY_ENTER) {
        // The conversation key is taken BEFORE the channel goes, because
        // riftChannelConv() reads the channel table and a blanked slot answers
        // unknown - which matches nothing, so the purge would silently remove
        // nothing at all.
        RiftConvKey gone = (_del_sel < _del_count) ? riftChannelConv(_del_idx[_del_sel])
                                                   : riftConvUnknown();
        bool ok = (_del_sel < _del_count) && the_mesh.removeChannel(_del_idx[_del_sel]);
        // The scope goes with the channel. It did not: the entry stayed in RAM and
        // in /rift.cfg keyed on the slot, and with eight of them a fresh channel
        // was told "No scope slots left" while no scope was live. The fingerprint
        // check kept routing safe; the slot count was what leaked.
        if (ok && riftScopes().clear(_del_idx[_del_sel])) {
          riftSaveSettings();
          riftLogf("scope dropped with channel slot %d", (int) _del_idx[_del_sel]);
        }
        if (ok && gone.kind == RIFT_CONV_CHANNEL) {
          // Also takes any fingerprint-less entries in that slot, restored from a log
          // written before channel identity existed: they cannot prove they belong to
          // anything, they are in the slot being emptied, and leaving them would let
          // them attach to whatever is created there next.
          int purged = msg_log.purgeConversation(gone);
          msg_unread.clear(gone);
          if (purged > 0) riftLogf("purged %d msg from deleted channel", purged);
        }
        riftLogf("channel slot %d deleted: %s",
                 _del_sel < _del_count ? _del_idx[_del_sel] : -1, ok ? "ok" : "FAILED");
        _task->showAlert(ok ? "Channel deleted" : "Delete failed", 1400);
        collectDeletable();
        _mode = _del_count > 0 ? CH_DELETE : MENU;
        return true;
      }
      _mode = CH_DELETE;   // anything else backs out without deleting
      return true;
    }

    if (_mode == CH_SHOW_KEY) {
      if (c == KEY_ENTER || c == RIFT_KEY_BACK || c == KEY_CANCEL) {
        memset(_ch_key, 0, sizeof(_ch_key));   // acknowledged: gone now, not on leave
        _mode = MENU;
        return true;
      }
      return true;   // keep the key on screen until acknowledged
    }

    // The pages sit inside SYSTEM, and the edges still navigate out of it - so
    // right from page 1 is the readings, right from page 2 leaves, and the trackball
    // never becomes the only way out of a screen.
    if (c == KEY_NEXT || c == KEY_RIGHT) {
      if (_mode == MENU && _page == 0) { _page = 1; return true; }
      _task->cycleNavScreen(1);
      return true;
    }
    if (c == KEY_PREV || c == KEY_LEFT) {
      if (_mode == MENU && _page == 1) { _page = 0; return true; }
      _task->cycleNavScreen(-1);
      return true;
    }
    // Page 2 has nothing to select, so up and down are consumed rather than moving
    // a cursor the user cannot see, and ENTER opens the log the page is summarising.
    if (_page == 1) {
      if (c == KEY_ENTER) { _log_scroll = 0; _mode = LOG; return true; }
      if (c == KEY_UP || c == KEY_DOWN) return true;
      return false;
    }
    if (c == KEY_UP) { _sel = (_sel + IT_COUNT - 1) % IT_COUNT; return true; }
    if (c == KEY_DOWN) { _sel = (_sel + 1) % IT_COUNT; return true; }
    if (c == KEY_ENTER) { activate(); return true; }
    return false;
  }
};


// CONSTELLATION: the mesh as observed topology rather than a node count.
//
// Built from MyMesh's advert-path cache, which records the actual route each
// advert travelled: path_len is the hop count and path[] holds the hop hashes.
// So node distance from centre is real hop distance, and nodes that arrived via
// the same first hop share a direction - the branching in the original concept
// sketch is genuine routing structure, not decoration.
//
// Note there is no per-node RSSI here: adverts are cached without signal
// strength, so brightness encodes recency (how long since we last heard the
// node) rather than link quality.
#define RIFT_NODE_NAME_MAX 10

// NODES (1c) - buckets, a scrollable list, and the route expanding the selected row.
//
// Replaces the constellation, which tried to place every node in a fixed hop column
// three rows deep. Real meshes are not evenly distributed: almost everything lands
// beyond the third column, so the layout spent three quarters of its width on the
// minority and hid the majority. This answers the same four questions in order -
// how big is the mesh, how spread out, who is active, and how did the selected one
// reach me - without pretending a 320x240 panel can draw a graph faithfully.
//
// Bucket ranges are fixed. A column that means something different from one look to
// the next means nothing.
class RiftConstellationScreen : public RiftScreen {
  UITask* _task;
  AdvertPath _paths[RIFT_CONST_MAX];
  int _count;
  int _sel;
  int _scroll;

  // The selection is an identity. _sel is where it was last drawn; the key is what it
  // means. getRecentlyHeard returns the list ordered by recency, so a single advert
  // arriving reorders it - and a cursor held only as an index then points at a
  // different node while the user believes it has not moved. ENTER would message the
  // wrong one. The identical fix went into RADAR first and this screen did not get it.
  uint8_t _sel_key[7];
  bool _have_sel;
  int _drag_residual = 0;   // finger travel not yet worth a row
  // Where each row was actually drawn, so touch hits what the eye sees rather than
  // what the layout intended - the selected row is 36px tall, so a fixed pitch
  // would put every tap below it on the wrong node.
  int _row_y[RIFT_CONST_MAX];
  unsigned long _last_refresh;
  bool _refreshed_once;

  static const int BUCKET_X[RIFT_HOPB_COUNT];
  static const char* BUCKET_LABEL[RIFT_HOPB_COUNT];
  // 58, not the 64 four columns could afford: five on a 63px pitch leaves 5px of
  // gap, and two bars that touch read as one bar.
  static const int BUCKET_BAR_W = 58;

  // ---- the reach scale
  //
  // The row spent 23 character cells - x 134 to 271, on every row, in the middle
  // where the eye already is - on nothing, and answered "how far away" with a
  // single right-aligned digit in the last 46 pixels. On a screen whose one
  // question is distance. The list was never the wrong form; it was a form that
  // had been given no room to answer, next to a large amount of room nobody had
  // claimed.
  //
  // Ten cells, filled up to the node's hop count and hollow beyond, so distance
  // is a length you compare down the column without reading a digit. This is
  // RADAR's filled-versus-hollow cell, which is the encoding already proven on
  // this panel - used here as a scale per row rather than as a heap, because these
  // items have names that have to stay readable and a per-row scale leaves the
  // name column untouched.
  //
  // Absolute, matching the rule the bucket band above already follows: cell 1 is
  // always direct, so a mark means the same thing between refreshes. Filled count
  // is hops + 1, which is what makes a known route always draw at least one cell -
  // a zero-length bar for a node heard directly would have looked exactly like no
  // bar at all, which is the state reserved for a route nobody knows. Ten cells
  // saturate at nine hops or more and the exact count stays in the HOPS column,
  // because past nine the digit is the only thing that can still say the
  // difference and the length has stopped being comparable anyway.
  //
  // An unknown route draws all ten hollow and keeps its `?`. Nine hollow cells
  // cannot be misread as near, which a short bar could be.
  static const int REACH_X = 140;      // first cell
  static const int REACH_PITCH = 12;   // 8 of cell, 4 of gap - a count, not a bar
  static const int REACH_CELLS = 10;
  static const int REACH_W = 8;        // the shared 8x8 cell of the September design system
  static const int REACH_H = 8;

  // hops < 0 means no route known: every cell hollow.
  void renderReach(DisplayDriver& display, int y, int hops, uint16_t ink) {
    for (int c = 0; c < REACH_CELLS; c++) {
      int cx = REACH_X + c * REACH_PITCH;
      display.setColor(ink);
      if (hops >= 0 && c <= hops) display.fillRect(cx, y, REACH_W, REACH_H);
      else                        display.drawRect(cx, y, REACH_W, REACH_H);
    }
  }

  // Which bucket a hop count belongs in. Five, including the one for a node whose
  // route is not known.
  //
  // That fifth column is new. The band used to have four and put an unknown route
  // in none of them, on the reasoning that the four counts summing to less than the
  // heard total was itself the signal. It is not a signal anybody reads: the list
  // below draws `?` for those rows, correctly, and the band above it was the one
  // place on the screen that made a node with no route look like a node that was
  // not there. Rule 5 says an unknown has to look like what it is, and the band was
  // the exception nobody had noticed.
  static int bucketOf(uint8_t path_len) {
    if (path_len == RIFT_PATH_UNKNOWN) return RIFT_HOPB_UNKNOWN;
    return riftHopBucket((int) riftHopCount(path_len));
  }

  // Relative age, the same words the conversation list and the RIFT tab use.
  // Monotonic, so it is right whether or not the clock has ever been set. This
  // used to be a second age formatter with its own thresholds and its own
  // vocabulary ("now", "9d+"); one clock, one vocabulary.
  static void ageText(uint32_t recv_millis, char* out, size_t out_size) {
    riftFormatAge((uint32_t) millis() - recv_millis, out, out_size);
  }

  // Now a thin call into the mesh layer, which is the only place with both the recent
  // advert cache and the stored contact table. This used to build its candidate list
  // from _paths alone - the nodes this screen happens to be showing - so a hash that
  // collided with a stored contact that had not been heard recently resolved as unique
  // and named the wrong node with full confidence. The three-state result was already
  // careful; the candidate set was not.
  int resolveHash(const uint8_t* hash, uint8_t len, char* name, size_t name_len) {
    if (len > sizeof(AdvertPath::pubkey_prefix)) len = sizeof(AdvertPath::pubkey_prefix);
    return the_mesh.resolvePathHash(hash, len, name, name_len);
  }

  void refresh() {
    int n = the_mesh.getRecentlyHeard(_paths, RIFT_CONST_MAX);
    // Occupancy is the valid flag, not a timestamp or a name: recv_timestamp is
    // legitimately zero on a node whose RTC was never set, and inferring emptiness
    // from it hid every node this device had heard.
    int live = 0;
    for (int i = 0; i < n; i++) {
      if (!_paths[i].valid) continue;
      if (live != i) _paths[live] = _paths[i];
      live++;
    }
    _count = live;

    // Re-find the selection by key. If it is gone the cursor stays where it is and
    // adopts what is there now, which is a visible change; silently carrying the
    // selection onto a different identity is not.
    if (_have_sel) {
      int at = -1;
      for (int i = 0; i < _count; i++) {
        if (memcmp(_paths[i].pubkey_prefix, _sel_key, sizeof(_sel_key)) == 0) { at = i; break; }
      }
      if (at >= 0) _sel = at;
      else _have_sel = false;
    }
    if (_sel >= _count) _sel = _count > 0 ? _count - 1 : 0;
    if (_sel < 0) _sel = 0;
    if (!_have_sel && _count > 0) captureSelection();
  }

  // Remembers which node the cursor is on, so the next refresh can follow it.
  void captureSelection() {
    if (_sel < 0 || _sel >= _count) { _have_sel = false; return; }
    memcpy(_sel_key, _paths[_sel].pubkey_prefix, sizeof(_sel_key));
    _have_sel = true;
  }

  // The route as text, one hop per name. An unresolved or colliding hash is drawn
  // `?` and never the first candidate: a hop byte is only a prefix of a repeater's
  // public key, so two nodes can share one.
  void routeText(const AdvertPath* p, char* out, size_t out_size, int* ambiguous) {
    out[0] = 0;
    *ambiguous = 0;
    int hops = (int) riftHopCount(p->path_len);
    uint8_t hsz = riftHashSize(p->path_len);
    if (p->path_len == 0xFF || hops == 0 || hsz == 0) return;

    size_t used = 0;
    for (int k = 0; k < hops; k++) {
      const char* label = "?";
      char resolved[32];
      int via = resolveHash(&p->path[k * hsz], hsz, resolved, sizeof(resolved));
      if (via == RIFT_RESOLVE_AMBIGUOUS) (*ambiguous)++;
      else if (via == RIFT_RESOLVE_UNIQUE) label = resolved;

      size_t need = strlen(label) + (used ? 3 : 0);
      if (used + need >= out_size - 4) {              // room for " ..."
        StrHelper::strncpy(out + used, " ...", out_size - used);
        return;
      }
      if (used) { memcpy(out + used, " \xAF ", 3); used += 3; }   // CP437 0xAF, a right guillemet
      size_t n = strlen(label);
      memcpy(out + used, label, n);
      used += n;
      out[used] = 0;
    }
  }

  // How tall a row is: 12, or 36 for the selected one because the route and the
  // detail line sit inside its block rather than in a bar at the bottom of the
  // screen. A detail bar cost four list rows for information about one node.
  int rowHeight(int i) const { return (i == _sel) ? 36 : 12; }

  // Scrolled so the selected row *and* its two detail rows are drawn. A selection
  // that is not on screen does not exist for the user.
  void clampScroll() {
    const int TOP = 56, BOTTOM = 226;
    if (_scroll > _sel) _scroll = _sel;
    if (_scroll < 0) _scroll = 0;
    for (;;) {
      int y = TOP;
      bool fits = false;
      for (int i = _scroll; i < _count; i++) {
        int h = rowHeight(i);
        if (y + h > BOTTOM) break;
        if (i == _sel) { fits = true; break; }
        y += h;
      }
      if (fits || _scroll >= _sel) break;
      _scroll++;
    }
  }

 public:
  RiftConstellationScreen(UITask* task)
     : _task(task), _count(0), _sel(0), _scroll(0), _have_sel(false),
       _last_refresh(0), _refreshed_once(false) {
    memset(_sel_key, 0, sizeof(_sel_key));
    for (int i = 0; i < RIFT_CONST_MAX; i++) _row_y[i] = -1;
  }

  int render(DisplayDriver& display) override {
    if (!_refreshed_once || millis() - _last_refresh >= 3000) {
      refresh();
      _last_refresh = millis();
      _refreshed_once = true;
    }
    display.setTextSize(1);

    char tmp[64];
    int counts[RIFT_HOPB_COUNT];
    for (int b = 0; b < RIFT_HOPB_COUNT; b++) counts[b] = 0;
    int maxhop = -1;
    for (int i = 0; i < _count; i++) {
      counts[bucketOf(_paths[i].path_len)]++;
      if (_paths[i].path_len != RIFT_PATH_UNKNOWN) {
        int h = (int) riftHopCount(_paths[i].path_len);
        if (h > maxhop) maxhop = h;
      }
    }

    // Geometry from design/redesign-2026-09/rift-nodes-spec.md: heading y 2,
    // bucket labels y 14, tracks y 24, counts y 30, column headings y 44, rows
    // from y 56 on the 12px pitch (14 of them), text at the row top and the
    // selection fill two pixels above it. Text stops at x 314; 318-319 is the
    // thumb's.

    // ---- heading
    display.setColor(rift_pal.mid);
    // "RECENT" is heard in the last 30 minutes - the same threshold as the filled
    // freshness marker - and "NODES" is what the cache holds. The cache is bounded,
    // so on a mesh larger than it the count is not a claim about the mesh; when it
    // is at capacity the eviction count says so, because that is the number which
    // decides whether the cache is too small.
    int recent = 0;
    for (int i = 0; i < _count; i++) {
      if (((uint32_t) millis() - _paths[i].recv_millis) < 1800000u) recent++;
    }
    int cache_used = the_mesh.getPathCacheUsed(), cache_size = the_mesh.getPathCacheSize();
    if (cache_used >= cache_size && the_mesh.getPathEvictions() > 0) {
      snprintf(tmp, sizeof(tmp), "%d RECENT %s %d NODES %s %u EVICT", recent, RIFT_DOT, _count,
               RIFT_DOT, (unsigned) the_mesh.getPathEvictions());
    } else if (_count == 0) {
      StrHelper::strncpy(tmp, "0 NODES", sizeof(tmp));
    } else {
      snprintf(tmp, sizeof(tmp), "%d RECENT %s %d NODES", recent, RIFT_DOT, _count);
    }
    display.drawTextLeftAlign(2, 2, tmp);
    if (maxhop >= 0) {
      snprintf(tmp, sizeof(tmp), "MAX %d HOPS", maxhop);
      display.drawTextRightAlign(314, 2, tmp);
    }

    // ---- bucket band. The bars compare the five with each other, not against an
    // absolute scale: on a mesh of six nodes an absolute scale draws five stubs.
    // The track is always drawn, so an empty bucket is a shape rather than an
    // absence.
    int maxc = 0;
    for (int b = 0; b < RIFT_HOPB_COUNT; b++) if (counts[b] > maxc) maxc = counts[b];
    for (int b = 0; b < RIFT_HOPB_COUNT; b++) {
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(BUCKET_X[b], 14, BUCKET_LABEL[b]);
      display.setColor(rift_pal.rule);
      display.drawRect(BUCKET_X[b], 24, BUCKET_BAR_W, 4);
      if (counts[b] > 0 && maxc > 0) {
        int w = (counts[b] * BUCKET_BAR_W + maxc / 2) / maxc;
        if (w < 2) w = 2;
        display.setColor(rift_pal.fg);
        display.fillRect(BUCKET_X[b], 24, w, 4);
      }
      display.setColor(rift_pal.fg);
      snprintf(tmp, sizeof(tmp), "%d", counts[b]);
      display.drawTextLeftAlign(BUCKET_X[b], 30, tmp);
    }

    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(2, 44, "NODE");
    display.drawTextLeftAlign(REACH_X, 44, "REACH");
    display.drawTextRightAlign(284, 44, "HOPS");
    display.drawTextRightAlign(314, 44, "AGE");

    for (int i = 0; i < RIFT_CONST_MAX; i++) _row_y[i] = -1;

    if (_count == 0) {
      display.setColor(rift_pal.mid);
      // "since boot" is part of the claim: this cache is cleared at startup, so an
      // empty list after a restart is the normal state and not a fault. The second
      // line says what to do about it.
      display.drawTextLeftAlign(2, 56, "NO NODES HEARD SINCE BOOT");
      display.drawTextLeftAlign(2, 68, "ADVERT NEAR on RIFT asks neighbours");
      renderNavBar(display, RIFT_NAV_NODES);
      return 1000;
    }

    clampScroll();

    const int TOP = 56, BOTTOM = 226;
    int y = TOP;
    int shown = 0;
    for (int i = _scroll; i < _count; i++) {
      int h = rowHeight(i);
      if (y + h > BOTTOM) break;
      _row_y[i] = y - 2;   // the fill's top, which is what a finger lands on
      shown++;

      bool sel = (i == _sel);
      AdvertPath* p = &_paths[i];
      bool known = !riftHopsUnknown(p->path_len);
      int hops = known ? (int) riftHopCount(p->path_len) : 0;

      if (sel) {
        display.setColor(rift_pal.accent);
        display.fillRect(0, y - 2, 316, h);
      }
      uint16_t ink = sel ? rift_pal.on_accent : rift_pal.fg;

      // Freshness is shape, not brightness: four grey levels collapse into each
      // other in sunlight, a filled versus hollow square does not.
      display.setColor(ink);
      uint32_t age_s = ((uint32_t) millis() - p->recv_millis) / 1000u;
      if (age_s < 1800u) display.fillRect(2, y + 1, 5, 5);
      else               display.drawRect(2, y + 1, 5, 5);

      char shown_name[24];
      riftTranslateUTF8(shown_name, p->name, sizeof(shown_name));
      display.setColor(ink);
      display.drawTextEllipsized(10, y, 120, shown_name);

      // Saturating at the last cell rather than clamping the printed value: the
      // digit still says 21 where the bar has run out of room to.
      int reach = known ? (hops > REACH_CELLS - 1 ? REACH_CELLS - 1 : hops) : -1;
      renderReach(display, y, reach, ink);

      if (known) {
        snprintf(tmp, sizeof(tmp), "%d", hops);
        display.setColor(ink);
      } else {
        StrHelper::strncpy(tmp, "?", sizeof(tmp));
        display.setColor(sel ? rift_pal.on_accent : rift_pal.mid);
      }
      display.drawTextRightAlign(284, y, tmp);

      ageText(p->recv_millis, tmp, sizeof(tmp));
      display.setColor(ink);
      display.drawTextRightAlign(314, y, tmp);

      if (sel) {
        // Two more rows inside the same fill: the route, then the action with the
        // node's type and heard time on the right. All in on_accent - the accent
        // as text was the old treatment for the action line, and text is the one
        // thing the accent no longer is.
        int ambiguous = 0;
        char route[64];
        routeText(p, route, sizeof(route), &ambiguous);

        display.setColor(rift_pal.on_accent);
        if (!known) {
          snprintf(tmp, sizeof(tmp), "route unknown %s heard by flood", RIFT_DOT);
          display.drawTextLeftAlign(10, y + 12, tmp);
        } else if (ambiguous > 0) {
          // "ambiguous hops", not "candidates": this counts positions in the route
          // that could not be resolved, and one such position may itself have had
          // several candidate nodes behind it.
          snprintf(tmp, sizeof(tmp), "route ambiguous %s %d hop%s", RIFT_DOT, ambiguous,
                   ambiguous == 1 ? "" : "s");
          display.drawTextLeftAlign(10, y + 12, tmp);
        } else if (route[0]) {
          snprintf(tmp, sizeof(tmp), "via %s", route);
          display.drawTextEllipsized(10, y + 12, 304, tmp);
        } else {
          display.drawTextLeftAlign(10, y + 12, "heard direct");
        }

        // Offered only when it would work. A repeater cannot receive a message, and
        // an action that will be refused is worse than no action shown.
        // The inverse of the rule above is just as true: an action with no hint
        // is an action nobody finds. Repeater control shipped without this line
        // and was invisible - the row looked exactly like one where ENTER does
        // nothing, so the key was never pressed.
        ContactInfo* contact = the_mesh.lookupContactByPubKey(p->pubkey_prefix, 6);
        const char* action = "not a contact yet";
        if (contact != NULL) {
          if (riftCanDirectMessage(contact->type))          action = "ENTER: message";
          else if (contact->type == RIFT_ADV_REPEATER)      action = "ENTER: control";
          else if (contact->type == RIFT_ADV_SENSOR)        action = "ENTER: read";
          else                                              action = "no action for this type";
        }
        display.drawTextLeftAlign(10, y + 24, action);

        // Type and the absolute time it was heard, when the clock can say. The
        // spec's "RIGHT: control" is not offered: left and right are the screen
        // change on every screen, and one screen where they are not is a trap.
        // The RSSI the design once asked for here does not exist - adverts are
        // cached without it - so it is omitted rather than invented.
        const char* type = (contact != NULL) ? riftAdvertTypeName(contact->type) : "unknown";
        uint32_t clk = the_mesh.getRTCClock()->getCurrentTime();
        char detail[48];
        if (clk >= 1600000000u && p->recv_timestamp >= 1600000000u) {
          int hh, mm;
          riftCivilFromEpoch(p->recv_timestamp, NULL, NULL, NULL, &hh, &mm);
          snprintf(detail, sizeof(detail), "%s %s %02d:%02d", type, RIFT_DOT, hh, mm);
        } else {
          StrHelper::strncpy(detail, type, sizeof(detail));   // no invented time
        }
        display.drawTextRightAlign(314, y + 24, detail);
      }

      y += h;
    }

    // The window pattern says where in the list this is; "N more" used to.
    // The expanded selection is three rows, so the window is counted in rows drawn.
    riftDrawThumb(display, TOP - 2, BOTTOM - (TOP - 2), _scroll, _count, shown);

    renderNavBar(display, RIFT_NAV_NODES);
    return 1000;
  }

  bool handleTouch(int x, int y) override {
    (void) x;
    if (_count == 0) return false;
    // Hit-tested against where the rows were drawn, because the selected one is
    // three rows tall and a fixed pitch would select the wrong node below it.
    for (int i = 0; i < _count; i++) {
      if (_row_y[i] < 0) continue;
      if (y >= _row_y[i] && y < _row_y[i] + rowHeight(i)) {
        _sel = i;
        captureSelection();
        return true;
      }
    }
    return false;
  }

  // Dragging moves the cursor, which is what up and down already do here - so a
  // finger and the trackball mean the same thing rather than two kinds of scroll
  // on one list. Rows vary in height, so the pitch is finger travel rather than a
  // row: the alternative is a gesture that moves a different distance depending
  // on which node happens to be under it.
  bool handleDrag(int dy) override {
    int steps = riftDragSteps(&_drag_residual, dy, RIFT_DRAG_PITCH);
    if (steps == 0) return false;
    int n = _sel + steps;
    if (n < 0) n = 0;
    if (n >= _count) n = _count > 0 ? _count - 1 : 0;
    if (n == _sel) return false;
    _sel = n;
    captureSelection();
    return true;
  }

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) { _task->cycleNavScreen(1); return true; }
    if (c == KEY_PREV || c == KEY_LEFT) { _task->cycleNavScreen(-1); return true; }
    // Not wrapped: this is a position in a list, and jumping from the last node to
    // the first reads as the list having moved rather than the cursor.
    if (c == KEY_UP)   { if (_sel > 0) { _sel--; captureSelection(); } return true; }
    if (c == KEY_DOWN) { if (_sel + 1 < _count) { _sel++; captureSelection(); } return true; }
    if (c == KEY_ENTER) {
      if (_count == 0) { riftLogf("NODES enter: list empty"); return true; }
      const uint8_t* key = _paths[_sel].pubkey_prefix;
      ContactInfo* contact = the_mesh.lookupContactByPubKey((uint8_t*) key, 6);
      // Logged because this decision is invisible when it goes the wrong way:
      // three outcomes look identical from the outside if none of them draws.
      riftLogf("NODES enter %02X%02X: %s", key[0], key[1],
               contact ? riftAdvertTypeName(contact->type) : "not a contact");
      if (contact == NULL) {
        _task->showAlert("Not a contact yet", 1400);
        return true;
      }
      if (!riftCanDirectMessage(contact->type)) {
        // Not a dead end any more. A repeater or room server cannot take a
        // direct message, but it can be logged into and read, so Enter opens
        // that panel instead of reporting what the key cannot do.
        _task->openRepeaterPanel(key);
        return true;
      }
      _task->startDirectMessage(key);
      return true;
    }
    return false;
  }
};

const int RiftConstellationScreen::BUCKET_X[RIFT_HOPB_COUNT] =
  { 2, 65, 128, 191, 254 };
const char* RiftConstellationScreen::BUCKET_LABEL[RIFT_HOPB_COUNT] =
  { "DIRECT", "1-2", "3-5", "6+", "NO ROUTE" };

// Placeholder nav screen - visual only, real functionality lands in a later milestone.
class RiftPlaceholderScreen : public RiftScreen {
  UITask* _task;
  int _nav_idx;
  const char* _title;
  const char* _detail;

public:
  RiftPlaceholderScreen(UITask* task, int nav_idx, const char* title, const char* detail)
     : _task(task), _nav_idx(nav_idx), _title(title), _detail(detail) { }

  int render(DisplayDriver& display) override {

    display.setColor(UIColor::primary_txt);
    display.setTextSize(2);
    display.drawTextCentered(display.width() / 2, display.height() / 2 - 20, _title);

    display.setColor(UIColor::secondary_txt);
    display.setTextSize(1);
    display.drawTextCentered(display.width() / 2, display.height() / 2 + 6, _detail);
    display.drawTextCentered(display.width() / 2, display.height() / 2 + 18, "coming soon");

    renderNavBar(display, _nav_idx);
    return 1000;
  }

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) { _task->cycleNavScreen(1); return true; }
    if (c == KEY_PREV || c == KEY_LEFT) { _task->cycleNavScreen(-1); return true; }
    return false;
  }
};

#ifdef RIFT_RADAR

// Shared scan result table. BLE advertisement callbacks fire on the Bluedroid
// task (core 0) while rendering happens on the loop task (core 1), so every
// touch of this table is inside the spinlock.
#define RIFT_RF_MAX 48

struct RfContact {
  uint8_t key[6];     // BSSID for Wi-Fi, MAC for BLE - the actual identity
  char name[24];      // display only; may be empty, duplicated or absent
  int8_t rssi;
  uint8_t channel;    // wifi only
  bool is_wifi;
  bool encrypted;     // wifi only
  unsigned long seen_at;
  unsigned long first_seen;   // for the "+N new" figure; set once, never refreshed
};

static RfContact rf_table[RIFT_RF_MAX];
static int rf_count = 0;
static portMUX_TYPE rf_mux = portMUX_INITIALIZER_UNLOCKED;

// Waterfall history: strongest signal seen per Wi-Fi channel, one column per
// completed sweep. This is not an SDR spectrum - the ESP32 gives no access to
// raw RF - it is observed 802.11 channel occupancy over time, which is what
// actually matters for picking a clear channel or spotting congestion.
#define RIFT_WF_CHANNELS 14    // index 1..13 used
#define RIFT_WF_SLICES   40

static int8_t wf_hist[RIFT_WF_SLICES][RIFT_WF_CHANNELS];
static int wf_count = 0;
static int wf_head = RIFT_WF_SLICES - 1;   // newest slice

static void wfPushSlice(const int8_t* per_channel) {
  wf_head = (wf_head + 1) % RIFT_WF_SLICES;
  if (wf_count < RIFT_WF_SLICES) wf_count++;
  memcpy(wf_hist[wf_head], per_channel, RIFT_WF_CHANNELS);
}

static void wfClear() {
  wf_count = 0;
  wf_head = RIFT_WF_SLICES - 1;
  memset(wf_hist, 0, sizeof(wf_hist));
}

// Insert or refresh, keyed on hardware address rather than display name. Names
// are unreliable as identity: hidden Wi-Fi networks report an empty SSID, and
// BLE devices frequently share a name (several "AirPods" in one room), so
// keying on the name both duplicated hidden networks on every sweep and
// collapsed distinct BLE devices into one row.
static void rfUpsert(const uint8_t* key, const char* name, int8_t rssi, uint8_t channel,
                     bool is_wifi, bool encrypted) {
  portENTER_CRITICAL(&rf_mux);
  int slot = -1;
  for (int i = 0; i < rf_count; i++) {
    if (rf_table[i].is_wifi == is_wifi && memcmp(rf_table[i].key, key, 6) == 0) {
      slot = i;
      break;
    }
  }
  bool is_new = (slot < 0);
  if (slot < 0) {
    if (rf_count < RIFT_RF_MAX) {
      slot = rf_count++;
    } else {
      // Table full - let stronger signals displace weaker, but never a watched
      // device. A watched one is usually the weak one: it is being tracked because
      // it comes and goes, and in a crowded room it would be pushed out by whatever
      // is nearer, after which the presence check would report it gone while the
      // radio was still hearing it. That would look exactly like a broken alert.
      int weakest = -1;
      for (int i = 0; i < rf_count; i++) {
        if (rfWatchFind(rf_table[i].key, rf_table[i].is_wifi) >= 0) continue;
        if (weakest < 0 || rf_table[i].rssi < rf_table[weakest].rssi) weakest = i;
      }
      // every slot is watched, which needs the watch list to be as large as the
      // table; impossible today, but it must not index -1 if that ever changes
      if (weakest < 0) { portEXIT_CRITICAL(&rf_mux); return; }
      // The guard above only protected a watched device already in the table. A
      // watched one arriving weaker than everything present was turned away here,
      // and the presence check then said "not heard" while the radio heard it -
      // the exact failure the comment above says this code prevents. A watched
      // arrival takes the weakest slot whatever its signal.
      bool watched = rfWatchFind(key, is_wifi) >= 0;
      if (!watched && rssi <= rf_table[weakest].rssi) { portEXIT_CRITICAL(&rf_mux); return; }
      slot = weakest;
    }
  }
  RfContact* e = &rf_table[slot];
  memcpy(e->key, key, 6);
  StrHelper::strncpy(e->name, (name && name[0]) ? name : "(hidden)", sizeof(e->name));
  e->rssi = rssi;
  e->channel = channel;
  e->is_wifi = is_wifi;
  e->encrypted = encrypted;
  e->seen_at = millis();
  if (is_new) e->first_seen = e->seen_at;
  portEXIT_CRITICAL(&rf_mux);
}

// Drop everything a now-disabled radio had found. Ageing would remove it eventually,
// but "eventually" is minutes of showing devices nobody is looking for any more.
static void rfDropSource(bool drop_wifi, bool drop_ble) {
  if (!drop_wifi && !drop_ble) return;
  portENTER_CRITICAL(&rf_mux);
  int w = 0;
  for (int i = 0; i < rf_count; i++) {
    bool drop = rf_table[i].is_wifi ? drop_wifi : drop_ble;
    if (drop) continue;
    if (w != i) rf_table[w] = rf_table[i];
    w++;
  }
  rf_count = w;
  portEXIT_CRITICAL(&rf_mux);
}

static void rfClear() {
  portENTER_CRITICAL(&rf_mux);
  rf_count = 0;
  portEXIT_CRITICAL(&rf_mux);
}

// drop entries not heard recently, so the picture reflects what's here now
//
// A watched device is kept for RIFT_WATCH_GONE_MILLIS rather than the table's
// own age. rfWatchCheck judges presence by the entry's age against that longer
// window, and this ran first with the shorter one, so an entry could never be
// found aged between 45 and 90 seconds: the 90-second window was dead code and
// a beacon quiet for a minute was reported gone, then alerted on again.
static void rfAgeOut() {
  unsigned long now = millis();
  portENTER_CRITICAL(&rf_mux);
  int w = 0;
  for (int i = 0; i < rf_count; i++) {
    unsigned long keep = rfWatchFind(rf_table[i].key, rf_table[i].is_wifi) >= 0
                         ? RIFT_WATCH_GONE_MILLIS : RIFT_RF_AGE_MILLIS;
    if (now - rf_table[i].seen_at <= keep) {
      if (w != i) rf_table[w] = rf_table[i];
      w++;
    }
  }
  rf_count = w;
  portEXIT_CRITICAL(&rf_mux);
}

// Toggle a mark. Returns true if it is now watched, false if it was removed or
// there was no room. The name is copied for display only - the address is what is
// matched, because a BLE name is often absent, duplicated, or a rendering of the
// address itself.
// Three outcomes, not two. A bool could not distinguish "removed" from "refused
// because the list is full" - both leave the device absent from the list, so the
// caller's rfWatchFind check reported a full list as a removal and told the user
// something had been taken off when nothing had changed.
enum RfWatchResult { RF_WATCH_ADDED, RF_WATCH_REMOVED, RF_WATCH_FULL };

static RfWatchResult rfWatchToggle(const uint8_t* key, bool is_wifi, const char* name) {
  // Under rf_mux because rfUpsert reads this table from the BLE advertisement
  // callback on core 0, inside that same lock, to decide what it may evict. Taking
  // it there and not here meant the reader was holding a lock the writer ignored,
  // which is no protection: the shift below could be observed half done.
  portENTER_CRITICAL(&rf_mux);
  int at = rfWatchFind(key, is_wifi);
  if (at >= 0) {
    for (int i = at; i + 1 < rf_watch_count; i++) rf_watch[i] = rf_watch[i + 1];
    rf_watch_count--;
    portEXIT_CRITICAL(&rf_mux);
    return RF_WATCH_REMOVED;
  }
  if (rf_watch_count >= RIFT_WATCH_MAX) {
    portEXIT_CRITICAL(&rf_mux);
    return RF_WATCH_FULL;
  }
  RfWatch* w = &rf_watch[rf_watch_count++];
  memset(w, 0, sizeof(*w));
  memcpy(w->key, key, 6);
  w->is_wifi = is_wifi;
  StrHelper::strncpy(w->name, (name != NULL && name[0]) ? name : "(unnamed)", sizeof(w->name));
  // present false, so a device already in range announces itself on the next sweep
  // rather than being silently assumed present from the moment it was marked
  portEXIT_CRITICAL(&rf_mux);
  return RF_WATCH_ADDED;
}

// How many watched devices are currently present, and the name of one of them. The
// present flag is maintained by rfWatchCheck below, so this reads a decision that
// has already been made rather than re-scanning the table on every frame - render
// runs several times a second and shares its SPI bus with the radio.
static int rfWatchPresent(const char** name_out) {
  int n = 0;
  const char* first = NULL;
  for (int i = 0; i < rf_watch_count; i++) {
    if (!rf_watch[i].present) continue;
    if (first == NULL) first = rf_watch[i].name;
    n++;
  }
  if (name_out) *name_out = first;
  return n;
}

// Called after each completed sweep. Only the transition alerts: a device sitting
// in range must not alert every 700ms, and one at the edge of range must not alert
// every time a sweep happens to catch it.
static void rfWatchCheck(UITask* task) {
  unsigned long now = millis();
  for (int i = 0; i < rf_watch_count; i++) {
    RfWatch* w = &rf_watch[i];

    // A watch on a radio that is currently switched off is left alone. Judging it
    // absent would report a device as gone because the user stopped listening, and
    // then alert again the moment they switched the radio back on.
    if (w->is_wifi ? !riftScanWifi() : !riftScanBle()) continue;

    bool seen = false;
    portENTER_CRITICAL(&rf_mux);
    for (int j = 0; j < rf_count; j++) {
      if (rf_table[j].is_wifi == w->is_wifi && memcmp(rf_table[j].key, w->key, 6) == 0) {
        seen = (now - rf_table[j].seen_at) <= RIFT_WATCH_GONE_MILLIS;
        break;
      }
    }
    portEXIT_CRITICAL(&rf_mux);

    if (seen && !w->present) {
      w->present = true;
      if (w->last_alert == 0 || (now - w->last_alert) >= RIFT_WATCH_REARM_MILLIS) {
        w->last_alert = now;
        task->proximityAlert(w->name, w->is_wifi);
      } else {
        // suppressed, but recorded - otherwise a missing alert and a device that
        // never arrived look the same afterwards
        riftLogf("watch %s back, alert held", w->name);
      }
    } else if (!seen && w->present) {
      w->present = false;
      riftLogf("watch gone: %s", w->name);
    }
  }
}

class RiftBleCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    // copy immediately - getName()/toString() return temporaries whose c_str()
    // would dangle past the end of this statement
    BLEAddress addr = dev.getAddress();

    char name[24];
    if (dev.haveName()) {
      StrHelper::strncpy(name, dev.getName().c_str(), sizeof(name));
    } else {
      StrHelper::strncpy(name, addr.toString().c_str(), sizeof(name));
    }

    uint8_t key[6];
    memcpy(key, addr.getNative(), 6);   // MAC is the identity, not the name
    rfUpsert(key, name, (int8_t) dev.getRSSI(), 0, false, false);
  }
};
static RiftBleCallbacks ble_callbacks;

// set on core 0 by the scan-completion callback, consumed by poll() on core 1
static volatile bool ble_scan_done = false;
static void onBleScanComplete(BLEScanResults results) { ble_scan_done = true; }

// RADAR: passive Wi-Fi + BLE situational awareness.
//
// Wi-Fi and BLE share one 2.4GHz PHY and antenna on the ESP32-S3, and the
// coexistence arbiter time-slices them - running both at once halves each. So
// this alternates strictly: Wi-Fi scan, then BLE scan, then repeat.
//
// Everything here must stay non-blocking. There is no watchdog on the main
// loop (loopTaskWDTEnabled is false, and the IDF task WDT only watches core 0
// idle), so a blocking call would silently starve the LoRa radio rather than
// panicking. Both scan APIs have a blocking overload that is easy to hit by
// accident - see the comments at each call site.
class RiftRadarScreen : public RiftScreen {
  UITask* _task;

  enum ScanState { OFF, START_WIFI, WIFI_RUNNING, START_BLE, BLE_RUNNING, STOPPING };
  enum View { VIEW_BANDS, VIEW_WATERFALL, VIEW_WATCHES };
  ScanState _state;
  View _view;
  bool _want_active;   // set by screen changes; acted on from the main loop
  bool _wifi_up, _ble_up;
  // Teardown completion must be tracked separately: _ble_up deliberately stays
  // true after teardown (BLEDevice::deinit is avoided), so using it as the
  // "needs teardown" test would restart the cycle forever.
  bool _torn_down;
  // wrap-safe pacing: when the wait started, and how long to wait (0 = ready
  // now). Same reason as _last_refresh - a future deadline breaks at the
  // millis() wrap, and here it would leave the scan state machine spinning.
  unsigned long _wait_since;
  unsigned long _wait_ms;
  unsigned long _ble_started;   // when the running BLE scan was started, for its deadline
  int _scroll;
  int _last_n;
  int _watch_sel;      // cursor in the watch list
  int _drag_residual = 0;   // finger travel not yet worth a row

  // The selection is an identity, not a row. The list is sorted by signal strength
  // and re-sorts every sweep, so a cursor held as an index would slide onto a
  // different device whenever signals crossed - and then ENTER would mark the wrong
  // one. The index below is where it was last drawn; the key is what it means.
  uint8_t _sel_key[6];
  bool _sel_is_wifi;
  char _sel_name[24];
  bool _have_sel;
  int  _sel_idx;
  bool _resel;        // set by up/down: adopt whatever is at _sel_idx next frame
  // Whether any sweep has reported since the radios came up, so an empty table can
  // say "listening" rather than "nothing found" while the first one is running.
  bool _scanned_once = false;
  int _watch_first = 0;   // first watched entry drawn; the window follows _watch_sel


  void beginWifi() {
    if (!_wifi_up) {
      WiFi.mode(WIFI_STA);
      WiFi.disconnect(false, false);   // never associate; listen only
      _wifi_up = true;
    }
    // async=true is essential - the default-argument form blocks up to 10s.
    // passive=true means no probe requests are transmitted.
    WiFi.scanNetworks(true, true, true, RIFT_WIFI_DWELL_MILLIS);
  }

  void collectWifi(int n) {
    _scanned_once = true;
    int8_t per_channel[RIFT_WF_CHANNELS];
    memset(per_channel, 0, sizeof(per_channel));

    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      int8_t rssi = (int8_t) WiFi.RSSI(i);
      uint8_t ch = (uint8_t) WiFi.channel(i);

      // BSSID is the identity - hidden networks report an empty SSID
      uint8_t key[6];
      const uint8_t* bssid = WiFi.BSSID(i);
      if (bssid != NULL) memcpy(key, bssid, 6); else memset(key, 0, 6);

      rfUpsert(key, ssid.c_str(), rssi, ch, true, WiFi.encryptionType(i) != WIFI_AUTH_OPEN);

      // strongest signal seen on each channel this sweep (0 means "nothing")
      if (ch < RIFT_WF_CHANNELS && (per_channel[ch] == 0 || rssi > per_channel[ch])) {
        per_channel[ch] = rssi;
      }
    }
    WiFi.scanDelete();   // free the result array promptly

    wfPushSlice(per_channel);
  }

  // Returns whether a scan is actually running. start() fails when the previous
  // scan's stop is still in flight, and the return value was ignored: the state
  // machine then sat in BLE_RUNNING waiting for a completion that was never
  // coming, with no Wi-Fi sweeps, no ageing and no watch checks, until the user
  // left the screen. Nothing on screen said so.
  bool beginBle() {
    if (!_ble_up) {
      BLEDevice::init("");
      _ble_up = true;
    }
    BLEScan* scan = BLEDevice::getScan();
    scan->setActiveScan(false);   // passive: do NOT transmit SCAN_REQ
    scan->setInterval(100);
    scan->setWindow(99);
    scan->setAdvertisedDeviceCallbacks(&ble_callbacks, false, true);
    ble_scan_done = false;
    _ble_started = millis();
    // the function-pointer overload returns immediately; start(duration, bool)
    // would block for the whole duration
    if (!scan->start(RIFT_BLE_DWELL_SECS, onBleScanComplete, false)) {
      riftLogf("radar: BLE scan start refused");
      return false;
    }
    return true;
  }

public:
  RiftRadarScreen(UITask* task)
     : _task(task), _state(OFF), _view(VIEW_BANDS), _want_active(false),
       _wifi_up(false), _ble_up(false), _torn_down(true),
       _wait_since(0), _wait_ms(0), _ble_started(0), _scroll(0), _last_n(0),
       _watch_sel(0), _sel_is_wifi(false), _have_sel(false), _sel_idx(0), _resel(false) {
    memset(_sel_key, 0, sizeof(_sel_key));
    _sel_name[0] = 0;
  }

  // These only record intent. Screen changes can originate from a mesh callback,
  // and tearing the BT controller down from inside the LoRa receive path crashes
  // the device - so the actual work happens in service(), on the main loop.
  //
  // service() is still driven from UITask::loop() on every iteration whichever
  // screen is showing. It must not move in here: teardown has to keep running
  // after the user has navigated away, which is exactly when onLeave() has
  // already fired.
  void onEnter() override { _want_active = true; }
  void onLeave() override { _want_active = false; }

  // Ask the radios to stop, but do NOT deinit yet - BLEDevice::deinit() while a
  // scan is still winding down panics the device. The actual teardown happens
  // in the STOPPING state after a grace period.
  void beginTeardown() {
    if (_ble_up) {
      BLEDevice::getScan()->setAdvertisedDeviceCallbacks(NULL);   // no late callbacks
      BLEDevice::getScan()->stop();
    }
    _state = STOPPING;
    _wait_since = millis();
    _wait_ms = RIFT_SCAN_STOP_GRACE_MILLIS;
  }

  void finishTeardown() {
    if (_ble_up) {
      // Deliberately NOT calling BLEDevice::deinit(): in this ESP32 core it
      // panics when a scan has recently been active, and no amount of grace
      // period made it reliable. The stack stays initialised and idle - it
      // transmits nothing once the scan is stopped. _ble_up stays true so we
      // don't re-init on the next visit.
      BLEDevice::getScan()->clearResults();
    }
    if (_wifi_up) {
      WiFi.scanDelete();
      WiFi.mode(WIFI_OFF);
      _wifi_up = false;
    }
    rfClear();   // hand the heap back; the mesh is the primary job
    wfClear();   // history would be stale and misleading on return
    // Presence is forgotten with the table it was derived from. Left set, the lamp
    // would light on re-entry from a sighting made minutes ago in another room,
    // before any sweep had confirmed it - which is the one thing an indicator must
    // never do. It also means a device still in range announces itself again, which
    // is the same choice the settings loader makes for a watch read off disk.
    for (int i = 0; i < rf_watch_count; i++) rf_watch[i].present = false;
    _state = OFF;
    _torn_down = true;
    _scanned_once = false;   // the next visit starts with "listening" again
  }

  // Driven every main-loop iteration, whichever screen is showing, so that
  // teardown still happens after the user navigates away.
  void service() {
    if (!_want_active) {
      if (_state == STOPPING) {
        if (millis() - _wait_since >= _wait_ms) finishTeardown();
      } else if (!_torn_down) {
        beginTeardown();
      }
      return;
    }
    if (_state == OFF || _state == STOPPING) {
      _state = START_WIFI;   // came back before teardown finished
      _wait_ms = 0;
      _torn_down = false;
    }

    if (_wait_ms != 0) {
      if (millis() - _wait_since < _wait_ms) return;
      _wait_ms = 0;
    }

    switch (_state) {
      case START_WIFI:
        if (!riftScanWifi()) { _state = START_BLE; break; }
        beginWifi();
        _state = WIFI_RUNNING;
        break;

      case WIFI_RUNNING: {
        int n = WiFi.scanComplete();
        if (n >= 0) {
          collectWifi(n);
          _state = START_BLE;
        } else if (n == WIFI_SCAN_FAILED) {
          _state = START_BLE;   // don't get stuck; try the other radio
        }
        break;
      }

      case START_BLE:
        // BLE off, or a scan that would not start: the cycle ends here instead
        // of in BLE_RUNNING. Ageing, the presence check and the gap all have to
        // happen exactly once per cycle, so they are duplicated here rather
        // than being skipped. A refused start gets the gap too, which is also
        // the time the previous stop needs to finish.
        if (!riftScanBle() || !beginBle()) {
          rfAgeOut();
          rfWatchCheck(_task);
          _state = START_WIFI;
          _wait_since = millis();
          _wait_ms = RIFT_SCAN_GAP_MILLIS;
          break;
        }
        _state = BLE_RUNNING;
        break;

      case BLE_RUNNING: {
        // The completion callback is the normal exit. The deadline is the other
        // one: a scan that started but never reports done would otherwise hold
        // this state for ever, and the screen would freeze on stale entries with
        // every watch silent. Dwell plus a margin; stop() is what makes the
        // callback fire if the stack is merely late.
        bool overdue = (millis() - _ble_started) >= (RIFT_BLE_DWELL_SECS * 1000UL + 1500UL);
        if (overdue && !ble_scan_done) {
          riftLogf("radar: BLE scan overdue, stopping it");
          BLEDevice::getScan()->stop();
        }
        if (ble_scan_done || overdue) {
          _scanned_once = true;
          BLEDevice::getScan()->clearResults();   // keep the internal map bounded
          rfAgeOut();
          // after ageing, so a device that has just dropped out of the table is
          // judged absent rather than lingering for one more cycle
          rfWatchCheck(_task);
          _state = START_WIFI;
          _wait_since = millis();
          _wait_ms = RIFT_SCAN_GAP_MILLIS;
        }
        break;
      }

      default:
        break;
    }
  }

  // colour ramp for observed signal strength; 0 means nothing heard
  // This is the one place brightness genuinely is the quantity, so it is the one
  // place a ramp belongs. The old four colours - orange, orange, white, grey -
  // had no order, so a cell could not be read as a value at all.
  //
  // Four steps, not twelve: at 6x8 with no antialiasing, finer steps are not
  // distinguishable. The ramp runs from mid-grey to full contrast rather than
  // from the background up, since the darkest steps are the first to disappear
  // under reflected light. In day mode it inverts, because a light grey on white
  // is invisible.
  static ColorVal wfColor(int8_t rssi) {
    if (rssi == 0) return 0;   // silent: leave the background
    static const ColorVal NIGHT[4] = { 0x6B6D, 0x9CD3, 0xC618, 0xFFFF };
    static const ColorVal DAY[4]   = { 0x9CD3, 0x6B6D, 0x39E7, 0x0000 };
    const ColorVal* ramp = rift_day_mode ? DAY : NIGHT;
    if (rssi >= -55) return ramp[3];
    if (rssi >= -70) return ramp[2];
    if (rssi >= -85) return ramp[1];
    return ramp[0];
  }

  int renderWaterfall(DisplayDriver& display) {
    // The lamp wins the row here too. Someone reading the waterfall still wants to
    // know whether the thing they marked has turned up, and "WATERFALL" is a label
    // for a view they are already looking at.
    if (!renderWatchLamp(display)) {
      renderHeading(display, (_state == OFF) ? "IDLE" : "WATERFALL");
    }

    display.setTextSize(1);

    // channels across, time down (newest at top)
    const int left = 30;
    const int top = 30;
    const int cell_w = 21;
    const int cell_h = 5;
    const int channels = 13;
    const int rows = 30;

    // The strongest cell in the frame is drawn in the accent, which is the only
    // way to find the current peak without comparing shades by eye.
    int8_t peak = 0;
    for (int r = 0; r < wf_count && r < rows && r < RIFT_WF_SLICES; r++) {
      int idx = (wf_head - r + RIFT_WF_SLICES * 2) % RIFT_WF_SLICES;
      for (int ch = 1; ch <= channels; ch++) {
        int8_t v = wf_hist[idx][ch];
        if (v != 0 && (peak == 0 || v > peak)) peak = v;
      }
    }

    bool peak_drawn = false;
    for (int r = 0; r < wf_count && r < rows && r < RIFT_WF_SLICES; r++) {
      int idx = (wf_head - r + RIFT_WF_SLICES * 2) % RIFT_WF_SLICES;
      int y = top + r * cell_h;
      if (y + cell_h > 180) break;

      for (int ch = 1; ch <= channels; ch++) {
        int8_t v = wf_hist[idx][ch];
        if (v == 0) continue;   // nothing heard - leave background
        if (!peak_drawn && v == peak) {
          display.setColor(rift_pal.accent);
          peak_drawn = true;
        } else {
          display.setColor(wfColor(v));
        }
        display.fillRect(left + (ch - 1) * cell_w, y, cell_w - 2, cell_h - 1);
      }
    }

    if (wf_count == 0) {
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(left, top + 40, "waiting for first sweep...");
    }

    // time axis - without it the vertical dimension is unlabelled and the plot
    // cannot be read as history at all
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(2, top, "now");
    display.drawTextLeftAlign(2, top + 10 * cell_h, "-1m");
    display.drawTextLeftAlign(2, top + 20 * cell_h, "-2m");

    // channel axis
    char tmp[40];
    for (int ch = 1; ch <= channels; ch += 3) {
      sprintf(tmp, "%s%d", ch == 1 ? "CH" : "", ch);
      display.drawTextLeftAlign(left + (ch - 1) * cell_w, 186, tmp);
    }

    // Read the answer out rather than leaving it to be inferred from the
    // shading: knowing which channel is congested is why this screen is opened.
    // Silence sorts below the weakest signal, and ties are broken by the lower
    // channel number, so the answer does not flicker between redraws.
    int busiest = 1, quietest = 1;
    int best = -128, worst = 127;
    for (int ch = 1; ch <= channels; ch++) {
      int8_t m = 0;
      for (int r = 0; r < wf_count && r < rows && r < RIFT_WF_SLICES; r++) {
        int idx = (wf_head - r + RIFT_WF_SLICES * 2) % RIFT_WF_SLICES;
        int8_t v = wf_hist[idx][ch];
        if (v != 0 && (m == 0 || v > m)) m = v;
      }
      int strength = (m == 0) ? -128 : (int) m;
      if (strength > best)  { best = strength;  busiest = ch; }
      if (strength < worst) { worst = strength; quietest = ch; }
    }

    display.setColor(rift_pal.rule);
    display.fillRect(0, 196, display.width(), 1);
    display.setColor(rift_pal.mid);
    if (wf_count > 0) {
      snprintf(tmp, sizeof(tmp), "busiest CH%d - quietest CH%d", busiest, quietest);
      display.drawTextLeftAlign(2, 206, tmp);
    }
    display.drawTextRightAlign(display.width() - 2, 206, "ENTER: back");

    renderNavBar(display, RIFT_NAV_RADAR);
    return 700;
  }

  int render(DisplayDriver& display) override {
    if (_view == VIEW_WATERFALL) return renderWaterfall(display);
    if (_view == VIEW_WATCHES) return renderWatches(display);

    // No heading while scanning. `LIVE` said nothing the screen did not already
    // show - the device count below it is the evidence that scanning is happening,
    // and it moves. The two states that are *not* obvious keep their heading:
    // `IDLE` means the count is stale rather than zero, and `INITIALISING` means
    // the radios have not come up yet, so an empty list means nothing yet.
    //
    // The heading therefore appears exactly when there is something to say. That
    // is deliberate rather than a flicker.
    // Geometry from design/redesign-2026-09/rift-radar-spec.md: heading y 2, the
    // count at y 16 in size 3, bands at y 48/60/72 with their cells from x 92,
    // column headings y 88, nine rows from y 100, footer y 214. Text stops at
    // x 314; 318-319 is the thumb's.
    char tmp[64];

    // The heading is the source and the state - which radios are on is the
    // distinction the source switch exists to make, and "0 devices" with a radio
    // off is a different reading from "0 devices" with it on. The watch lamp
    // takes the row when a watched device is present.
    if (!renderWatchLamp(display)) {
      const char* src = (rift_radar_src == RIFT_SRC_BOTH) ? "WIFI+BLE"
                      : (rift_radar_src == RIFT_SRC_WIFI) ? "WIFI" : "BLE";
      const char* st = (_state == OFF) ? "IDLE"
                     : (!_wifi_up && !_ble_up) ? "INITIALISING" : "SCANNING";
      snprintf(tmp, sizeof(tmp), "%s %s %s", src, RIFT_DOT, st);
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(2, 2, tmp);
      // a claim the user is entitled to see on the device, not only in the README
      display.drawTextRightAlign(314, 2, "nothing transmitted");
    }

    // snapshot under the lock, then draw without holding it
    RfContact snap[RIFT_RF_MAX];
    int n;
    unsigned long now_ms = millis();
    portENTER_CRITICAL(&rf_mux);
    n = rf_count;
    memcpy(snap, rf_table, sizeof(RfContact) * n);
    portEXIT_CRITICAL(&rf_mux);
    _last_n = n;

    int new_n = 0;
    for (int i = 0; i < n; i++) {
      if (now_ms - snap[i].first_seen < 60000) new_n++;
    }

    // The one large value on the screen. "Is there anything around me" is the
    // question RADAR exists for, and it was previously answered by counting
    // dots in a scatter plot. "--" until the first sweep has reported.
    display.setTextSize(3);
    display.setColor(rift_pal.fg);
    bool first_sweep_pending = (_state != OFF && n == 0 && !_scanned_once);
    if (first_sweep_pending) strcpy(tmp, "--"); else sprintf(tmp, "%d", n);
    display.drawTextLeftAlign(2, 16, tmp);
    int count_w = (int) strlen(tmp) * 18;

    display.setTextSize(1);
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(2 + count_w + 6, 32, n == 1 ? "DEVICE" : "DEVICES");

    // "and is that changing" - the second half of the question. Kept from the
    // previous screen, in mid rather than accent: it is information, not a warning.
    if (new_n > 0) {
      snprintf(tmp, sizeof(tmp), "+%d new in 60s", new_n);
      display.setColor(rift_pal.mid);
      display.drawTextRightAlign(314, 32, tmp);
    }

    // Distance bands replace the scatter. The scatter placed each blip by
    // `i & 15` - its index in a table that compacts whenever a device ages out -
    // so blips moved with nothing having moved. One countable cell per device
    // in a signal-strength band says the same thing and stays still. The dBm
    // boundary sits in its own column so the three cell rows start on one x.
    static const int BAND_Y[3] = { 48, 60, 72 };
    static const char* BAND_LABEL[3] = { "CLOSE", "MID", "FAR" };
    static const char* BAND_RANGE[3] = { "> -60", "-60/-80", "< -80" };
    const int CELL_X = 92;
    const int CELL_SLOTS = 28;   // 92 + 28*8 = 316; the last slot is the overflow mark

    for (int b = 0; b < 3; b++) {
      display.setColor(rift_pal.fg);
      display.drawTextLeftAlign(2, BAND_Y[b], BAND_LABEL[b]);
      display.setColor(rift_pal.dim);
      display.drawTextLeftAlign(38, BAND_Y[b], BAND_RANGE[b]);

      int cnt = 0;
      for (int i = 0; i < n; i++) {
        int s = snap[i].rssi;
        int band = (s > -60) ? 0 : (s > -80 ? 1 : 2);
        if (band == b) cnt++;
      }

      display.setColor(rift_pal.fg);
      int drawn = (cnt > CELL_SLOTS) ? CELL_SLOTS - 1 : cnt;
      for (int c = 0; c < drawn; c++) {
        int x = CELL_X + c * 8;
        // FAR is hollow rather than a dimmer grey: brightness steps disappear
        // under reflected light outdoors, form does not
        if (b == 2) display.drawRect(x, BAND_Y[b], 6, 8);
        else        display.fillRect(x, BAND_Y[b], 6, 8);
      }
      if (cnt > CELL_SLOTS) {
        // more than the row can hold: a glyph that says "more", in fg. Overflow is
        // not a warning, and the accent is spoken for.
        display.setColor(rift_pal.fg);
        display.drawTextLeftAlign(CELL_X + (CELL_SLOTS - 1) * 8, BAND_Y[b], "\xAF");
      }
    }

    // strongest first
    for (int i = 0; i < n - 1; i++) {
      for (int j = i + 1; j < n; j++) {
        if (snap[j].rssi > snap[i].rssi) {
          RfContact t = snap[i]; snap[i] = snap[j]; snap[j] = t;
        }
      }
    }
    if (_scroll >= n) _scroll = (n > 0) ? n - 1 : 0;

    // Resolve the selection against this frame's ordering. If the marked device is
    // still in the list the cursor follows it wherever it sorted to; if it is gone,
    // or the user just moved, the cursor adopts whatever is at that position now.
    if (n == 0) {
      _have_sel = false;
      _sel_idx = 0;
    } else {
      int found = -1;
      if (_have_sel && !_resel) {
        for (int i = 0; i < n; i++) {
          if (snap[i].is_wifi == _sel_is_wifi && memcmp(snap[i].key, _sel_key, 6) == 0) {
            found = i; break;
          }
        }
      }
      if (found >= 0) {
        _sel_idx = found;
      } else {
        if (_sel_idx >= n) _sel_idx = n - 1;
        if (_sel_idx < 0) _sel_idx = 0;
        memcpy(_sel_key, snap[_sel_idx].key, 6);
        _sel_is_wifi = snap[_sel_idx].is_wifi;
        StrHelper::strncpy(_sel_name, snap[_sel_idx].name, sizeof(_sel_name));
        _have_sel = true;
      }
      _resel = false;
    }

    // Scroll follows the cursor. Without this the selection could sit outside the
    // visible window, which is the state NODES was just fixed for having.
    const int LIST_TOP = 100, ROWS = 9;
    if (_sel_idx < _scroll) _scroll = _sel_idx;
    if (_sel_idx >= _scroll + ROWS) _scroll = _sel_idx - ROWS + 1;
    if (_scroll < 0) _scroll = 0;

    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(2, 88, "DEVICE");
    display.drawTextLeftAlign(254, 88, "RSSI");
    display.drawTextLeftAlign(290, 88, "SEEN");

    int y = LIST_TOP;
    int shown = 0;
    for (int i = _scroll; i < n && shown < ROWS; i++, y += RIFT_LINE_H, shown++) {
      bool is_sel = _have_sel && (i == _sel_idx);
      bool watched = rfWatchFind(snap[i].key, snap[i].is_wifi) >= 0;

      // The selected row is the accent fill every other list uses. A watched
      // device carries a filled 5x5 in its own column: two marks rather than one
      // colour, because a watched device that is also selected has to read as
      // both, and colour cannot say two things in one cell.
      if (is_sel) {
        display.setColor(rift_pal.accent);
        display.fillRect(0, y - 2, 316, 12);
      }
      uint16_t ink = is_sel ? rift_pal.on_accent : rift_pal.fg;
      uint16_t sub = is_sel ? rift_pal.on_accent : rift_pal.mid;

      display.setColor(sub);
      display.drawTextLeftAlign(2, y, snap[i].is_wifi ? "W" : "B");

      char filtered[sizeof(snap[i].name)];
      riftTranslateUTF8(filtered, snap[i].name, sizeof(filtered));
      display.setColor(ink);
      display.drawTextEllipsized(14, y, 222, filtered);

      if (watched) {
        display.setColor(ink);
        display.fillRect(243, y + 1, 5, 5);
      }

      sprintf(tmp, "%d", snap[i].rssi);
      display.setColor(ink);
      display.drawTextRightAlign(272, y, tmp);

      // How long since it was last heard, so a row that is about to age out reads
      // as one. "now" inside two seconds; the age otherwise.
      uint32_t since = (uint32_t) (now_ms - snap[i].seen_at);
      if (since < 2000u) strcpy(tmp, "now"); else riftFormatAge(since, tmp, sizeof(tmp));
      display.setColor(ink);
      display.drawTextRightAlign(314, y, tmp);
    }

    if (n == 0) {
      display.setColor(rift_pal.mid);
      if (first_sweep_pending) {
        display.drawTextLeftAlign(2, LIST_TOP, "listening...");
      } else if (_state == OFF) {
        display.drawTextLeftAlign(2, LIST_TOP, "Not scanning.");
      } else {
        display.drawTextLeftAlign(2, LIST_TOP, "No devices in the last scan.");
        display.drawTextLeftAlign(2, LIST_TOP + 12, "S switches source; walls cost 20 dB.");
      }
    }

    riftDrawThumb(display, LIST_TOP - 2, ROWS * RIFT_LINE_H, _scroll, n, shown);

    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(2, 214, "ENTER: waterfall  N: name  S: source  W: watched");

    renderNavBar(display, RIFT_NAV_RADAR);
    return 700;   // coarse: the TFT shares its SPI bus with the LoRa radio
  }

  // The proximity lamp: a state, not an event.
  //
  // The alert on arrival is a moment and you have to be looking at it. What a person
  // actually wants to know is "is it here now", which is a question the screen can
  // answer continuously. Returns true if it drew, so the caller knows the heading
  // row is taken.
  //
  // Deliberately only on RADAR. The radios are torn down when this screen is left,
  // so a lamp on MESH or COMMS would be showing a reading nobody is taking - and a
  // stale indicator is worse than none, because it is believed. If background
  // scanning is ever added, the nav bar is where this belongs.
  bool renderWatchLamp(DisplayDriver& display) {
    if (rf_watch_count == 0) return false;      // no chrome for an unused feature
    // Not scanning: the lamp cannot know, so it does not claim. IDLE and
    // INITIALISING own the row in that case and say something truer.
    if (_state == OFF || (!_wifi_up && !_ble_up)) return false;

    const char* who = NULL;
    int near = rfWatchPresent(&who);

    char label[40];
    if (near == 0) {
      snprintf(label, sizeof(label), "WATCHING %d", rf_watch_count);
    } else if (near == 1) {
      snprintf(label, sizeof(label), "NEAR  %s", who ? who : "device");
    } else {
      snprintf(label, sizeof(label), "NEAR  %s  +%d", who ? who : "device", near - 1);
    }

    // Filled when present, outlined when armed and waiting. Fill against outline
    // rather than bright against dim, because the grey step between two levels
    // disappears in sunlight and the difference between these two states is the
    // whole point of the lamp. Same idiom the band cells and the NODES markers use.
    if (near > 0) {
      display.setColor(rift_pal.accent);
      display.fillRect(0, 2, display.width(), 14);
      // Ink from the palette, not white. This comment used to argue that a 3.5:1
      // accent makes a legal fill and an illegal text colour - true - and then
      // reversed white out of the fill, which is the same measurement read in the
      // other direction, on the one row the screen is shouting about.
      display.setColor(rift_pal.on_accent);
      display.drawTextLeftAlign(4, 6, label);
      // the block sits at the right so the name can be read left to right without
      // the eye stepping over a marker first
      display.fillRect(display.width() - 14, 5, 8, 8);
      display.setColor(rift_pal.accent);
      display.fillRect(display.width() - 12, 7, 4, 4);
    } else {
      display.setColor(rift_pal.rule);
      display.drawRect(0, 2, display.width(), 14);
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(4, 6, label);
      display.drawRect(display.width() - 14, 5, 8, 8);
    }
    return true;
  }

  // The watch list, as a list.
  //
  // Marking was reachable and unmarking was not. A watch could only be toggled from
  // the scan list, and the scan list holds what is in range now: rfAgeOut drops
  // anything not heard recently, watched or not, and switching source drops a whole
  // radio at once. So a device marked and then carried out of the room - the case a
  // proximity watch exists for - vanished from the only place it could be unmarked,
  // permanently. The watch list was state the user had no way to see.
  int renderWatches(DisplayDriver& display) {
    if (!renderWatchLamp(display)) renderHeading(display, "WATCHES");
    display.setTextSize(1);

    if (rf_watch_count == 0) {
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(2, 40, "No watched devices.");
      display.drawTextLeftAlign(2, 52, "N on a RADAR row names one, which watches it.");
      display.drawTextLeftAlign(2, 214, "W: back");
      renderNavBar(display, RIFT_NAV_RADAR);
      return 1000;
    }

    if (_watch_sel >= rf_watch_count) _watch_sel = rf_watch_count - 1;
    if (_watch_sel < 0) _watch_sel = 0;

    // Six two-line rows fit between the heading and the footer. The list holds
    // twelve, and rows seven to twelve used to draw over the footer and the nav
    // bar with the cursor able to reach them unseen. A window that follows the
    // cursor, and the thumb says where in the list it is.
    const int WROWS = 6;
    if (_watch_sel < _watch_first) _watch_first = _watch_sel;
    if (_watch_sel >= _watch_first + WROWS) _watch_first = _watch_sel - WROWS + 1;
    if (_watch_first < 0) _watch_first = 0;
    riftDrawThumb(display, 28, WROWS * RIFT_LINE_H * 2, _watch_first, rf_watch_count, WROWS);

    int y = 30;
    for (int i = _watch_first; i < rf_watch_count && i < _watch_first + WROWS; i++, y += RIFT_LINE_H * 2) {
      bool sel = (i == _watch_sel);
      if (sel) {
        display.setColor(rift_pal.accent);
        display.fillRect(0, y - 2, display.width(), 13);
      }
      uint16_t ink = sel ? rift_pal.on_accent : rift_pal.fg;

      // present is a fill, absent an outline - the shape-not-brightness rule the
      // lamp uses, so the two say the same thing the same way
      display.setColor(ink);
      if (rf_watch[i].present) display.fillRect(4, y + 1, 6, 6);
      else                     display.drawRect(4, y + 1, 6, 6);

      char shown[28];
      riftTranslateUTF8(shown, rf_watch[i].name, sizeof(shown));
      display.setColor(ink);
      display.drawTextEllipsized(16, y, 190, shown);

      char tag[16];
      bool off = rf_watch[i].is_wifi ? !riftScanWifi() : !riftScanBle();
      if (off)                      StrHelper::strncpy(tag, "radio off", sizeof(tag));
      else if (rf_watch[i].present) StrHelper::strncpy(tag, "near", sizeof(tag));
      else                          StrHelper::strncpy(tag, "not heard", sizeof(tag));
      display.setColor(sel ? rift_pal.on_accent : rift_pal.mid);
      display.drawTextRightAlign(314, y, tag);

      // The address, because a BLE name is often absent or shared and the key is what
      // is actually being matched. It is also the only way to tell two "AirPods"
      // apart in this list.
      const uint8_t* k = rf_watch[i].key;
      char addr[44];
      snprintf(addr, sizeof(addr), "%s  %02X:%02X:%02X:%02X:%02X:%02X",
               rf_watch[i].is_wifi ? "wifi" : "ble ",
               k[0], k[1], k[2], k[3], k[4], k[5]);
      display.setColor(rift_pal.dim);
      display.drawTextLeftAlign(16, y + RIFT_LINE_H, addr);
    }

    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(2, 214, "ENTER: forget  N: rename  W: back");
    renderNavBar(display, RIFT_NAV_RADAR);
    return 1000;
  }


  // Both lists here move a cursor, so a finger does the same thing the trackball
  // does - and the waterfall has nothing to scroll, so it is left alone rather
  // than given a gesture that does nothing.
  bool handleDrag(int dy) override {
    int steps = riftDragSteps(&_drag_residual, dy, RIFT_DRAG_PITCH);
    if (steps == 0) return false;

    if (_view == VIEW_WATCHES) {
      if (rf_watch_count == 0) return false;
      int n = _watch_sel + steps;
      if (n < 0) n = 0;
      if (n >= rf_watch_count) n = rf_watch_count - 1;
      if (n == _watch_sel) return false;
      _watch_sel = n;
      return true;
    }
    if (_view == VIEW_BANDS) {
      if (_last_n <= 0) return false;
      int n = _sel_idx + steps;
      if (n < 0) n = 0;
      if (n >= _last_n) n = _last_n - 1;
      if (n == _sel_idx) return false;
      _sel_idx = n;
      _resel = true;    // the window follows the cursor in render()
      return true;
    }
    return false;
  }

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) { _task->cycleNavScreen(1); return true; }
    if (c == KEY_PREV || c == KEY_LEFT) { _task->cycleNavScreen(-1); return true; }
    if (_view == VIEW_WATCHES) {
      if (c == 'w' || c == 'W' || c == RIFT_KEY_BACK) { _view = VIEW_BANDS; return true; }
      if (rf_watch_count == 0) return true;
      if (c == KEY_UP)   { if (_watch_sel > 0) _watch_sel--; return true; }
      if (c == KEY_DOWN) { if (_watch_sel + 1 < rf_watch_count) _watch_sel++; return true; }
      if (c == 'n' || c == 'N') { _task->openRenameWatch(_watch_sel); return true; }
      if (c == KEY_ENTER) {
        char gone[24];
        StrHelper::strncpy(gone, rf_watch[_watch_sel].name, sizeof(gone));
        // removed by key rather than by index: the toggle is the one place that knows
        // how to take an entry out, and it matches on identity
        rfWatchToggle(rf_watch[_watch_sel].key, rf_watch[_watch_sel].is_wifi, gone);
        riftSaveSettings();
        riftLogf("watch - %s", gone);
        _task->showAlert("Watch removed", 1400);
        if (_watch_sel >= rf_watch_count) _watch_sel = rf_watch_count - 1;
        if (_watch_sel < 0) _watch_sel = 0;
        return true;
      }
      return true;
    }


    // ENTER acts on the selected row, which is what it does in every other list in
    // this firmware. The waterfall moves to W - it is a view swap, not a row action,
    // and having ENTER mean two different things depending on which view you were in
    // is how a keypress ends up doing the wrong thing.
    // Cycles the source. Wi-Fi is genuinely powered down when it is not wanted;
    // BLE is only stopped, because BLEDevice::deinit() panics in this ESP32 core
    // once a scan has been active - see finishTeardown(), where the same limit
    // applies. Stopped is enough: nothing is collected and nothing is transmitted.
    if (c == 's' || c == 'S') {
      rift_radar_src = (rift_radar_src == RIFT_SRC_BOTH) ? RIFT_SRC_WIFI
                     : (rift_radar_src == RIFT_SRC_WIFI) ? RIFT_SRC_BLE
                                                         : RIFT_SRC_BOTH;
      riftSaveSettings();

      if (!riftScanWifi() && _wifi_up) {
        WiFi.scanDelete();
        WiFi.mode(WIFI_OFF);
        _wifi_up = false;
      }
      // Only stopping is needed to re-enable: beginBle() sets the callbacks, the
      // passive flag and the window on every call, so the next cycle restores all of
      // it. stop() does not clear the callbacks - only beginTeardown() does that.
      if (!riftScanBle() && _ble_up) BLEDevice::getScan()->stop();

      // Drop what the disabled radio had found. Leaving it would show devices that
      // are no longer being looked for, ageing out slowly over the next minutes,
      // and a watch could match one of them.
      rfDropSource(!riftScanWifi(), !riftScanBle());

      // And forget that those devices were present. rfWatchCheck skips a watch whose
      // radio is off, so the flag would freeze at true and the lamp would keep
      // claiming "NEAR" for a device nobody is listening for - the same stale reading
      // the teardown path clears, missed here because the two features were built
      // hours apart.
      for (int i = 0; i < rf_watch_count; i++) {
        if (rf_watch[i].is_wifi ? !riftScanWifi() : !riftScanBle()) {
          rf_watch[i].present = false;
        }
      }

      // restart the cycle at a phase that is enabled
      if (_state != OFF && _state != STOPPING) {
        _state = START_WIFI;
        _wait_ms = 0;
      }
      const char* label = (rift_radar_src == RIFT_SRC_BOTH) ? "WIFI + BLE"
                        : (rift_radar_src == RIFT_SRC_WIFI) ? "WIFI only" : "BLE only";
      _task->showAlert(label, 1400);
      riftLogf("radar source: %s", label);
      return true;
    }
    // Keys after the September design round, and the footer names them:
    //   ENTER  waterfall on and off - a view swap, so it is the same key both ways
    //   N      name the selected device, which also watches it
    //   S      source
    //   W      the watched list and back
    // ENTER used to toggle the watch and W used to walk three views in a ring;
    // naming a device meant watching it first and then finding it in a second
    // list. Now the row you are looking at is the one you name.
    if (c == 'w' || c == 'W') {
      _view = (_view == VIEW_WATCHES) ? VIEW_BANDS : VIEW_WATCHES;
      return true;
    }
    if (c == KEY_ENTER) {
      _view = (_view == VIEW_WATERFALL) ? VIEW_BANDS : VIEW_WATERFALL;
      return true;
    }
    if ((c == 'n' || c == 'N') && _view == VIEW_BANDS) {
      if (!_have_sel) {
        _task->showAlert("Nothing selected", 1200);
        return true;
      }
      int at = rfWatchFind(_sel_key, _sel_is_wifi);
      if (at < 0) {
        char msg[40];
        switch (rfWatchToggle(_sel_key, _sel_is_wifi, _sel_name)) {
          case RF_WATCH_ADDED:
            riftSaveSettings();
            riftLogf("watch + %s %s", _sel_is_wifi ? "wifi" : "ble", _sel_name);
            at = rfWatchFind(_sel_key, _sel_is_wifi);
            break;
          case RF_WATCH_FULL:
            // nothing changed, so nothing is saved either
            snprintf(msg, sizeof(msg), "Watch list full (%d)", RIFT_WATCH_MAX);
            _task->showAlert(msg, 1800);
            return true;
          default:
            break;
        }
      }
      if (at >= 0) _task->openRenameWatch(at);
      return true;
    }
    // waterfall is a level down from the band view
    if (c == RIFT_KEY_BACK && _view == VIEW_WATERFALL) {
      _view = VIEW_BANDS;
      return true;
    }
    if (_view == VIEW_BANDS) {
      // moves the cursor, not the window - the window follows it in render()
      if (c == KEY_UP)   { if (_sel_idx > 0) { _sel_idx--; _resel = true; } return true; }
      if (c == KEY_DOWN) { if (_sel_idx + 1 < _last_n) { _sel_idx++; _resel = true; } return true; }
    }
    return false;
  }
};

#endif   // RIFT_RADAR

// How many messages the popup lists. Two rows each at RIFT_LINE_H in the 154px
// between the header and the hint row is six, with 10px to spare.
#define RIFT_PREVIEW_ROWS  6

class RiftMsgPreviewScreen : public RiftScreen {
  UITask* _task;

public:
  RiftMsgPreviewScreen(UITask* task) : _task(task) { }

  // A popup over whatever the user is doing, not somewhere they navigated to.
  // This is what keeps RADAR's teardown and SYSTEM's secret wipe from firing
  // when a message arrives; both used to, and both were bugs.
  bool isOverlay() const override { return true; }

  // Drawn over the screen the user is on, after its own render() and inside the
  // same frame - the mechanism showAlert() already used. It is a panel rather
  // than a full-screen repaint so the title bar and nav bar behind it stay
  // readable: battery, and which screen you will be back on when you dismiss it.
  //
  // Not a saving in SPI traffic - endFrame() ships the whole canvas in one burst
  // either way - but it does keep the user's context on screen.
  //
  // A list rather than one message with ENTER to step through them, which is what
  // this was. Paging through an arrival one press at a time told you less than
  // seeing six at once, and cost N presses to clear N messages. Rows follow the
  // COMMS idiom exactly - time in dim, sender in mid, body in fg beneath - so
  // this reads as a shorter COMMS rather than a different view. It is triage;
  // COMMS is the record.
  int render(DisplayDriver& display) override {
    const int x = 8, w = display.width() - 16;
    const int y = 24, h = display.height() - 24 - 24;
    const int inner = w - 12;

    display.setColor(rift_pal.bg);
    display.fillRect(x, y, w, h);
    // A 2px frame in rule (September design round): a box says "on top" by being
    // a box, where the accent said "act on all of this". Two nested rects, since
    // the driver has no line width.
    display.setColor(rift_pal.rule);
    display.drawRect(x, y, w, h);
    display.drawRect(x + 1, y + 1, w - 2, h - 2);

    display.setTextSize(1);
    display.setColor(rift_pal.accent);
    display.drawTextLeftAlign(x + 6, y + 6, "MESSAGES");

    char tmp[40];
    // From the model rather than from a count of its own. This panel presents the
    // newest few entries of the message log; how much is unread is not a fact about
    // the panel.
    sprintf(tmp, "Unread: %d", (int) msg_unread.total());
    display.setColor(rift_pal.mid);
    display.drawTextRightAlign(x + w - 6, y + 6, tmp);

    int row_y = y + 22;
    int shown = 0;
    for (int back = 0; back < RIFT_PREVIEW_ROWS; back++) {
      auto p = msg_log.peek(back);
      if (p == NULL) break;

      char filtered_origin[sizeof(p->origin)];
      riftTranslateUTF8(filtered_origin, p->origin, sizeof(filtered_origin));
      char filtered_msg[sizeof(p->msg)];
      riftTranslateUTF8(filtered_msg, p->msg, sizeof(filtered_msg));

      // same sender line as COMMS: time, then the origin, ellipsized rather than
      // hard-cut so a long name reads as truncated instead of as a shorter name
      char tbuf[8];
      sprintf(tbuf, "%02d:%02d", (int) ((p->timestamp / 3600) % 24),
                                 (int) ((p->timestamp / 60) % 60));
      display.setColor(rift_pal.dim);
      display.drawTextLeftAlign(x + 6, row_y, tbuf);
      display.setColor(rift_pal.mid);
      display.drawTextEllipsized(x + 42, row_y, inner - 36, filtered_origin);

      display.setColor(rift_pal.fg);
      display.drawTextEllipsized(x + 6, row_y + RIFT_LINE_H, inner, filtered_msg);

      row_y += RIFT_LINE_H * 2;
      shown++;
    }

    if (shown == 0) {
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(x + 6, y + 22, "no messages");
    }

    // The hint row doubles as the overflow notice. Anything past the six newest
    // is in COMMS, which is one key away - there is deliberately no scrolling
    // here, because scrolling is what COMMS is for.
    display.setColor(rift_pal.dim);
    if (msg_log.count > shown) {
      sprintf(tmp, "+%d more - ENTER opens COMMS", msg_log.count - shown);
    } else {
      strcpy(tmp, "ENTER opens COMMS   BKSP dismiss");
    }
    display.drawTextLeftAlign(x + 6, y + h - 14, tmp);

    return 1000;
  }

  bool handleInput(char c) override {
    // One press ends it either way. Enter used to advance one message and take N
    // presses to clear N of them; with trackball click mapped to Enter that was
    // also easy to overshoot. Now Enter is the way through to the full history and
    // backspace is the way out.
    if (c == KEY_NEXT || c == KEY_RIGHT || c == KEY_ENTER) {
      clearUnread();
      _task->gotoCommsScreen();
      return true;
    }
    // the preview is an overlay on whatever you were doing - back dismisses it
    // and hands that screen back untouched
    if (c == RIFT_KEY_BACK) {
      clearUnread();
      _task->dismissOverlay();
      return true;
    }
    return false;
  }

private:
  // Dismissing no longer clears the unread state, and that is the behaviour change.
  //
  // It used to zero a global counter, which meant glancing at a popup marked every
  // conversation read - including ones whose messages had scrolled past the six rows
  // this panel shows. The old comment accepted that deliberately, on the grounds that
  // leaving the dot lit would nag. What actually removes the nag is that the dot now
  // clears per conversation as each one is opened, which is a defined action rather
  // than a side effect of dismissing a popup.
  //
  // What dismissal does still acknowledge is anything that arrived without a
  // conversation to open, because nothing else can.
  void clearUnread() {
    msg_unread.onPreviewDismissed();
  }
};

// COMMS: MeshCore text terminal - scrollable history plus a compose line.
// Sends either to the Public channel (flooded, no ACK possible) or direct to a
// chosen contact (ACKed, so delivery state is shown). ESC on an empty line
// opens the target picker.
// Channels can occupy MAX_GROUP_CHANNELS slots and contacts up to MAX_CONTACTS -
// 358 on this board - so this list cannot hold everything, and the question is what
// gets cut rather than whether anything does.
//
// Raised from 48, which was reported full on a device with about 64 contacts. 48 was
// chosen when this was a send-target picker, where being cut off meant "scroll to
// find someone else". It is now the conversation list, where being cut off means a
// conversation is unreachable and its unread dot invisible - so the number needed
// headroom and, more importantly, the order needed fixing. See openPicker().
//
// 48 bytes an entry, so 96 costs 4.6KB against 2.3KB - on the heap, not in the
// build's static figure, because RiftCommsScreen is allocated with new. Which means
// the reported RAM percentage does not move when this changes, and FREE HEAP on
// SYSTEM is where the cost actually shows.
#define RIFT_PICKER_MAX 96

class RiftCommsScreen : public RiftScreen, ContactVisitor {
  UITask* _task;
  char _input[MAX_TEXT_LEN + 1];
  int _len;
  // Pixels of older content scrolled into view, not messages skipped. Counting
  // messages meant one whole block moved per step whatever its height, which is
  // the jerk: a one-line reply and a six-line one moved the view by wildly
  // different amounts for the same gesture.
  int _scroll;      // 0 = pinned to newest

  // last printable key and when, for double-tap detection
  char _last_key = 0;
  unsigned long _last_key_ms = 0;

  // current send target: a group channel by index, or a contact by pubkey prefix
  bool _target_is_channel;
  uint8_t _target_channel_idx;
  uint8_t _target_key[6];

public:
  // Used by NODES, whose detail bar offers ENTER: DM. Takes the 6-byte key
  // prefix; the contact has to exist in MeshCore's book for a send to work, so
  // the caller checks that first rather than leaving the user typing into a
  // message that can never leave.
  void setDirectTarget(const uint8_t* key6) {
    _target_is_channel = false;
    memcpy(_target_key, key6, 6);
    _picking = false;
    _scroll = 0;   // a different conversation: land on its newest, not at some offset

    // The name was never set here, only by the conversation list - so arriving from
    // NODES showed the heading of whoever was picked last, or a blank one on a fresh
    // boot. It read as a bug in NODES rather than in this line. The filter is what
    // turned it from cosmetic into load-bearing: a version 1 entry with no key is
    // placed by matching this name.
    ContactInfo* c = the_mesh.lookupContactByPubKey(_target_key, 6);
    if (c != NULL) StrHelper::strncpy(_target_name, c->name, sizeof(_target_name));
    else _target_name[0] = 0;
  }

  // This used to be unconditional, and the reasoning was sound at the time: the
  // history walked the whole log without filtering, so an arriving message was
  // already on screen and a popup would only cover the view that was showing it.
  // That comment ended by naming the change that would invalidate it - filtering the
  // history per conversation - and that change has now been made. An idle COMMS
  // screen showing Public can now miss a direct message entirely.
  //
  // So the two cases have come apart, and each keeps the answer that fits it.
  // Composing or picking still suppresses the popup, because it would cost a
  // half-typed line or a selection. Idle no longer does; whether the popup is worth
  // raising is a question about the message, and showsConversation() below answers
  // it.
  bool isModal() const override { return _picking || _len > 0; }

  bool showsConversation(const RiftConvKey& k) const override {
    return riftConvSame(currentConv(), k);
  }
private:
  char _target_name[32];

  // target picker sub-view. Channels and contacts share one list; each entry
  // carries what it needs to become the send target.
  bool _picking;
  int _pick_idx;
  int _pick_scroll;   // index of the first visible row
  int _pick_drag_residual = 0;   // finger travel not yet worth a row
  struct PickEntry {
    char name[32];
    bool is_channel;
    uint8_t channel_idx;   // channels only
    uint8_t key[6];        // contacts only
    uint32_t last_ts;      // newest message in this conversation, 0 if none
    uint8_t unread;
    // A room server behaves nothing like a person - it holds a shared transcript
    // and it is always listening - and the two were drawn identically. Only CHAT
    // and ROOM reach this list at all, because riftCanDirectMessage refuses the
    // rest, so this is the one distinction the column has to carry.
    bool is_room;
  };
  PickEntry _picks[RIFT_PICKER_MAX];
  int _pick_count;
  bool _pick_truncated;

  // Where each visible row actually landed, and which entry it holds.
  //
  // Hit-tested against recorded positions rather than a fixed pitch, because the
  // two section headings take a line each and move every row below them - a
  // computed row number would select the wrong conversation, and on this screen
  // that means opening someone else's. The same mistake has been made twice in
  // this file before, on NODES and on SYSTEM.
  //
  // Visible rows only. Ninety-six entries fit the list but only about fourteen
  // fit the screen, and a table for all of them would be 768 bytes to answer a
  // question about fourteen.
  static const int PICK_ROWS_MAX = 24;
  int _pick_row_y[PICK_ROWS_MAX];
  int _pick_row_of[PICK_ROWS_MAX];
  int _pick_rows_drawn = 0;

  static const int BODY_TOP = 12;      // picker list starts here
  static const int BODY_BOTTOM = 194;
  static const int INPUT_Y = 204;
  static const int TAB_VISIBLE = 4;    // channel tabs shown at once

  // The strip and the history top move by whether a heading is drawn, which is
  // only when the target is a contact - the strip cannot name one. With a channel
  // target there is no heading, the strip sits at the very top and the history
  // gets the 16px the old title bar held, which is one more message.
  //
  // Nothing visibly jumps: the history is drawn bottom-up from BODY_BOTTOM, so a
  // varying top changes how many messages fit rather than where they sit.
  static const int TABS_Y_BARE    = 6;
  static const int TABS_Y_HEADED  = 18;
  int _tabs_y;      // recorded at render, like _tab_w, so taps hit the same rows
  int _hist_top;
  int _tab_visible; // 3 in DM mode, so the DM marker has a column of its own

  // Configured channels, cached for the strip. Rebuilt on a timer rather than
  // every frame: render runs on the same SPI bus as the LoRa radio.
  struct ChanTab { char name[20]; uint8_t idx; };
  // Sized from the configured channel limit rather than a round number: at 12,
  // channels 13 and up existed, could be picked from the target picker, and
  // received messages, but never appeared in the strip. 21 bytes each.
  ChanTab _tabs[MAX_GROUP_CHANNELS];
  int _tab_count;
  int _tab_scroll;
  int _tab_w;                 // recorded at render, so taps hit the same columns
  unsigned long _last_tabs_refresh;   // see _last_refresh above re: millis() wrap
  bool _tabs_refreshed_once;

  void refreshTabs() {
    _tab_count = 0;
    for (int i = 0; i < MAX_GROUP_CHANNELS && _tab_count < (int) (sizeof(_tabs) / sizeof(_tabs[0])); i++) {
      ChannelDetails ch;
      if (!the_mesh.getChannel(i, ch) || ch.name[0] == 0) continue;
      ChanTab* t = &_tabs[_tab_count++];
      StrHelper::strncpy(t->name, ch.name, sizeof(t->name));
      t->idx = (uint8_t) i;
    }
    // keep the active channel in view
    int active = -1;
    for (int i = 0; i < _tab_count; i++) {
      if (_target_is_channel && _tabs[i].idx == _target_channel_idx) { active = i; break; }
    }
    if (active >= 0) {
      if (active < _tab_scroll) _tab_scroll = active;
      if (active >= _tab_scroll + TAB_VISIBLE) _tab_scroll = active - TAB_VISIBLE + 1;
    }
    if (_tab_scroll > _tab_count - TAB_VISIBLE) _tab_scroll = _tab_count - TAB_VISIBLE;
    if (_tab_scroll < 0) _tab_scroll = 0;
  }

  int tabWidth(DisplayDriver& display) const { return display.width() / TAB_VISIBLE; }

  void renderTabs(DisplayDriver& display) {
    if (!_tabs_refreshed_once || millis() - _last_tabs_refresh >= 2000) {
      refreshTabs();
      _last_tabs_refresh = millis();
      _tabs_refreshed_once = true;
    }

    int tw = _tab_w = tabWidth(display);
    display.setTextSize(1);

    // One fewer channel tab in DM mode, so the DM marker below gets the fourth
    // column instead of being drawn on top of it. The tab width is unchanged, so
    // the columns and their tap targets stay where they were.
    _tab_visible = _target_is_channel ? TAB_VISIBLE : TAB_VISIBLE - 1;

    for (int i = _tab_scroll; i < _tab_count && i < _tab_scroll + _tab_visible; i++) {
      int col = i - _tab_scroll;
      int x = col * tw;
      bool active = _target_is_channel && _tabs[i].idx == _target_channel_idx;

      if (active) {
        // accent fill with the label in on_accent - the active channel has to
        // survive being read in sunlight, where a fill one shade off the
        // background does not. The ink is 6.01:1; white here was 3.5:1.
        display.setColor(rift_pal.accent);
        display.fillRect(x, _tabs_y - 2, tw - 2, 13);
        display.setColor(rift_pal.on_accent);
      } else {
        // The border carries the channel colour; the label stays mid. The active
        // tab keeps its accent fill untouched - being the selected channel is the
        // stronger thing to say, and a colour bar at the same lightness as the
        // accent would only muddy it. So the channel you are on is identified by
        // name in the fill, and the ones you are not by colour in the outline.
        uint16_t col = riftChannelColour(_tabs[i].idx);
        display.setColor(col != RIFT_CHAN_COL_NONE ? col : rift_pal.rule);
        display.drawRect(x, _tabs_y - 2, tw - 2, 13);
        display.setColor(rift_pal.mid);
      }

      char filtered[sizeof(_tabs[i].name)];
      riftTranslateUTF8(filtered, _tabs[i].name, sizeof(filtered));
      display.drawTextEllipsized(x + 3, _tabs_y, tw - 8, filtered);

      // Unread, on the tabs you are not looking at. The active tab is excluded
      // because the accent dot would be invisible inside its accent fill - and
      // because it is the conversation on screen, whose unread render() has just
      // cleared. A channel scrolled out of the strip has no dot here; the nav bar
      // still says something is unread somewhere.
      if (!active && msg_unread.count(riftChannelConv(_tabs[i].idx)) > 0) {
        renderUnreadDot(display, x + tw - 8, _tabs_y - 1);
      }
    }

    // a contact target is not in the strip, so say so rather than showing no
    // selection at all
    if (!_target_is_channel) {
      display.setColor(rift_pal.accent);
      display.drawTextRightAlign(display.width() - 2, _tabs_y, "DM");
    }

    display.setColor(rift_pal.rule);
    display.fillRect(0, _tabs_y + 13, display.width(), 1);
  }

  // What actually fits in a channel message. MeshCore puts "<sender>: " in front
  // of the text and truncates the whole thing at MAX_TEXT_LEN, so the usable
  // length depends on how long this node's name is - a 20-character name costs
  // 22 characters of message.
  int channelCapacity() const {
    if (!_target_is_channel) return MAX_TEXT_LEN;
    return riftChannelCapacity(MAX_TEXT_LEN, the_mesh.getNodeName());
  }

  bool getTargetChannel(ChannelDetails& ch) {
    return the_mesh.getChannel(_target_channel_idx, ch) && ch.name[0] != 0;
  }

public:
  // The conversation on screen. Public because the popup gate asks the screen
  // whether an arriving message is already visible.
  RiftConvKey currentConv() const {
    if (_target_is_channel) return riftChannelConv(_target_channel_idx);
    return riftConvDM(_target_key);
  }
private:

  // The current target's name, for placing entries that have no conversation key -
  // see inCurrentConv(). Channels are named by the mesh rather than by _target_name,
  // which is only filled in when the target came from the conversation list.
  const char* currentTargetName(char* buf, size_t buf_len) const {
    if (!_target_is_channel) return _target_name;
    ChannelDetails ch;
    if (!the_mesh.getChannel(_target_channel_idx, ch) || ch.name[0] == 0) return "";
    StrHelper::strncpy(buf, ch.name, buf_len);
    return buf;
  }

  // Whether a history entry belongs in the view.
  //
  // Entries written by this build carry the conversation, so the test is exact.
  // Entries restored from a version 1 file do not, and are placed by matching the
  // origin name - which is what the whole screen did before this change, so it is
  // not a regression, and dropping them instead would make a history restored from
  // an older build vanish the moment this shipped. They fade out as the log turns
  // over, and nothing new ever takes that path.
  // How many stored messages this conversation has. Walked rather than counted
  // incrementally: it is asked on a keypress, not per frame, and a cached count would
  // be one more thing that can disagree with the log.
  int convCount() const {
    int n = 0;
    for (int back = 0; back < msg_log.count; back++) {
      const RiftMsgLog::Entry* p = msg_log.peek(back);
      if (p == NULL) break;
      if (inCurrentConv(p)) n++;
    }
    return n;
  }

  bool inCurrentConv(const RiftMsgLog::Entry* p) const {
    if (p->conv.kind != RIFT_CONV_UNKNOWN) return riftConvSame(p->conv, currentConv());

    char name[64], target[32];
    if (!riftOriginName(p->origin, name, sizeof(name))) return false;
    const char* want = currentTargetName(target, sizeof(target));
    return want[0] != 0 && strcmp(name, want) == 0;
  }

  // from ContactVisitor - called by scanRecentContacts(), already ordered by
  // last_advert_timestamp descending (most recently heard first)
  // Is this key already in the list? The history pass in openPicker() adds
  // conversations before the mesh sweep does, so the sweep has to skip what is
  // already there or every conversation would appear twice.
  bool alreadyPicked(const uint8_t* key6) const {
    for (int i = 0; i < _pick_count; i++) {
      if (_picks[i].is_channel) continue;
      if (memcmp(_picks[i].key, key6, 6) == 0) return true;
    }
    return false;
  }

  void onContactVisit(const ContactInfo& contact) override {
    if (contact.name[0] == 0) return;   // skip the reserved anon slots

    // only nodes that can actually receive a text message - repeaters and
    // sensors have no one reading them
    if (!riftCanDirectMessage(contact.type)) return;

    if (alreadyPicked(contact.id.pub_key)) return;   // the history pass had it

    // the list is finite; say so instead of dropping contacts silently. Only
    // reachable contacts count, so the warning does not fire for repeaters.
    if (_pick_count >= RIFT_PICKER_MAX) { _pick_truncated = true; return; }

    PickEntry* e = &_picks[_pick_count++];
    StrHelper::strncpy(e->name, contact.name, sizeof(e->name));
    e->is_channel = false;
    e->channel_idx = 0;
    memcpy(e->key, contact.id.pub_key, 6);
  }

  void openPicker() {
    _pick_count = 0;
    _pick_truncated = false;
    // Left over travel from the last drag would move the cursor the instant this
    // opened, which reads as the list picking a row on its own.
    _pick_drag_residual = 0;
    _pick_rows_drawn = 0;   // nothing has been laid out yet, so nothing is hittable

    // configured channels first. There is no getNumChannels() - num_channels is
    // protected and only tracks addChannel() - so probe the slots and skip the
    // ones with no name.
    for (int i = 0; i < MAX_GROUP_CHANNELS && _pick_count < RIFT_PICKER_MAX; i++) {
      ChannelDetails ch;
      if (!the_mesh.getChannel(i, ch) || ch.name[0] == 0) continue;

      PickEntry* e = &_picks[_pick_count++];
      StrHelper::strncpy(e->name, ch.name, sizeof(e->name));
      e->is_channel = true;
      e->channel_idx = (uint8_t) i;
      memset(e->key, 0, sizeof(e->key));
    }
    int first_dm = _pick_count;

    // Conversations you actually have, before candidates you merely could have.
    //
    // This is the fix for a real defect, not a refinement. The only source of DM rows
    // used to be scanRecentContacts(), which orders by when the mesh last heard a
    // node - and that is the wrong axis for a conversation list. Someone you messaged
    // an hour ago whose node has since gone quiet sorts to the bottom, and on a device
    // with more contacts than this list holds it was cut off entirely: the
    // conversation unreachable, its unread dot invisible, and the only clue a line
    // saying the list was full. Being heard recently is not the same as being someone
    // you are talking to.
    //
    // Bounded by the log rather than by the contact table: at most RIFT_MSG_LOG_SIZE
    // entries exist, so at most that many distinct conversations can have history, and
    // in practice far fewer. Walked newest-first, so if it ever did have to stop, it
    // would stop at the least recently active conversation.
    for (int back = 0; back < msg_log.count && _pick_count < RIFT_PICKER_MAX; back++) {
      const RiftMsgLog::Entry* p = msg_log.peek(back);
      if (p == NULL) break;
      if (p->conv.kind != RIFT_CONV_DM) continue;
      if (alreadyPicked(p->conv.peer)) continue;

      // The contact has to still exist: a send needs MeshCore's book, and history
      // outlives the contact it belongs to. A conversation whose contact is gone is
      // still readable in the log, it just cannot be a row you can send from.
      ContactInfo* c = the_mesh.lookupContactByPubKey((uint8_t*) p->conv.peer,
                                                      RIFT_CONV_PEER_LEN);
      if (c == NULL || !riftCanDirectMessage(c->type)) continue;

      PickEntry* e = &_picks[_pick_count++];
      StrHelper::strncpy(e->name, c->name, sizeof(e->name));
      e->is_channel = false;
      e->channel_idx = 0;
      memcpy(e->key, c->id.pub_key, 6);
    }

    // then the rest, most recently heard first (scanRecentContacts sorts them)
    the_mesh.scanRecentContacts(0, this);

    // Every row now carries what the list needs to say about it. Both are looked up
    // here rather than while drawing: the log walk is 48 entries per row and render()
    // shares the SPI bus with the radio, so it belongs on the open rather than in
    // every frame.
    for (int i = 0; i < _pick_count; i++) {
      RiftConvKey k = _picks[i].is_channel ? riftChannelConv(_picks[i].channel_idx)
                                           : riftConvDM(_picks[i].key);
      _picks[i].last_ts = newestIn(k, _picks[i].name);
      _picks[i].unread = msg_unread.count(k);
      _picks[i].is_room = false;
      if (!_picks[i].is_channel) {
        ContactInfo* c = the_mesh.lookupContactByPubKey(_picks[i].key, 6);
        if (c != NULL) _picks[i].is_room = (c->type == ADV_TYPE_ROOM);
      }
    }

    // Direct rows sort by their newest message, most recent first, with contacts you
    // have never exchanged anything with after them in the order the mesh last heard
    // them. The channel section keeps slot order: a strip you can learn the position
    // of is worth more than one ranked by traffic, and the strip uses the same order.
    //
    // Insertion sort, over at most RIFT_PICKER_MAX rows, once per open. It is stable,
    // which is what preserves the recently-heard order among the rows that tie at
    // zero - a faster sort that reordered them would lose information the mesh
    // supplied for free.
    for (int i = first_dm + 1; i < _pick_count; i++) {
      PickEntry tmp = _picks[i];
      int j = i - 1;
      while (j >= first_dm && _picks[j].last_ts < tmp.last_ts) {
        _picks[j + 1] = _picks[j];
        j--;
      }
      _picks[j + 1] = tmp;
    }

    _picking = true;
    _pick_scroll = 0;

    // Open on the conversation you are in, so the list says where you are before it
    // asks where you want to go.
    _pick_idx = 0;
    for (int i = 0; i < _pick_count; i++) {
      RiftConvKey k = _picks[i].is_channel ? riftChannelConv(_picks[i].channel_idx)
                                           : riftConvDM(_picks[i].key);
      if (riftConvSame(k, currentConv())) { _pick_idx = i; break; }
    }
    ensurePickVisible();
  }

  // Timestamp of the newest message in a conversation, or 0 if it has none.
  //
  // `name` is for the version 1 entries that carry no key: they are placed by the
  // same origin match the history uses, so a conversation restored from an older
  // file still reports a time instead of reading as empty.
  uint32_t newestIn(const RiftConvKey& k, const char* name) const {
    for (int back = 0; back < msg_log.count; back++) {
      const RiftMsgLog::Entry* p = msg_log.peek(back);
      if (p == NULL) break;
      if (p->conv.kind != RIFT_CONV_UNKNOWN) {
        if (riftConvSame(p->conv, k)) return p->timestamp;   // newest first, so done
        continue;
      }
      char origin_name[64];
      if (riftOriginName(p->origin, origin_name, sizeof(origin_name))
          && strcmp(origin_name, name) == 0) return p->timestamp;
    }
    return 0;
  }

  void sendToChannel() {
    ChannelDetails ch;
    if (!getTargetChannel(ch)) {
      _task->showAlert("No channel!", 1200);
      return;
    }

    // BaseChatMesh prepends "<name>: " and then silently truncates the packet to
    // MAX_TEXT_LEN. Truncate here too, so the history shows what was transmitted
    // rather than what was typed.
    // Cut on a code point boundary, not a byte. _len can exceed the capacity even
    // though input is bounded, because the capacity moves: it depends on this
    // node's name, and a DM allows the full MAX_TEXT_LEN where a channel does not.
    // Compose a long direct message, switch the target to a channel, and the text
    // is now over budget - a byte-wise cut there could split a two-byte character
    // and put a dangling lead byte on the air.
    int cap = channelCapacity();
    int sent_len = (int) mesh::validUtf8PrefixLength(_input, (size_t) cap);
    char sent[MAX_TEXT_LEN + 1];
    memcpy(sent, _input, sent_len);
    sent[sent_len] = 0;

    bool ok = the_mesh.sendGroupMessage(the_mesh.getRTCClock()->getCurrentTime(),
                                        ch.channel, the_mesh.getNodeName(),
                                        sent, sent_len);
    if (ok) {
      // "to <channel>:", matching what sendToContact records for a DM. This used
      // to be this node's own name, which said the same thing as the accent bar
      // beside it and left the row unable to say which channel the message went
      // to - so a message you sent to a channel could not be coloured while every
      // message you received on it could.
      char origin[62];
      snprintf(origin, sizeof(origin), "to %s:", ch.name);
      msg_log.add(the_mesh.getRTCClock()->getCurrentTime(),
                  riftChannelConv((uint8_t) _target_channel_idx), origin, sent, true);
      riftLogf("tx s%d %s: %s", _target_channel_idx, ch.name, sent);
      clearInput();
    } else {
      riftLogf("TX FAILED s%d %s", _target_channel_idx, ch.name);
      _task->showAlert("Send failed", 1200);
    }
  }

  void sendToContact() {
    // must be a live pointer - MyMesh stores it in its ACK table
    ContactInfo* rcpt = the_mesh.lookupContactByPubKey(_target_key, 6);
    if (rcpt == NULL) {
      _task->showAlert("Contact lost", 1200);
      return;
    }

    uint32_t expected_ack = 0, est_timeout = 0;
    int result = the_mesh.sendTextTo(rcpt, _input, expected_ack, est_timeout);
    if (result == MSG_SEND_FAILED) {
      riftLogf("TX FAILED to %s", _target_name);
      _task->showAlert("Send failed", 1200);
      return;
    }
    // surface the routing choice - a stale stored path sends DIRECT into the
    // void, which looks identical to success until the ACK never arrives
    _task->showAlert(result == MSG_SEND_SENT_FLOOD ? "Sent (flood)" : "Sent (direct)", 1500);

    char origin[62];
    sprintf(origin, "to %s:", _target_name);
    // DMs sent were not logged at all; only channel sends were. The routing choice
    // goes in because a stale stored path sends DIRECT into the void and looks
    // exactly like success until the ack never arrives - which is the pair of lines
    // this log exists to put next to each other.
    riftLogf("tx %s %s: %s", result == MSG_SEND_SENT_FLOOD ? "flood" : "direct",
             _target_name, _input);
    auto p = msg_log.add(the_mesh.getRTCClock()->getCurrentTime(),
                         riftConvDM(rcpt->id.pub_key), origin, _input, true);
    p->expected_ack = expected_ack;
    p->sent_at_ms = millis();
    p->timeout_ms = est_timeout;
    clearInput();
  }

  void clearInput() {
    _input[0] = 0;
    _len = 0;
    _scroll = 0;
    // Otherwise a send followed by the same letter within the double-tap window
    // opened the picker over a buffer that no longer holds the first press. The
    // buffer check above already refuses that case; clearing here means the two
    // conditions cannot disagree, rather than one covering for the other.
    _last_key = 0;
    _last_key_ms = 0;
  }

  void send() {
    if (_len == 0) return;
    if (_target_is_channel) sendToChannel(); else sendToContact();
  }

  // delivery suffix for an outgoing direct message, or NULL if not applicable
  const char* deliveryLabel(const RiftMsgLog::Entry* p, char* buf, size_t buf_len) {
    if (!p->outgoing || p->expected_ack == 0) return NULL;   // channel send / incoming
    if (p->delivered) {
      // The check glyph (CP437 0xFB) is the form; the colour is the second cue.
      // No channel or sender name can begin with it, and it sits in the slot where
      // names never are, so "delivered" survives even where day-mode green and a
      // channel colour would be confused. The trip time is what this device knows
      // about the delivery; the hop count of the ack is not.
      snprintf(buf, buf_len, "\xFB %.1fs", p->trip_ms / 1000.0f);
      return buf;
    }
    // subtract rather than add: millis() wraps at ~49.7 days, and
    // (sent_at + timeout) would overflow and report a fresh send as timed out
    if (p->timeout_ms > 0 && millis() - p->sent_at_ms > p->timeout_ms) return "no ack";
    return "sending";
  }

  int pickerRows() const { return (BODY_BOTTOM - (BODY_TOP + 4)) / RIFT_LINE_H; }

  // A 3x3 accent square, not a letter and not a colour change.
  //
  // The accent already means active tab, your own message, and you can act here.
  // This is a fourth use and it only works because it is a shape: the palette rules
  // forbid encoding data in brightness where shape can carry it, and a mark either
  // occupying its three pixels or not is the least ambiguous state a 6x8 cell has
  // room for. The count is deliberately not drawn - one glyph of digits in this
  // space would be unreadable, and the question the row answers is whether to open
  // it, not how far behind you are.
  //
  // ink is what to draw it in when it lands on an accent fill. The mark is accent
  // and so is the selected row, so on that one row it was accent on accent and
  // vanished - and "there is something unread here" disappearing on the row you
  // have moved the cursor to is the wrong row to lose it on. Drawn in the fill's
  // own ink instead: the shape survives, which is the property it was chosen for.
  void renderUnreadDot(DisplayDriver& display, int x, int y, bool on_fill = false) {
    // fg since the September design round: unread is a shape, and the accent is
    // reserved for active tab, selection, warning and the wordmark.
    display.setColor(on_fill ? rift_pal.on_accent : rift_pal.fg);
    display.fillRect(x, y, 3, 3);
  }

  int renderPicker(DisplayDriver& display) {
    display.setTextSize(1);
    int total = _pick_count;

    // The heading is the count, in words, with the key on the right. "How many is
    // there" was one of the three questions this list could not answer, and the
    // total was once shown as a fraction of a cursor position, which read as one.
    {
      char cnt[32];
      snprintf(cnt, sizeof(cnt), "%d conversation%s", total, total == 1 ? "" : "s");
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(2, 2, cnt);
      display.drawTextRightAlign(314, 2, "ENTER: open");
    }
    int y = BODY_TOP + 4;
    bool drew_channels = false, drew_direct = false;
    _pick_rows_drawn = 0;

    // Bounded by the space left rather than by a row count: the two section headings
    // take a line each, so a fixed count would have run off the bottom of one list
    // and stopped short on another.
    for (int i = _pick_scroll; i < total; i++) {
      bool is_ch = _picks[i].is_channel;

      // Section heading, drawn at the first row of each kind that is actually
      // visible - so scrolling into the middle of the direct section still says
      // which section that is.
      if ((is_ch && !drew_channels) || (!is_ch && !drew_direct)) {
        if (is_ch) drew_channels = true; else drew_direct = true;
        if (y + RIFT_LINE_H * 2 > BODY_BOTTOM) break;
        display.setColor(rift_pal.mid);
        display.drawTextLeftAlign(4, y, is_ch ? "CHANNELS" : "DIRECT");
        display.setColor(rift_pal.rule);
        display.fillRect(4, y + 9, 76, 1);
        y += RIFT_LINE_H;
      }
      if (y + RIFT_LINE_H > BODY_BOTTOM) break;

      bool sel = (_pick_idx == i);
      if (_pick_rows_drawn < PICK_ROWS_MAX) {
        _pick_row_y[_pick_rows_drawn] = y;
        _pick_row_of[_pick_rows_drawn] = i;
        _pick_rows_drawn++;
      }
      // A filled bar rather than a "> " prefix, matching the scope list and the
      // NODES and RADAR rows - this was the last list still marking its selection
      // with two characters of text, which is a shape only if you are looking for
      // it and costs two cells on every row to say something about one.
      if (sel) {
        display.setColor(rift_pal.accent);
        display.fillRect(0, y - 2, 316, 12);
      }
      if (_picks[i].unread > 0) renderUnreadDot(display, 2, y + 2, sel);

      // The channel's colour, as the 2px chip the strip borders already use, the
      // full row height. Only four channels get one - see design/channel-colours.md
      // - so a fifth draws the chip in rule: it has an identity, not a colour, and
      // repeating a colour would be worse than none. Public, which every node
      // has, draws no chip at all.
      if (is_ch) {
        uint16_t cc = riftChannelColour(_picks[i].channel_idx);
        if (cc != RIFT_CHAN_COL_NONE) {
          display.setColor(cc);
          display.fillRect(7, y - 2, 2, 12);
        } else if (_picks[i].channel_idx != 0) {
          display.setColor(sel ? rift_pal.on_accent : rift_pal.rule);
          display.fillRect(7, y - 2, 2, 12);
        }
      }

      // The name in the sender's own colour, so a person is the same colour here
      // as in the conversation - which is the whole point of having derived it
      // from the name. Direct rows only: a channel row already carries the
      // channel's 2px chip, and hashing the channel's name would put a second,
      // unrelated colour on the same row, which is two colour claims about one
      // thing. Channels keep chip plus fg.
      uint16_t name_col;
      if (sel)            name_col = rift_pal.on_accent;
      else if (is_ch)     name_col = UIColor::secondary_txt;
      else {
        name_col = riftNameColour(_picks[i].name);
        if (name_col == RIFT_CHAN_COL_NONE) name_col = UIColor::secondary_txt;
      }
      display.setColor(name_col);
      char filtered[32];
      riftTranslateUTF8(filtered, _picks[i].name, sizeof(filtered));
      // x 11, where "> " used to push it to 23. The bar says which row is selected
      // on every row rather than on one, and gives two cells back to every name.
      // 240, which stops at 251. The widest right slot is "ROOM >99d" at 9 cells,
      // right-aligned at 316, so it begins at 262 in the worst case.
      display.drawTextEllipsized(11, y, 240, filtered);

      // The right slot: what kind of thing this is, and how long ago it spoke.
      //
      // Relative, not a clock. "05:47" asks the reader to know what time it is now
      // and then subtract; "3m" and "4d" do not. NODES has printed relative ages in
      // its HEARD column since the August round decided relative in lists and
      // absolute in detail rows, and this list is the one that did not get the
      // memo - so this is drift, not a new idea.
      //
      // The type sits here rather than in front of the name, because a type before
      // the name pushes every name right by four cells and makes the left edge
      // ragged between rows. Next to the age it is also the pairing that gets read
      // together: a room server last heard four days ago and a person last heard
      // four days ago mean different things.
      char right[20];
      char age[RIFT_AGE_BUF_LEN];
      uint32_t now = the_mesh.getRTCClock()->getCurrentTime();
      if (_picks[i].last_ts == 0) {
        StrHelper::strncpy(age, "-", sizeof(age));
      } else if (!riftClockPlausible(now) || now < _picks[i].last_ts) {
        // An unset clock, or history stamped after it - the age cannot be computed
        // and a wrong one would look exactly like a right one. `?` is the glyph
        // this firmware uses for that everywhere else.
        StrHelper::strncpy(age, "?", sizeof(age));
      } else {
        riftFormatAgeSecs(now - _picks[i].last_ts, age, sizeof(age));
      }
      // Type in its own column at x 258 in mid, age right-aligned at 314 in fg -
      // the September design system's row: the tail is the thumb's, and the two
      // facts get two colours because they are two facts.
      (void) right;
      if (_picks[i].is_room) {
        display.setColor(sel ? rift_pal.on_accent : rift_pal.mid);
        display.drawTextLeftAlign(258, y, "ROOM");
      }
      display.setColor(sel ? rift_pal.on_accent : rift_pal.fg);
      display.drawTextRightAlign(314, y, age);

      y += RIFT_LINE_H;
    }

    // The window pattern: where in the list this is, in rows drawn.
    riftDrawThumb(display, BODY_TOP + 2, BODY_BOTTOM - (BODY_TOP + 2),
                  _pick_scroll, total, _pick_rows_drawn);

    if (_pick_count == 0) {
      display.setColor(UIColor::secondary_txt);
      display.drawTextLeftAlign(4, y + RIFT_LINE_H, "(no channels or contacts)");
    } else if (_pick_truncated) {
      display.setColor(UIColor::warning_txt);
      // Named precisely, because the old wording - "some contacts not shown" - was
      // read as the contact table being full, which is a different and much more
      // serious thing. Every conversation with history is now listed before this line
      // can appear, so what is missing is only people you have not written to.
      display.drawTextLeftAlign(4, INPUT_Y - RIFT_LINE_H,
                                "list full - contacts with no history not shown");
    }

    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(2, INPUT_Y, "drag or trackball  tap or ENTER opens");

    renderNavBar(display, RIFT_NAV_COMMS);
    return 1000;
  }

  // Opening a conversation, from Enter or from a tap. One function because two
  // copies of this drifted apart once already in this file - the tap path would
  // have been the copy that forgot to reset _scroll, and the symptom is landing
  // mid-history in a conversation you just opened.
  void openPick(int i) {
    if (i < 0 || i >= _pick_count) return;
    PickEntry* e = &_picks[i];
    _pick_idx = i;
    _target_is_channel = e->is_channel;
    if (e->is_channel) {
      _target_channel_idx = e->channel_idx;
    } else {
      memcpy(_target_key, e->key, 6);
    }
    StrHelper::strncpy(_target_name, e->name, sizeof(_target_name));
    _picking = false;
    _scroll = 0;   // a different conversation: land on its newest
  }

  // keep the selected row inside the visible window
  void ensurePickVisible() {
    // Two fewer than the raw row count: the CHANNELS and DIRECT headings each take a
    // line, so scrolling by the unadjusted count put the selected row just off the
    // bottom. Conservative rather than exact - a list with only one section wastes a
    // row, which is cheaper than a selection you cannot see.
    int rows = pickerRows() - 2;
    if (rows < 1) rows = 1;
    if (_pick_idx < _pick_scroll) _pick_scroll = _pick_idx;
    if (_pick_idx >= _pick_scroll + rows) _pick_scroll = _pick_idx - rows + 1;
    if (_pick_scroll < 0) _pick_scroll = 0;
  }

  bool handlePickerInput(char c) {
    int total = _pick_count;
    if (total == 0) {
      // nothing to choose - don't trap the user in an empty picker
      if (c == KEY_CANCEL || c == KEY_ENTER || c == KEY_NEXT || c == KEY_PREV
          || c == RIFT_KEY_BACK) _picking = false;
      return true;
    }
    if (c == KEY_UP) {
      _pick_idx = (_pick_idx + total - 1) % total;
      ensurePickVisible();
      return true;
    }
    if (c == KEY_DOWN) {
      _pick_idx = (_pick_idx + 1) % total;
      ensurePickVisible();
      return true;
    }
    if (c == KEY_ENTER) { openPick(_pick_idx); return true; }
    // no dedicated ESC key on this keyboard - backspace and trackball click both back out
    if (c == KEY_CANCEL || c == KEY_NEXT || c == KEY_PREV || c == RIFT_KEY_BACK) {
      _picking = false;
      return true;
    }
    return true;   // swallow everything else while picking
  }

public:
  // Order follows the declarations, which is the order they are actually
  // constructed in regardless of what is written here.
  RiftCommsScreen(UITask* task)
     : _task(task), _len(0), _scroll(0),
       _target_is_channel(true), _target_channel_idx(RIFT_PUBLIC_CHANNEL_IDX),
       _picking(false), _pick_idx(0), _pick_scroll(0), _pick_count(0),
       _pick_truncated(false),
       // in declaration order: members are initialised in the order they are
       // declared regardless of how they are listed here, so a list in a
       // different order reads as a sequence the compiler will not honour
       _tabs_y(0), _hist_top(0), _tab_visible(TAB_VISIBLE),
       _tab_count(0), _tab_scroll(0), _tab_w(0),
       _last_tabs_refresh(0), _tabs_refreshed_once(false) {
    _input[0] = 0;
    _target_name[0] = 0;
    memset(_target_key, 0, sizeof(_target_key));
  }

  bool isComposing() const { return _len > 0; }

  // Whether a keystroke would reach the compose line at all. False while the
  // target picker is up: handleInput() routes everything there instead, so the
  // line is neither visible nor being typed into.
  //
  // The Nordic picker has to check this. Holding a vowel over the target picker
  // would otherwise open it, and choosing a variant would replace the last
  // character of a line the user cannot see - a character the held key never
  // inserted, because the keystroke went to the picker.
  bool acceptsText() const { return !_picking; }


  // A history entry's channel, as a colour. Channel messages carry the channel
  // name as their origin - MeshCore prepends the sender to the text itself - so
  // the tab cache, which already maps name to slot, is the whole lookup and no
  // new field is needed in the log or its file format.
  //
  // Two honest limits. A direct message from a contact whose name happens to equal
  // a channel name picks up that channel's colour; recording the slot in every
  // entry would disambiguate it, at the cost of a file format version, to fix a
  // collision the user created. And ChanTab::name is 20 bytes, so a channel named
  // longer than that is cached truncated and never matches - it gets no colour
  // rather than the wrong one.
  uint16_t originColour(const char* origin) const {
    if (origin == NULL || origin[0] == 0) return RIFT_CHAN_COL_NONE;

    // riftOriginName strips the "to <name>:" an outgoing entry carries; it is in
    // RiftLogic.h so the edge cases have tests rather than an argument
    char name[64];
    if (!riftOriginName(origin, name, sizeof(name))) return RIFT_CHAN_COL_NONE;

    for (int i = 0; i < _tab_count; i++) {
      if (strcmp(_tabs[i].name, name) == 0) return riftChannelColour(_tabs[i].idx);
    }
    return RIFT_CHAN_COL_NONE;
  }

  void onDelivered(uint32_t ack_hash, uint32_t trip_ms) {
    msg_log.markDelivered(ack_hash, trip_ms);
  }

  int render(DisplayDriver& display) override {
    if (_picking) return renderPicker(display);

    // The heading is only drawn when the strip below cannot answer the question
    // for itself. A channel target is already there with an accent fill, so
    // naming it again at the top said the same thing twice.
    //
    // A contact target needs a heading: the strip holds channels only - there can
    // be MAX_CONTACTS of the other kind - and marks a DM without naming it. This
    // line is then the only place the target appears, and at full width rather
    // than the twelve characters a strip column would allow.
    //
    // A channel target needs none. It is already in the strip under an accent
    // fill, and naming it again above said the same thing twice. That case gets
    // the strip at the top of the screen and the 16px into the history instead.
    // On screen is read. Done here rather than on navigation because switching
    // channel in the strip neither enters nor leaves a screen, so there is no
    // navigation event to hang it on - and the frame is what actually knows.
    msg_unread.clear(currentConv());

    // Decided here, drawn after the history.
    //
    // The history is laid out with a pixel offset and this driver has no
    // clipping, so a block that is partly outside the view has to be drawn and
    // then covered. Drawing the chrome first and the history over it would put
    // half a message across the tab strip; drawing the history first, masking the
    // margins and putting the chrome back on top is what makes a partly-visible
    // block possible at all - and a block that appears whole or not at all is
    // exactly the jerk that pixel scrolling was meant to remove.
    ChannelDetails ch;
    bool headed = true;
    const char* heading = NULL;
    if (!_target_is_channel) {
      heading = _target_name;
    } else if (!getTargetChannel(ch)) {
      heading = "NO CHANNEL";   // nothing in the strip is filled in this state
    } else {
      headed = false;
    }

    _tabs_y = headed ? TABS_Y_HEADED : TABS_Y_BARE;
    _hist_top = _tabs_y + 16;

    display.setTextSize(1);

    // History, newest at the bottom: lay entries out upward from the input line.
    //
    // _scroll is in pixels, and counts within this conversation rather than the log.
    // Indexing the log directly meant a scroll step through a run of other
    // conversations moved the counter without moving the view, and coming back then
    // cost one press per entry that had never been shown.
    int avail_px = display.width() - 8;
    int y = BODY_BOTTOM;

    // Eviction can take the entry the scroll position was counted against, and a
    // scroll past the end drew nothing at all - the same failure the event log and
    // the packet log both clamp for, a few hundred lines up.
    int n_conv = convCount();
    if (n_conv == 0) _scroll = 0;
    if (_scroll < 0) _scroll = 0;

    // Measure the whole conversation before laying any of it out, so the scroll
    // can be clamped to its real end.
    //
    // It used to be allowed past the end and taken back on the next frame, which
    // is exactly the spring-back: the view moved where the finger asked and then
    // moved itself somewhere else. wrapText with a null display only counts
    // lines, so this is arithmetic over the text already in RAM.
    int total_h = 0;
    for (int back = 0; back < msg_log.count; back++) {
      auto q = msg_log.peek(back);
      if (q == NULL) break;
      if (!inCurrentConv(q)) continue;
      // Measured on the body the render will actually lay out, prefix removed.
      // Measuring the whole string and drawing less made the scroll stop short.
      // Split on the raw text and translate what is left, the same order as the
      // render - see there for why the order matters.
      const char* raw_body = q->msg;
      if (!q->outgoing && q->conv.kind == RIFT_CONV_CHANNEL) {
        raw_body += senderSplit(q->msg, NULL, 0);
      }
      char m[sizeof(q->msg)];
      riftTranslateUTF8(m, raw_body, sizeof(m));
      total_h += (wrapText(m, avail_px, 0, NULL, 0) + 1) * RIFT_LINE_H;
    }
    const int view_h = BODY_BOTTOM - _hist_top;
    const int max_scroll = total_h > view_h ? total_h - view_h : 0;
    if (_scroll > max_scroll) _scroll = max_scroll;

    // Everything is laid out from the input line upward, shifted down by the
    // scroll offset. Blocks that fall outside are skipped rather than clipped:
    // this driver has no clipping, so a partly-visible block would draw over the
    // tab strip or the compose line.
    y += _scroll;
    for (int back = 0; back < msg_log.count; back++) {
      auto p = msg_log.peek(back);
      if (p == NULL) break;
      if (!inCurrentConv(p)) continue;   // a different conversation, not this one

      // The sender is split off the RAW text, before translation. The translator
      // emits 0x01/0x02 for ø/Ø and 0x03..0x1B for mapped emoji, and the split
      // refuses any byte below 32 in a name, so "Bjørn: hei" split after
      // translation found no sender at all - no colour, no name lift, the prefix
      // left on the body - and the contact lookup inside senderSplit compared a
      // CP437 string against UTF-8 contact names it could never equal.
      //
      // Channels only. A direct message carries no such prefix, so applying the
      // split there would take the first word off "hei: noe" and call it a name.
      // Only for incoming: an outgoing row records "to <channel>:", which says
      // where it went, and the accent bar down its edge already says it was ours.
      char raw_sender[RIFT_SENDER_MAX];
      const char* raw_body = p->msg;
      bool have_sender = false;
      if (!p->outgoing && p->conv.kind == RIFT_CONV_CHANNEL) {
        int skip = senderSplit(p->msg, raw_sender, sizeof(raw_sender));
        if (skip > 0) { raw_body = p->msg + skip; have_sender = true; }
      }

      char filtered[sizeof(p->msg)];
      riftTranslateUTF8(filtered, raw_body, sizeof(filtered));
      const char* body = filtered;
      char sender[RIFT_SENDER_MAX * 2];
      sender[0] = 0;
      if (have_sender) riftTranslateUTF8(sender, raw_sender, sizeof(sender));

      // Skipping the hop marker, for the same reason as above: on a direct message
      // there is no sender prefix to reparse, so the "(5) " sat in the stored
      // origin string and reached the row unchanged.
      char filtered_origin[sizeof(p->origin)];
      riftTranslateUTF8(filtered_origin, riftOriginSkipHops(p->origin),
                        sizeof(filtered_origin));
      int hh = (p->timestamp / 3600) % 24;
      int mm = (p->timestamp / 60) % 60;

      char ack_buf[16];
      const char* ack = deliveryLabel(p, ack_buf, sizeof(ack_buf));

      // A channel message arrives as "<sender>: <text>" with the row told it came
      // from the channel, so the header named the channel and the person who
      // spoke sat unnoticed at the head of the body. On a channel row the sender
      // is lifted out and becomes the name - which is what a conversation is read
      // by - and the body is drawn without the prefix. The split itself is above.

      int body_lines = wrapText(body, avail_px, 0, NULL, 0);
      int block_h = (body_lines + 1) * RIFT_LINE_H;

      y -= block_h;
      if (y + block_h <= _hist_top) break;   // wholly above: nothing left to show
      if (y > BODY_BOTTOM) continue;         // wholly below: scrolled past it
      // Anything overlapping the view is drawn in full, including the part that
      // falls outside. The mask below takes that part away.

      // Own messages carry a 2px accent bar down the left edge rather than being
      // right-aligned: on a 320px screen right alignment costs half the width for
      // every outgoing line, and this costs two pixels.
      // fg, not accent (September design round): no other row has a bar, so the
      // shape alone says "mine", and the accent is kept for selection and warning.
      if (p->outgoing) {
        display.setColor(rift_pal.fg);
        display.fillRect(0, y, 2, block_h - 2);
      }

      char tbuf[8];
      sprintf(tbuf, "%02d:%02d", hh, mm);
      display.setColor(rift_pal.dim);
      display.drawTextLeftAlign(4, y, tbuf);
      // The channel name is drawn in the channel's colour, so the row and the tab
      // above it carry the same identity. This started as a 4x6 block beside the
      // name, on the argument that the row already had three roles and a fourth
      // colour in the text would compete with the accent bar. Seen on hardware the
      // block was the weaker answer: it asks the eye to associate a mark with a
      // border, where the name in the same colour simply is the association.
      //
      // Contrast permits it because these four were chosen in the 4.5:1 text band
      // rather than the 3:1 band that a non-text marker could have used - see
      // riftChannelColour. Had they been picked for a block, this would not have
      // been available.
      //
      // Matched on the raw origin rather than the CP437-translated copy, because
      // that is what the channel is named.
      // A channel name keeps the channel's colour, so an outgoing row and the tab
      // above it still carry the same identity. Anything else - which is every
      // incoming row, since those record the sender rather than the channel - is
      // coloured from the sender's name instead of falling to grey. Three people
      // in a channel used to be three identical grey rows.
      uint16_t name_col;
      const char* shown_name;
      char decorated[80];
      if (have_sender) {
        // The name alone. The hop count used to be rebuilt in front of it here, to
        // keep what the channel header had carried - but the right-hand slot below
        // already prints "5 hops" in words, so the row said it twice, and the copy
        // in front of the name landed two pixels from the clock. "19:38" and "(5)"
        // with two pixels between them read as one token, which is precisely the
        // misreading that moved the count to the right in the first place.
        snprintf(decorated, sizeof(decorated), "%s:", sender);
        shown_name = decorated;
        // From the raw name, so a person is the same colour here as in the
        // conversation list, which hashes the same bytes.
        name_col = riftNameColour(raw_sender);
      } else {
        shown_name = filtered_origin;
        name_col = originColour(p->origin);
        if (name_col == RIFT_CHAN_COL_NONE) {
          char who[64];
          if (riftOriginName(p->origin, who, sizeof(who))) name_col = riftNameColour(who);
        }
      }
      display.setColor(name_col != RIFT_CHAN_COL_NONE ? name_col : rift_pal.mid);
      display.drawTextEllipsized(40, y, 230, shown_name);

      // The right end of this row answers "what happened to this packet", and the
      // hop count is the same kind of answer as the delivery state - so it goes
      // here rather than in front of the name, where "14:52 (6) Public:" read as
      // two clock times. Delivery wins the slot when there is one: a channel
      // message has no ack truth, so the hop count stands alone there.
      if (ack != NULL) {
        // ok for delivered, which begins with the check glyph; mid while it is
        // still on its way; accent for a send that never landed, which is the one
        // warning in the history.
        bool ok = ((unsigned char) ack[0] == 0xFB);
        bool waiting = (strcmp(ack, "sending") == 0);
        display.setColor(ok ? rift_pal.ok : (waiting ? rift_pal.mid : rift_pal.accent));
        display.drawTextRightAlign(314, y, ack);
      } else {
        int hops = 0;
        bool direct = false;
        if (riftOriginHops(p->origin, &hops, &direct)) {
          // "0 hops" rather than "direct": one column, one unit, never empty.
          char hb[12];
          snprintf(hb, sizeof(hb), "%d hop%s", direct ? 0 : hops, (!direct && hops == 1) ? "" : "s");
          display.setColor(rift_pal.mid);
          display.drawTextRightAlign(314, y, hb);
        }
      }

      display.setColor(rift_pal.fg);
      wrapText(body, avail_px, y + RIFT_LINE_H, &display, 6);
    }

    // Mask, then chrome. Two fills and the strip redrawn is the whole cost of
    // clipping on a driver that has none.
    display.setColor(rift_pal.bg);
    display.fillRect(0, 0, display.width(), _hist_top);
    // Down to the nav bar, not just to the compose rule.
    //
    // A five-pixel mask was not enough: the first block drawn has its top at or
    // above BODY_BOTTOM, so its body can reach BODY_BOTTOM plus a full block
    // height - sixty pixels for a six-line message - and that lands on the compose
    // line and the nav bar. Both are drawn after this, so masking the whole strip
    // costs nothing and is the only width that is actually correct.
    display.fillRect(0, BODY_BOTTOM + 1, display.width(), NAV_RULE_Y - (BODY_BOTTOM + 1));
    if (heading != NULL) renderHeading(display, heading);
    renderTabs(display);
    display.setTextSize(1);

    // The window pattern, in pixels: the history is anchored at its newest end, so
    // the window's top is the content height less the view less the scroll.
    {
      long first = (long) total_h - (long) view_h - (long) _scroll;
      if (first < 0) first = 0;
      riftDrawThumb(display, _hist_top, BODY_BOTTOM - _hist_top, first, total_h, view_h);
    }

    // An empty conversation now exists, where it could not before: the history used
    // to show every message from everywhere, so it was blank only on a device that
    // had never received anything. A blank panel reads as a screen that failed to
    // draw, so it says which of the two it is.
    if (n_conv == 0) {
      display.setColor(rift_pal.dim);
      display.drawTextLeftAlign(6, _hist_top + 6,
                                msg_log.count > 0 ? "No messages here yet."
                                                  : "No messages yet.");
    }

    // Compose line - show the tail once the text outgrows one line.
    display.setColor(rift_pal.rule);
    display.fillRect(0, INPUT_Y - 4, display.width(), 1);

    display.setColor(rift_pal.mid);
    if (_len == 0) {
      display.drawTextRightAlign(316, INPUT_Y, "ENTER: conversations");
    } else {
      // MeshCore truncates at MAX_TEXT_LEN, so show how close the message is: mid
      // while there is room, fg for the last ten characters, accent at the cap.
      char cnt[12];
      int cap = channelCapacity();
      snprintf(cnt, sizeof(cnt), "%d/%d", _len, cap);
      display.setColor(_len >= cap ? rift_pal.accent : (_len >= cap - 10 ? rift_pal.fg : rift_pal.mid));
      display.drawTextRightAlign(316, INPUT_Y, cnt);
    }

    // Translate first, then take the tail of the *translated* text. The compose
    // buffer holds UTF-8, because that is what goes on the air, but CP437 output
    // is one byte per code point - so slicing after translation cannot land
    // inside a sequence. Slicing _input by byte could, and would draw a block or
    // a stray glyph as soon as the line was long enough to scroll.
    int max_chars = (display.width() - 70) / RIFT_CHAR_W;
    char filtered[sizeof(_input)];   // translation never outputs more bytes than it reads
    riftTranslateUTF8(filtered, _input, sizeof(filtered));
    int flen = (int) strlen(filtered);
    const char* shown = filtered;
    if (flen > max_chars) shown = filtered + (flen - max_chars);

    display.setColor(rift_pal.accent);
    display.setCursor(2, INPUT_Y);
    display.print(">");
    display.setColor(rift_pal.fg);
    display.print(" ");
    display.print(shown);
    display.print("_");

    renderNavBar(display, RIFT_NAV_COMMS);
    return 1000;   // keystrokes force a repaint via _next_refresh; avoid flicker
  }

  // tapping the channel strip switches target; contacts still go via the picker,
  // since there can be many of them and they don't fit a strip
  bool handleTouch(int x, int y) override {
    // A tap on a row opens it, which is the whole reason this screen exists and
    // was the one gesture it refused. Safe now that a drag suppresses its own
    // release tap - before that fix this would have opened a conversation every
    // time you scrolled the list.
    if (_picking) {
      for (int r = 0; r < _pick_rows_drawn; r++) {
        if (y < _pick_row_y[r] - 2 || y > _pick_row_y[r] + RIFT_LINE_H - 2) continue;
        openPick(_pick_row_of[r]);
        return true;
      }
      return false;   // a heading, or the margin: not a row
    }
    if (_tabs_y <= 0) return false;   // strip hasn't been drawn yet

    // Scrolling by tap rather than by swipe: the touch driver reports once per
    // completed tap, on release, so there is no drag to follow. Upper half of the
    // history goes back, lower half comes forward - the same direction the text
    // moves, which is the part that has to be guessed right.
    //
    // This exists because the trackball is the only other way to scroll here and
    // its left and right change screen, so a slightly off flick leaves the
    // conversation entirely.
    // A tap in the history deliberately does nothing.
    //
    // It used to page half a screen, which was a second way to scroll competing
    // with the drag - and it is what made dragging to the bottom spring backwards,
    // because a release in the upper half paged away from it. With the drag
    // working and the wheel and arrow keys covering discrete steps, a tap here has
    // no unambiguous meaning: there is no way to tell "I meant to page" from "I
    // touched the screen and did not move".

    if (y < _tabs_y - 2 || y > _tabs_y + 13) return false;

    if (_tab_w <= 0) return false;   // strip hasn't been drawn yet
    int col = x / _tab_w;
    // in DM mode the last column holds the DM marker rather than a channel, so a
    // tap there must not select the tab that would have been drawn under it
    if (col < 0 || col >= _tab_visible) return false;
    int i = _tab_scroll + col;
    if (i < 0 || i >= _tab_count) return false;

    _target_is_channel = true;
    _target_channel_idx = _tabs[i].idx;
    StrHelper::strncpy(_target_name, _tabs[i].name, sizeof(_target_name));
    _scroll = 0;   // a different conversation: land on its newest
    return true;
  }

  // Follows the finger. Dragging down pulls older messages into view, the same
  // direction the content moves, which is the part that has to feel right.
  // One split, used by both the measuring pass and the render, because a
  // conversation measured one way and drawn another scrolls to the wrong end.
  //
  // The first delimiter is the answer for every ordinary name. Only when the
  // message offers a second candidate is the contact list consulted, which keeps
  // an O(contacts) lookup off the common path entirely.
  int senderSplit(const char* text, char* out, int out_sz) {
    int first = riftChannelSenderNth(text, 0, out, out_sz);
    if (first <= 0) return 0;
    if (riftChannelSenderNth(text, 1, NULL, 0) <= 0) return first;   // unambiguous

    for (int nth = 0; nth < 3; nth++) {
      char cand[RIFT_SENDER_MAX];
      int skip = riftChannelSenderNth(text, nth, cand, sizeof(cand));
      if (skip <= 0) break;
      ContactInfo* known = the_mesh.searchContactsByPrefix(cand);
      if (known != NULL && strcmp(known->name, cand) == 0) {
        if (out != NULL && out_sz > 0) {
          memcpy(out, cand, (size_t) (out_sz < (int) sizeof(cand) ? out_sz : (int) sizeof(cand)));
          out[out_sz - 1] = 0;
        }
        return skip;
      }
    }
    return first;   // nobody we know: the first delimiter is the best guess
  }

  bool handleDrag(int dy) override {
    // The picker moves its cursor and lets ensurePickVisible follow, which is what
    // the arrow keys already do - so a finger and the trackball mean the same
    // thing here rather than two kinds of scrolling on one list.
    //
    // Not wrapped, unlike the keys: wrapping under a finger reads as the list
    // having jumped rather than as the cursor reaching the end.
    if (_picking) {
      if (_pick_count == 0) return false;
      int steps = riftDragSteps(&_pick_drag_residual, dy, RIFT_DRAG_PITCH);
      if (steps == 0) return false;
      int n = _pick_idx + steps;
      if (n < 0) n = 0;
      if (n >= _pick_count) n = _pick_count - 1;
      if (n == _pick_idx) return false;
      _pick_idx = n;
      ensurePickVisible();
      return true;
    }
    _scroll += dy;
    if (_scroll < 0) _scroll = 0;
    return true;
  }

  bool handleInput(char c) override {
    if (_picking) return handlePickerInput(c);

    if (c == KEY_NEXT) { _task->cycleNavScreen(1); return true; }
    if (c == KEY_PREV) { _task->cycleNavScreen(-1); return true; }
    if (c == KEY_RIGHT) { _task->cycleNavScreen(1); return true; }
    if (c == KEY_LEFT) { _task->cycleNavScreen(-1); return true; }

    // A line at a time rather than a message at a time: the same units the drag
    // uses, so the two agree about what a small step is.
    if (c == KEY_UP) { _scroll += RIFT_LINE_H; return true; }
    if (c == KEY_DOWN) {
      _scroll = _scroll > RIFT_LINE_H ? _scroll - RIFT_LINE_H : 0;
      return true;
    }

    // Enter sends, or opens the target picker when there's nothing to send.
    // (The T-Deck keyboard has no dedicated ESC key, so Enter carries both.)
    if (c == KEY_ENTER) {
      if (_len > 0) { send(); } else { openPicker(); }
      return true;
    }
    if (c == KEY_CANCEL) { clearInput(); return true; }
    // backspace deletes while composing; with nothing to delete this is
    // already the top level, so there is nowhere further back to go
    if (c == RIFT_KEY_BACK) {
      // Delete a whole code point, not a byte. Dropping one byte of a two-byte
      // character leaves a dangling lead byte, which is not valid UTF-8 and which
      // the next backspace would then have to clean up. Truncating to _len-1
      // without splitting a sequence is exactly what this helper does, so the
      // walk is not reimplemented here.
      deleteLastChar();
      return true;
    }

    if (c >= 32 && c < 127 && _len < channelCapacity()) {
      _input[_len++] = c;
      _input[_len] = 0;

      // Two presses of the same vowel in quick succession offer its Nordic forms.
      // Both letters are inserted first and the picker replaces the pair, so
      // cancelling leaves exactly what was typed - which is what makes a false
      // trigger on a genuine double vowel cost one keypress rather than a word.
      //
      // millis() - _last_key_ms rather than a stored deadline: unsigned arithmetic
      // is wrap-safe, a future deadline is not.
      //
      // The buffer is checked as well as the keypress history, and that is not
      // belt-and-braces: the picker deletes two characters when a form is chosen,
      // so it must only open when two are actually there. _last_key alone did not
      // guarantee it, because backspace does not clear it - typing "Hei", then a,
      // backspace, a, opened the picker over a buffer holding one a, and choosing
      // a form deleted the i as well and produced "Heæ". Requiring the character
      // before the one just inserted to match makes the pair a fact rather than an
      // inference. _input[_len - 1] is c by construction, so only _len - 2 is
      // worth testing.
      if (c == _last_key && _last_key_ms != 0
          && _len >= 2 && _input[_len - 2] == c
          && millis() - _last_key_ms <= RIFT_DOUBLETAP_MILLIS) {
        _last_key = 0;          // a third tap starts over rather than re-firing
        _last_key_ms = 0;
        _task->openNordicPicker(c);
      } else {
        _last_key = c;
        _last_key_ms = millis();
      }
      return true;
    }
    return false;
  }

public:
  // Insert a character the keyboard cannot produce, as UTF-8. Used by the Nordic
  // picker. Returns false if it would not fit - the caller should not silently
  // drop it, since the user chose it deliberately.
  //
  // What goes in here is what goes on the air. CP437 codes must never be inserted:
  // o-slash on the wire is 0xC3 0xB8, while 0x01 is a display-side placeholder
  // that other clients would read as a C0 control byte.
  bool insertUtf8(const char* utf8) {
    if (utf8 == NULL) return false;
    int n = (int) strlen(utf8);
    if (n <= 0 || _len + n > channelCapacity()) return false;
    memcpy(&_input[_len], utf8, n);
    _len += n;
    _input[_len] = 0;
    return true;
  }

  // Remove one whole code point. Dropping a single byte of a two-byte character
  // would leave a dangling lead byte, which is not valid UTF-8 and which the next
  // backspace would have to clean up. Truncating to _len-1 without splitting a
  // sequence is exactly what this helper does, so the walk is not reimplemented.
  //
  // Also used by the Nordic picker, which replaces the base letter the initial
  // keypress inserted.
  void deleteLastChar() {
    if (_len <= 0) return;
    _len = (int) mesh::validUtf8PrefixLength(_input, (size_t) (_len - 1));
    _input[_len] = 0;
  }
private:
};

// Renames a watched RF device, so RADAR shows what you call it rather than what it
// broadcasts - which is often absent, duplicated, or the address rendered as text.
//
// An overlay: the watch list stays visible underneath and is handed back untouched.
// Fifth use of that mechanism.
//
// The name lives on the watch entry, which is why marking a device is the
// prerequisite. It was already persisted to /rift.cfg and already read by the
// arrival alert and the present indicator, so a user-set name flows through all of
// them without a second field - there is one name per device and everything reads it.
//
// Worth knowing, and the same caveat the watch itself carries: the identity is a
// hardware address. A BLE device that re-randomises its address will stop matching,
// and the name goes with the entry it was attached to. Stable for Wi-Fi access
// points and for beacons and tags with a static address.
class RiftRenameWatchScreen : public RiftScreen {
  UITask* _task;
  RiftTextInput _edit;
  int _idx = -1;

public:
  RiftRenameWatchScreen(UITask* task) : _task(task) { }

  bool isOverlay() const override { return true; }
  // half-typed text: a message arriving must not take the screen away
  bool isModal() const override { return true; }

  bool openFor(int watch_idx) {
#ifdef RIFT_RADAR
    if (watch_idx < 0 || watch_idx >= rf_watch_count) return false;
    _idx = watch_idx;
    _edit.begin(rf_watch[_idx].name, sizeof(rf_watch[_idx].name) - 1);
    return true;
#else
    (void) watch_idx;
    return false;
#endif
  }

  int render(DisplayDriver& display) override {
#ifdef RIFT_RADAR
    const int x = 8, w = display.width() - 16;
    const int y = 60, h = 96;

    display.setColor(rift_pal.bg);
    display.fillRect(x, y, w, h);
    // A 2px frame in rule (September design round): a box says "on top" by being
    // a box, where the accent said "act on all of this". Two nested rects, since
    // the driver has no line width.
    display.setColor(rift_pal.rule);
    display.drawRect(x, y, w, h);
    display.drawRect(x + 1, y + 1, w - 2, h - 2);

    display.setTextSize(1);
    display.setColor(rift_pal.accent);
    display.drawTextLeftAlign(x + 6, y + 6, "NAME THIS DEVICE");

    // the address, because that is the identity and the name is only a label on it
    if (_idx >= 0 && _idx < rf_watch_count) {
      const uint8_t* k = rf_watch[_idx].key;
      char addr[32];
      snprintf(addr, sizeof(addr), "%s %02X:%02X:%02X:%02X:%02X:%02X",
               rf_watch[_idx].is_wifi ? "wifi" : "ble",
               k[0], k[1], k[2], k[3], k[4], k[5]);
      display.setColor(rift_pal.dim);
      display.drawTextLeftAlign(x + 6, y + 20, addr);
    }

    _edit.render(display, x + 6, y + 42, w - 12);

    display.setColor(rift_pal.dim);
    display.drawTextLeftAlign(x + 6, y + h - 14, "ENTER save   BACKSPACE delete / back");
    return 1000;
#else
    (void) display;
    return 1000;
#endif
  }

  bool handleInput(char c) override {
#ifdef RIFT_RADAR
    if (c == KEY_ENTER) {
      if (_idx >= 0 && _idx < rf_watch_count && _edit.len > 0) {
        // under the lock: the BLE advertisement callback reads this table on core 0
        portENTER_CRITICAL(&rf_mux);
        StrHelper::strncpy(rf_watch[_idx].name, _edit.buf, sizeof(rf_watch[_idx].name));
        portEXIT_CRITICAL(&rf_mux);
        riftSaveSettings();
        riftLogf("watch named %s", rf_watch[_idx].name);
        _task->showAlert("Name saved", 1200);
      } else if (_edit.len == 0) {
        // an empty name would leave the row unreadable, so refuse rather than
        // storing a blank and calling it saved
        _task->showAlert("Name can't be empty", 1400);
        return true;
      }
      _task->dismissOverlay();
      return true;
    }
    if (_edit.handleKey(c)) return true;         // consumed as text editing
    if (c == RIFT_KEY_BACK || c == KEY_CANCEL) { _task->dismissOverlay(); return true; }
    return true;   // do not let stray keys navigate away mid-edit
#else
    (void) c;
    return true;
#endif
  }
};

// Repeater control: log in, read the stats, ask for telemetry, run a CLI command.
//
// An overlay reached with ENTER on a repeater in NODES, where that key used to
// report "Repeaters can't receive a DM" and stop. Everything it shows was
// already arriving at the radio and being forwarded to a phone that is usually
// not attached; RiftRepeater.h keeps it, and this draws it.
//
// Three modes rather than three screens. Typing a password and typing a command
// are the same interaction with a different prompt, and a mode keeps the stats
// on screen underneath instead of replacing them.
#define RIFT_RP_VIEW      0
#define RIFT_RP_PASSWORD  1
#define RIFT_RP_COMMAND   2
#define RIFT_RP_MENU      3

// The commands worth reaching without typing them.
//
// Not a new mechanism: every one of these is text sent down the same CLI that
// free entry uses, and each string is a command CommonCLI actually implements -
// checked against src/helpers/CommonCLI.cpp rather than assumed. The panel could
// already send `advert` the day it shipped; what it could not do was tell you
// that, or spare you typing it on a thumb keyboard in the field.
//
// Deliberately absent: `password` takes a secret and must never sit in a list
// where it can be picked by accident; `poweroff` and `start ota` are one
// keypress from unrecoverable. Those remain available by typing, which is the
// right amount of friction for them, and typing now asks for a confirmation too.
//
// `erase` is a special case worth stating rather than leaving to be rediscovered:
// CommonCLI runs it only when sender_timestamp == 0, and every command from here
// carries the node clock, so it cannot work from RIFT at all. The v0.9.0 notes
// said it remained available by typing. It does not.
//
// The whole CLI is admin-only on the far side - simple_repeater gates it on
// client->isAdmin() - so this menu is too, and refuses before spending airtime.
// A sentinel rather than a flag field: the table stays two columns, and the one
// entry that is not a CLI command says so by identity.
#define RIFT_CMD_REGIONS ((const char*) 1)

struct RiftRepCmd {
  const char* label;
  const char* cmd;      // NULL: free text entry. RIFT_CMD_REGIONS: not a command.
};

// No confirm flag: riftCliIsDestructive() decides, so the menu and free text
// obey one rule instead of the menu having its own.
static const RiftRepCmd RIFT_REP_CMDS[] = {
  { "Send advert (flood)",  "advert"         },
  { "Send advert (0-hop)",  "advert.zerohop" },
  { "Neighbours heard",     "neighbors"      },
  // Not a CLI command: an anonymous REGIONS request, which is how a scope is
  // discovered. NULL cmd would mean free text, so it carries its own marker.
  { "Regions it floods",    RIFT_CMD_REGIONS },
  { "Firmware version",     "ver"            },
  { "Board",                "board"          },
  { "Read clock",           "clock"          },
  // Sends our RTC as the message timestamp, which is what the far side adopts.
  // A repeater with a wrong clock rejects traffic as replayed, so this is a
  // repair, not a convenience - and it only moves a clock forwards, so it gets a
  // confirmation showing the value about to be sent.
  { "Set clock from RIFT",  "clock sync"     },
  { "Clear stats",          "clear stats"    },
  { "Reboot",               "reboot"         },
  { "Type a command...",    NULL             },
};
#define RIFT_REP_CMD_COUNT ((int) (sizeof(RIFT_REP_CMDS) / sizeof(RIFT_REP_CMDS[0])))

// LPP type to a label narrow enough for the panel. Only the types the reader in
// MyMesh decodes can reach here, so anything else is a bug rather than a device
// we have not met.
static const char* riftLppLabel(uint8_t type) {
  switch (type) {
    case LPP_VOLTAGE:             return "volt";
    case LPP_CURRENT:             return "amp";
    case LPP_POWER:               return "watt";
    case LPP_TEMPERATURE:         return "temp";
    case LPP_BAROMETRIC_PRESSURE: return "pres";
    case LPP_RELATIVE_HUMIDITY:   return "hum";
    case LPP_ALTITUDE:            return "alt";
    default:                      return "?";
  }
}

static const char* riftLppUnit(uint8_t type) {
  switch (type) {
    case LPP_VOLTAGE:             return "V";
    case LPP_CURRENT:             return "A";
    case LPP_POWER:               return "W";
    case LPP_TEMPERATURE:         return "C";
    case LPP_BAROMETRIC_PRESSURE: return "hPa";
    case LPP_RELATIVE_HUMIDITY:   return "%";
    case LPP_ALTITUDE:            return "m";
    default:                      return "";
  }
}

class RiftRepeaterScreen : public RiftScreen {
  UITask* _task;
  uint8_t _key[6];
  bool _have_key;
  int _mode;
  RiftTextInput _edit;
  uint8_t _type;      // advert type of the node this panel is showing
  int _menu_sel;
  // Armed by the first Enter on an entry that asks for confirmation, cleared by
  // any movement. Holding the index rather than a bool means moving the cursor
  // and pressing Enter cannot fire the command you were confirming.
  int _confirm_idx;

  ContactInfo* contact() const {
    if (!_have_key) return NULL;
    return the_mesh.lookupContactByPubKey((uint8_t*) _key, 6);
  }

  // What this node type can actually be asked.
  //
  // The panel is reached for anything that cannot take a direct message, which
  // is repeaters and sensors. Offering a sensor a login, a stats request and an
  // admin CLI is offering three things that will time out, and it made the
  // "ENTER: read" label on its row untrue. Telemetry is the one a sensor answers.
  bool allowsLogin()     const { return _type != RIFT_ADV_SENSOR; }
  bool allowsStats()     const { return _type != RIFT_ADV_SENSOR; }
  bool allowsTelemetry() const { return true; }
  bool allowsCli()       const { return _type != RIFT_ADV_SENSOR; }

  // The one place a CLI command leaves this panel, so the destructive-command
  // rule is enforced once instead of per entry point. Returns true when the
  // caller should stay where it is - either armed for a confirmation, or
  // refused - and false when the command has been sent.
  //
  // `confirm_key` identifies what is armed: the menu passes its index, free text
  // passes -2. Comparing it rather than holding a bool means arming Reboot and
  // then moving to something else cannot fire Reboot.
  bool sendCli(const char* text, int confirm_key) {
    RiftRepeaterSession& s = riftRepeater();
    ContactInfo* ct = contact();
    if (ct == NULL) { _task->showAlert("Contact is gone", 1400); return false; }
    if (s.pending() != RIFT_REP_IDLE) { _task->showAlert("Still waiting", 1200); return true; }

    // Refused rather than confirmed: upstream will not move a clock backwards,
    // so sending an unset one cannot be undone with the same command.
    if (strncmp(text, "clock sync", 10) == 0
        && !riftClockPlausible(the_mesh.getRTCClock()->getCurrentTime())) {
      _task->showAlert("Set RIFT clock first", 1800);
      return false;
    }

    if (riftCliIsDestructive(text) && _confirm_idx != confirm_key) {
      _confirm_idx = confirm_key;
      return true;
    }
    _confirm_idx = -1;

    uint32_t est = 0;
    if (!the_mesh.riftCliCommand(*ct, text, est)) {
      _task->showAlert("No packet free - try again", 1400);
    }
    return false;
  }

  static const char* loginText(uint8_t state) {
    switch (state) {
      case RIFT_LOGIN_WAITING: return "logging in";
      case RIFT_LOGIN_OK:      return "logged in";
      case RIFT_LOGIN_FAILED:  return "login refused";
      case RIFT_LOGIN_TIMEOUT: return "no answer";
      default:                 return "not logged in";
    }
  }

public:
  RiftRepeaterScreen(UITask* task)
     : _task(task), _have_key(false), _mode(RIFT_RP_VIEW), _type(0), _menu_sel(0), _confirm_idx(-1) {
    memset(_key, 0, sizeof(_key));
  }

  bool isOverlay() const override { return true; }
  // Modal only while typing. In view mode an arriving message may take the
  // screen, the same as anywhere else; half-typed text may not be thrown away.
  bool isModal() const override { return _mode != RIFT_RP_VIEW; }

  bool openFor(const uint8_t* pub_key) {
    if (pub_key == NULL) return false;
    ContactInfo* c = the_mesh.lookupContactByPubKey((uint8_t*) pub_key, 6);
    if (c == NULL) return false;
    memcpy(_key, pub_key, sizeof(_key));
    _type = c->type;
    _have_key = true;
    _mode = RIFT_RP_VIEW;
    _menu_sel = 0;
    _confirm_idx = -1;
    riftRepeater().setTarget(pub_key);   // drops any state belonging to another node
    return true;
  }

  int render(DisplayDriver& display) override {
    RiftRepeaterSession& s = riftRepeater();   // observed only; the loop services it

    const int x = 4, w = display.width() - 8;
    const int y = 18, h = 202;

    display.setColor(rift_pal.bg);
    display.fillRect(x, y, w, h);
    // A 2px frame in rule (September design round): a box says "on top" by being
    // a box, where the accent said "act on all of this". Two nested rects, since
    // the driver has no line width.
    display.setColor(rift_pal.rule);
    display.drawRect(x, y, w, h);
    display.drawRect(x + 1, y + 1, w - 2, h - 2);

    display.setTextSize(1);
    const int lx = x + 5;
    int ly = y + 5;

    ContactInfo* c = contact();
    char line[64];
    snprintf(line, sizeof(line), "%s", c ? c->name : "gone");
    display.setColor(rift_pal.accent);
    display.drawTextLeftAlign(lx, ly, line);

    // Session state on the same row, right-aligned, because it is the one thing
    // that decides what the keys below will do.
    display.setColor(s.loginState() == RIFT_LOGIN_OK ? UIColor::primary_txt : rift_pal.dim);
    snprintf(line, sizeof(line), "%s%s", loginText(s.loginState()),
             s.isAdmin() ? " (admin)" : "");
    display.drawTextRightAlign(x + w - 5, ly, line);
    ly += 12;

    display.setColor(rift_pal.rule);
    display.fillRect(lx, ly, w - 10, 1);
    ly += 5;

    // While choosing a command the body is the list, not the readings. Nothing
    // below is drawn, which is also why the list cannot overrun a panel whose
    // height depends on how many of the blocks below happen to have data.
    if (_mode == RIFT_RP_MENU) {
      for (int i = 0; i < RIFT_REP_CMD_COUNT; i++) {
        bool sel = (i == _menu_sel);
        if (sel) {
          display.setColor(rift_pal.accent);
          display.fillRect(lx - 2, ly - 1, w - 6, 12);
          display.setColor(rift_pal.on_accent);
        } else {
          display.setColor(UIColor::primary_txt);
        }
        display.drawTextLeftAlign(lx + 2, ly + 1, RIFT_REP_CMDS[i].label);
        ly += 12;
      }
      const int mfy = y + h - 24;
      display.setColor(rift_pal.dim);
      if (_confirm_idx == _menu_sel) {
        display.setColor(rift_pal.accent);
        const char* cmd = RIFT_REP_CMDS[_menu_sel].cmd;
        // RIFT_CMD_REGIONS is a sentinel pointer, not a string. Nothing arms a
        // confirmation on it today, so this is unreachable - but a sentinel that
        // would fault if it ever were reached is not something to leave guarded
        // by control flow alone.
        if (cmd != NULL && cmd != RIFT_CMD_REGIONS && strncmp(cmd, "clock sync", 10) == 0) {
          // The value itself, because a clock can only be pushed forwards and a
          // wrong one cannot be taken back. Seeing 2031 here is the only warning
          // that exists.
          uint32_t now = the_mesh.getRTCClock()->getCurrentTime();
          int yy, mo, dd, hh, mi;
          riftCivilFromEpoch(now, &yy, &mo, &dd, &hh, &mi);
          char cline[48];
          snprintf(cline, sizeof(cline), "Send %04d-%02d-%02d %02d:%02d UTC?", yy, mo, dd, hh, mi);
          display.drawTextLeftAlign(lx, mfy, cline);
        } else {
          display.drawTextLeftAlign(lx, mfy, "ENTER again to confirm");
        }
      } else {
        display.drawTextLeftAlign(lx, mfy, "UP/DOWN choose   ENTER send");
      }
      display.drawTextLeftAlign(lx, mfy + 11, "BACK cancel");
      return 1000;
    }

    // ---- stats ----
    if (s.haveStats()) {
      const RiftRepeaterStats& st = s.stats();
      char up[12], air[12];
      riftFormatDuration(st.up_time_secs, up, sizeof(up));
      riftFormatDuration(st.air_time_secs, air, sizeof(air));
      int duty = riftDutyTenths(st.air_time_secs, st.up_time_secs);

      display.setColor(UIColor::primary_txt);
      snprintf(line, sizeof(line), "up %-8s  air %-8s", up, air);
      display.drawTextLeftAlign(lx, ly, line);
      ly += 12;

      if (duty >= 0) {
        snprintf(line, sizeof(line), "duty %d.%d%%   queue %u",
                 duty / 10, duty % 10, (unsigned) st.tx_queue_len);
      } else {
        snprintf(line, sizeof(line), "duty -       queue %u", (unsigned) st.tx_queue_len);
      }
      display.drawTextLeftAlign(lx, ly, line);
      ly += 12;

      snprintf(line, sizeof(line), "batt %u.%02uV  noise %d",
               (unsigned) (st.batt_milli_volts / 1000),
               (unsigned) ((st.batt_milli_volts % 1000) / 10),
               (int) st.noise_floor);
      display.drawTextLeftAlign(lx, ly, line);
      ly += 12;

      // SNR arrives multiplied by four. Printed as one decimal rather than
      // divided into an int, which would report -6.5 dB as -6. The sign is
      // carried separately: between -1 and 0 the integer part is zero, so
      // printing it signed loses the minus - and -0.5 dB is a normal reading.
      int snr10 = (st.last_snr_x4 * 10) / 4;
      int snr_abs = snr10 < 0 ? -snr10 : snr10;
      snprintf(line, sizeof(line), "rssi %d      snr %s%d.%d",
               (int) st.last_rssi, snr10 < 0 ? "-" : "", snr_abs / 10, snr_abs % 10);
      display.drawTextLeftAlign(lx, ly, line);
      ly += 12;

      snprintf(line, sizeof(line), "rx %u  tx %u  err %u",
               (unsigned) st.packets_recv, (unsigned) st.packets_sent,
               (unsigned) st.err_events);
      display.drawTextLeftAlign(lx, ly, line);
      ly += 12;

      // A dash where an older repeater sent nothing, never a zero we invented.
      if (st.have_dups) {
        snprintf(line, sizeof(line), "dups %u/%u",
                 (unsigned) st.direct_dups, (unsigned) st.flood_dups);
      } else {
        snprintf(line, sizeof(line), "dups -    (older firmware)");
      }
      display.setColor(st.have_dups ? UIColor::primary_txt : rift_pal.dim);
      display.drawTextLeftAlign(lx, ly, line);
      ly += 12;

      char age[12];
      riftFormatDuration((millis() - s.statsAt()) / 1000, age, sizeof(age));
      display.setColor(rift_pal.dim);
      snprintf(line, sizeof(line), "read %s ago", age);
      display.drawTextLeftAlign(lx, ly, line);
      ly += 12;
    } else {
      display.setColor(rift_pal.dim);
      display.drawTextLeftAlign(lx, ly, "no stats yet - press S");
      ly += 12;
    }

    // ---- telemetry ----
    if (s.haveTelemetry()) {
      display.setColor(rift_pal.rule);
      display.fillRect(lx, ly, w - 10, 1);
      ly += 4;
      display.setColor(UIColor::primary_txt);
      int shown = 0;
      char cell[20];
      line[0] = 0;
      for (int i = 0; i < s.telemetryCount() && shown < 6; i++) {
        const RiftTelemReading& r = s.telemetry(i);
        snprintf(cell, sizeof(cell), "%s %.1f%s ",
                 riftLppLabel(r.type), r.value, riftLppUnit(r.type));
        if (strlen(line) + strlen(cell) >= sizeof(line)) break;
        strcat(line, cell);
        shown++;
      }
      display.drawTextLeftAlign(lx, ly, line);
      ly += 12;
    }

    // ---- CLI transcript, newest first ----
    //
    // 10px, and the one pitch in this firmware that is deliberately not 12. The
    // rule is about rows that get read at arm's length, and the stats and the
    // command menu above are exactly that - they moved to 12 when it turned out
    // this panel had quietly been running the whole body at 11, which is 3px of
    // air against a rule that names 4 as the floor. This block is different: it is
    // raw command output, scrollback rather than layout, and how much of it fits
    // is the only thing it is for. The loop below is bounded by the footer, so the
    // 6px the rows above gained comes out of here - one line in the fullest case.
    if (s.cliCount() > 0) {
      display.setColor(rift_pal.rule);
      display.fillRect(lx, ly, w - 10, 1);
      ly += 4;
      // Stops at the footer rather than at a row count: the blocks above vary in
      // height, and a fixed count overran the panel when stats were present.
      const int footer_y = y + h - 26;
      for (int i = 0; i < s.cliCount() && ly + 10 <= footer_y; i++) {
        display.setColor(s.cliLine(i)[0] == '>' ? rift_pal.dim : UIColor::primary_txt);
        // Stored as UTF-8 and translated here, the same order every other RIFT
        // screen uses. Drawn raw, a node name from `neighbors` containing a
        // Nordic character rendered as two CP437 blocks.
        char shown[RIFT_REP_CLI_TEXT];
        riftTranslateUTF8(shown, s.cliLine(i), sizeof(shown));
        display.drawTextLeftAlign(lx, ly, shown);
        ly += 10;
      }
    }

    // ---- footer: prompt or key hints ----
    const int fy = y + h - 24;
    if (_mode != RIFT_RP_VIEW) {
      display.setColor(rift_pal.accent);
      if (_mode == RIFT_RP_COMMAND && _confirm_idx == -2) {
        display.drawTextLeftAlign(lx, fy, "ENTER again to send");
      } else {
        display.drawTextLeftAlign(lx, fy,
          _mode == RIFT_RP_PASSWORD ? "PASSWORD" : "COMMAND");
      }
      _edit.render(display, lx, fy + 11, w - 10);
      return 400;
    }

    display.setColor(rift_pal.dim);
    if (s.pending() != RIFT_REP_IDLE) {
      display.drawTextLeftAlign(lx, fy, "waiting for answer...");
      display.drawTextLeftAlign(lx, fy + 11, "BACK close");
      return 400;   // so the wait ends visibly rather than on the next keypress
    }
    // The hints are the affordance, so they follow capability rather than
    // listing keys the node will not answer - the mistake that made repeater
    // control itself invisible.
    if (allowsLogin()) {
      display.drawTextLeftAlign(lx, fy, "L login   S stats   T telemetry");
    } else {
      display.drawTextLeftAlign(lx, fy, "T telemetry");
    }
    display.drawTextLeftAlign(lx, fy + 11,
      (allowsCli() && s.isAdmin()) ? "C commands   BACK close" : "BACK close");
    return 1000;
  }

  bool handleInput(char c) override {
    RiftRepeaterSession& s = riftRepeater();

    if (_mode == RIFT_RP_MENU) {
      // Movement disarms a pending confirmation, so the second Enter can only
      // ever fire the entry it was armed on.
      if (c == KEY_UP)   { if (_menu_sel > 0) _menu_sel--; _confirm_idx = -1; return true; }
      if (c == KEY_DOWN) { if (_menu_sel + 1 < RIFT_REP_CMD_COUNT) _menu_sel++; _confirm_idx = -1; return true; }
      if (c == RIFT_KEY_BACK || c == KEY_CANCEL) { _mode = RIFT_RP_VIEW; _confirm_idx = -1; return true; }
      if (c == KEY_ENTER) {
        const RiftRepCmd& e = RIFT_REP_CMDS[_menu_sel];
        if (e.cmd == NULL) {              // the free-text entry
          _edit.begin("", 63);
          _mode = RIFT_RP_COMMAND;
          _confirm_idx = -1;
          return true;
        }
        if (e.cmd == RIFT_CMD_REGIONS) {
          _confirm_idx = -1;
          ContactInfo* ct = contact();
          if (ct == NULL) { _task->showAlert("Contact is gone", 1400); _mode = RIFT_RP_VIEW; return true; }
          if (s.pending() != RIFT_REP_IDLE) { _task->showAlert("Still waiting", 1200); return true; }
          uint32_t est = 0;
          // Needs a route: the far side answers a directly routed request only,
          // and the reply path we hand it is that route reversed. Said here, in
          // words, rather than letting the send fail with an alert about packets.
          if (riftHopsUnknown(ct->out_path_len)) {
            _task->showAlert("No route to repeater yet", 1400);
          } else if (!the_mesh.riftRegionsReq(*ct, est)) {
            _task->showAlert("No packet free - try again", 1400);
          }
          _mode = RIFT_RP_VIEW;
          return true;
        }
        if (sendCli(e.cmd, _menu_sel)) return true;   // armed, refused, or busy
        _mode = RIFT_RP_VIEW;             // sent: back to the transcript, where the reply lands
        return true;
      }
      return true;   // the menu owns the keyboard while it is up
    }

    if (_mode != RIFT_RP_VIEW) {
      if (c == KEY_ENTER) {
        ContactInfo* ct = contact();
        if (ct == NULL) { _task->showAlert("Contact is gone", 1400); _mode = RIFT_RP_VIEW; return true; }
        uint32_t est = 0;
        if (_mode == RIFT_RP_PASSWORD) {
          if (!the_mesh.riftLogin(*ct, _edit.buf, est)) {
            _task->showAlert("No packet free - try again", 1400);
          }
        } else {
          if (_edit.len == 0) { _mode = RIFT_RP_VIEW; return true; }
          // Same gate as the menu. -2 is the free-text confirmation slot: a
          // typed `reboot` now asks a second time, which it did not before.
          if (sendCli(_edit.buf, -2)) return true;   // stay in the field, armed
        }
        // Wiped either way, and immediately: a password left in the edit buffer
        // would be one keypress from being redrawn unmasked in command mode.
        memset(_edit.buf, 0, sizeof(_edit.buf));
        _edit.len = 0;
        _mode = RIFT_RP_VIEW;
        return true;
      }
      if (_edit.handleKey(c)) { _confirm_idx = -1; return true; }   // text changed: disarm
      if (c == RIFT_KEY_BACK || c == KEY_CANCEL) {
        memset(_edit.buf, 0, sizeof(_edit.buf));
        _edit.len = 0;
        _confirm_idx = -1;
        _mode = RIFT_RP_VIEW;
        return true;
      }
      return true;   // no stray key navigates away mid-entry
    }

    if (c == RIFT_KEY_BACK || c == KEY_CANCEL) { _task->dismissOverlay(); return true; }

    ContactInfo* ct = contact();
    if (ct == NULL) { _task->dismissOverlay(); return true; }

    // One request at a time, because the radio has one pending slot: starting a
    // second would discard the first without saying so.
    bool busy = s.pending() != RIFT_REP_IDLE;
    uint32_t est = 0;

    if (c == 'l' || c == 'L') {
      if (!allowsLogin()) return true;   // not something this node type answers
      if (busy) { _task->showAlert("Still waiting", 1200); return true; }
      _edit.begin("", 31);
      _edit.mask = true;
      _mode = RIFT_RP_PASSWORD;
      return true;
    }
    if (c == 's' || c == 'S') {
      if (!allowsStats()) return true;   // not something this node type answers
      if (busy) { _task->showAlert("Still waiting", 1200); return true; }
      if (!the_mesh.riftStatusReq(*ct, est)) _task->showAlert("No packet free - try again", 1400);
      return true;
    }
    if (c == 't' || c == 'T') {
      if (busy) { _task->showAlert("Still waiting", 1200); return true; }
      if (!the_mesh.riftTelemetryReq(*ct, est)) _task->showAlert("No packet free - try again", 1400);
      return true;
    }
    if (c == 'c' || c == 'C') {
      if (!allowsCli()) return true;   // not something this node type answers
      if (busy) { _task->showAlert("Still waiting", 1200); return true; }
      // Guests can read stats, but the CLI is admin-only on the far side. Refusing
      // here means the airtime is not spent on a command that will be rejected.
      if (!s.isAdmin()) { _task->showAlert("Log in as admin first", 1600); return true; }
      _menu_sel = 0;
      _confirm_idx = -1;
      _mode = RIFT_RP_MENU;
      return true;
    }
    return true;   // the panel owns its keys; nothing navigates out but BACK
  }
};

// Nordic character picker: a long press on a base vowel offers its forms.
//
// An overlay, not a screen - the third use of that mechanism and the reason it
// exists. The compose line underneath keeps its half-typed text, stays on screen
// while you choose, and is handed straight back on dismissal.
//
// The base letter is already in the buffer, because the initial press inserted it
// normally - holding a key must not make ordinary typing feel delayed. Choosing a
// variant therefore replaces that character rather than appending to it.
class RiftNordicPickerScreen : public RiftScreen {
  UITask* _task;
  RiftCommsScreen* _comms;
  const char* _variants[RIFT_NORDIC_MAX_VARIANTS];
  int _count;
  int _sel;
  char _base;

public:
  RiftNordicPickerScreen(UITask* task, RiftCommsScreen* comms)
     : _task(task), _comms(comms), _count(0), _sel(0), _base(0) {
    for (int i = 0; i < RIFT_NORDIC_MAX_VARIANTS; i++) _variants[i] = NULL;
  }

  bool isOverlay() const override { return true; }

  // returns false if this key has no variants, so the caller knows not to open
  bool openFor(char base) {
    _count = riftNordicVariants(base, _variants);
    if (_count == 0) return false;
    _base = base;
    _sel = 0;
    return true;
  }

  int render(DisplayDriver& display) override {
    // Sits just above the compose line rather than centred, so it is next to the
    // text it is about and does not cover the history being replied to.
    const int cell = 26;
    const int w = _count * cell + 12;
    const int h = 40;
    const int x = 2;
    const int y = 158;

    display.setColor(rift_pal.bg);
    display.fillRect(x, y, w, h);
    // A 2px frame in rule (September design round): a box says "on top" by being
    // a box, where the accent said "act on all of this". Two nested rects, since
    // the driver has no line width.
    display.setColor(rift_pal.rule);
    display.drawRect(x, y, w, h);
    display.drawRect(x + 1, y + 1, w - 2, h - 2);

    display.setTextSize(1);
    display.setColor(rift_pal.dim);
    display.drawTextLeftAlign(x + 6, y + 4, "ENTER  BKSP");

    for (int i = 0; i < _count; i++) {
      int cx = x + 6 + i * cell;
      if (i == _sel) {
        // accent fill with the glyph in on_accent. Legal in both palettes for the
        // reason accent-as-text is not, which is contrast - and white was not
        // legal either, which is that same reason read the other way round.
        display.setColor(rift_pal.accent);
        display.fillRect(cx - 2, y + 14, cell - 2, 22);
        display.setColor(rift_pal.on_accent);
      } else {
        display.setColor(rift_pal.fg);
      }
      // the variant is UTF-8; translate it to draw it
      char glyph[8];
      riftTranslateUTF8(glyph, _variants[i], sizeof(glyph));
      display.setTextSize(3);
      display.drawTextLeftAlign(cx, y + 16, glyph);
      display.setTextSize(1);
    }

    return 1000;
  }

  bool handleInput(char c) override {
    if (c == KEY_RIGHT || c == KEY_NEXT) { _sel = (_sel + 1) % _count; return true; }
    if (c == KEY_LEFT || c == KEY_PREV)  { _sel = (_sel + _count - 1) % _count; return true; }

    if (c == KEY_ENTER) {
      // Both taps are already in the line - they were inserted as ordinary
      // letters - so the variant replaces the pair.
      if (_comms != NULL) {
        _comms->deleteLastChar();
        _comms->deleteLastChar();
        if (!_comms->insertUtf8(_variants[_sel])) {
          // put back what was typed rather than silently losing characters. A
          // two-byte variant can fail to fit where two one-byte letters did.
          char pair[3] = { _base, _base, 0 };
          _comms->insertUtf8(pair);
          _task->showAlert("No room for that character", 1200);
        }
      }
      _task->dismissOverlay();
      return true;
    }

    // cancel leaves both letters exactly as typed, which is what makes a false
    // trigger on a real double vowel cost one keypress
    if (c == RIFT_KEY_BACK || c == KEY_CANCEL) {
      _task->dismissOverlay();
      return true;
    }
    return false;
  }
};

// Result of a zero-hop repeater discovery round.
//
// An overlay, so the home screen keeps answering its own question underneath and
// gets handed back untouched. Fourth use of that mechanism.
//
// The two SNR columns are the point of the feature. `rx` is how well we heard the
// response; `tx` is the SNR the repeater reported for our request. Nothing else in
// this firmware can show the second one - adverts only ever tell us the inbound
// half - and an asymmetric link is exactly the thing worth knowing before you rely
// on a route.
class RiftDiscoverScreen : public RiftScreen {
  UITask* _task;

public:
  RiftDiscoverScreen(UITask* task) : _task(task) { }

  bool isOverlay() const override { return true; }

  int render(DisplayDriver& display) override {
    const int x = 6, w = display.width() - 12;
    const int y = 22, h = 186;
    const int inner = w - 12;

    display.setColor(rift_pal.bg);
    display.fillRect(x, y, w, h);
    // A 2px frame in rule (September design round): a box says "on top" by being
    // a box, where the accent said "act on all of this". Two nested rects, since
    // the driver has no line width.
    display.setColor(rift_pal.rule);
    display.drawRect(x, y, w, h);
    display.drawRect(x + 1, y + 1, w - 2, h - 2);

    display.setTextSize(1);
    display.setColor(rift_pal.accent);
    display.drawTextLeftAlign(x + 6, y + 5, "0-HOP REPEATERS");

    char tmp[48];
    int n = the_mesh.getDiscoveredCount();
    bool live = the_mesh.isDiscovering();

    // Say which state it is in. Responses arrive over seconds by design - repeaters
    // answer after a widened random delay because many reply at once - so an empty
    // list one second in means nothing yet, and must not read as "none found".
    display.setColor(rift_pal.mid);
    if (live) snprintf(tmp, sizeof(tmp), "listening %us", (unsigned) (the_mesh.discoveryElapsedMs() / 1000));
    else      snprintf(tmp, sizeof(tmp), "%d found", n);
    display.drawTextRightAlign(x + w - 6, y + 5, tmp);

    // column headings for the two directions
    display.setColor(rift_pal.dim);
    display.drawTextRightAlign(x + inner - 46, y + 20, "rx");
    display.drawTextRightAlign(x + inner + 6, y + 20, "tx");

    int row_y = y + 34;
    for (int i = 0; i < n && row_y < y + h - 20; i++) {
      const MyMesh::DiscoveredRepeater* d = the_mesh.getDiscovered(i);
      if (d == NULL) continue;

      // Name it if we know it. A repeater can answer a discovery without ever
      // having sent us an advert, so there may be no name at all - then the key
      // prefix is what we honestly have.
      ContactInfo* c = the_mesh.lookupContactByPubKey((uint8_t*) d->pubkey, PUB_KEY_SIZE);
      char label[32];
      if (c != NULL && c->name[0] != 0) {
        riftTranslateUTF8(label, c->name, sizeof(label));
      } else {
        snprintf(label, sizeof(label), "%02X%02X%02X%02X...",
                 d->pubkey[0], d->pubkey[1], d->pubkey[2], d->pubkey[3]);
      }
      display.setColor(rift_pal.fg);
      display.drawTextEllipsized(x + 6, row_y, inner - 100, label);

      // SNR arrives as a signed value times four, from both sides
      display.setColor(rift_pal.mid);
      snprintf(tmp, sizeof(tmp), "%.1f", d->snr_we_heard_them / 4.0f);
      display.drawTextRightAlign(x + inner - 46, row_y, tmp);
      snprintf(tmp, sizeof(tmp), "%.1f", d->snr_they_heard_us / 4.0f);
      display.drawTextRightAlign(x + inner + 6, row_y, tmp);

      row_y += RIFT_LINE_H;
    }

    if (n == 0) {
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(x + 6, y + 34,
        live ? "waiting for replies" : "no repeater answered");
    }

    display.setColor(rift_pal.dim);
    display.drawTextLeftAlign(x + 6, y + h - 14,
      live ? "BKSP dismiss - keeps listening" : "BKSP dismiss");

    return live ? 500 : 1000;
  }

  bool handleInput(char c) override {
    if (c == RIFT_KEY_BACK || c == KEY_CANCEL || c == KEY_ENTER) {
      // Dismissing does not cancel the round. The window stays open in MyMesh, so
      // late replies are still collected and reopening the panel shows them.
      _task->dismissOverlay();
      return true;
    }
    return false;
  }
};

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _display = display;
  _sensors = sensors;
  _auto_off = millis() + AUTO_OFF_MILLIS;

#if defined(PIN_USER_BTN)
  user_btn.begin();
#endif
#if defined(PIN_USER_BTN_ANA)
  analog_btn.begin();
#endif
#ifdef RIFT_INPUT_TRACKBALL
  RIFT_MARK("ui0");
  rift_trackball.begin();
#endif
#ifdef RIFT_INPUT_KEYBOARD
  RIFT_MARK("ball");
  rift_keyboard.begin();
#endif
#ifdef RIFT_INPUT_TOUCH
  RIFT_MARK("kbd");
  rift_touch.begin();
#endif
#ifdef RIFT_SPEAKER
  // Unverified pins - see platformio.ini. begin() returning false is reported on
  // SYSTEM rather than being silently ignored, because "no sound" has two very
  // different causes and only one of them is worth debugging in the driver.
  rift_speaker.begin(PIN_SPK_BCLK, PIN_SPK_LRCLK, PIN_SPK_DOUT);
  RIFT_MARK("spk");
#endif

  _node_prefs = node_prefs;

  if (_display != NULL) {
    _display->turnOn();
  }

#ifdef PIN_BUZZER
  buzzer.begin();
  buzzer.quiet(_node_prefs->buzzer_quiet);
  buzzer.startup();
#endif

#ifdef PIN_VIBRATION
  vibration.begin();
#endif

  ui_started_at = millis();
  _alert_expiry = 0;
  _alert[0] = 0;      // the text is what says whether an alert is live

  // Before any screen exists, so COMMS and the popup open with the history
  // already in place rather than filling in a moment later.
  riftLoadSettings();   // before the first render, so the palette is right at boot
  msg_log.load(RIFT_MSGLOG_PATH);
  // Builds before 0.5.0 wrote to a temporary and renamed it over the live file.
  // A device interrupted mid-save still has that temporary occupying a
  // filesystem which also holds the identity and has little room to spare.
  if (SPIFFS.exists(RIFT_MSGLOG_TMP)) SPIFFS.remove(RIFT_MSGLOG_TMP);
  RIFT_MARK("msgs");

  splash = new RiftSplashScreen(this);
  msg_preview = new RiftMsgPreviewScreen(this);
  nav_screens[RIFT_NAV_MESH] = new RiftMeshScreen(this, node_prefs);
  nav_screens[RIFT_NAV_NODES] = new RiftConstellationScreen(this);
#ifdef RIFT_RADAR
  nav_screens[RIFT_NAV_RADAR] = new RiftRadarScreen(this);
#else
  nav_screens[RIFT_NAV_RADAR] = new RiftPlaceholderScreen(this, RIFT_NAV_RADAR, "RF RADAR", "Wi-Fi / BLE / RF scan");
#endif
  RiftCommsScreen* comms = new RiftCommsScreen(this);
  nav_screens[RIFT_NAV_COMMS] = comms;
  nav_screens[RIFT_NAV_SYSTEM] = new RiftSystemScreen(this);
  // holds the concrete type: the picker edits the compose line directly, which is
  // the whole point of it
  nordic_picker = new RiftNordicPickerScreen(this, comms);
  discover_overlay = new RiftDiscoverScreen(this);
  rename_watch = new RiftRenameWatchScreen(this);
  repeater_panel = new RiftRepeaterScreen(this);
  nav_idx = 0;
  setCurrScreen(splash);
}

// Raised by holding a base vowel while COMMS is composing. Returns quietly for
// any other key, so the caller can offer every keypress without filtering first.
void UITask::openNordicPicker(char base) {
  if (nordic_picker == NULL || _overlay != NULL) return;
  if (curr != nav_screens[RIFT_NAV_COMMS]) return;   // nowhere else has a text field
  // and not while COMMS has the target picker up: the keystroke went there, not
  // into the compose line, so there is no base letter to replace
  if (!((RiftCommsScreen *) nav_screens[RIFT_NAV_COMMS])->acceptsText()) return;
  if (!((RiftNordicPickerScreen *) nordic_picker)->openFor(base)) return;
  pushOverlay(nordic_picker);
}

void UITask::cycleNavScreen(int dir) {
  nav_idx = (nav_idx + dir + RIFT_NAV_COUNT) % RIFT_NAV_COUNT;
  setCurrScreen(nav_screens[nav_idx]);
}

void UITask::showAlert(const char* text, int duration_millis) {
  StrHelper::strncpy(_alert, text, sizeof(_alert));
  _alert_expiry = millis() + duration_millis;
}

void UITask::notify(UIEventType t) {
#ifdef RIFT_SPEAKER
  // This board has no buzzer, so the block below compiled to nothing and every one
  // of these events was silent. MyMesh does raise them - contactMessage from a
  // direct message, channelMessage from a channel, newContactMessage from a sender
  // not yet in the book - they simply had nowhere to go.
  switch (t) {
    case UIEventType::contactMessage:
    case UIEventType::newContactMessage:
      riftPlay(SND_DM, (int) (sizeof(SND_DM) / sizeof(SND_DM[0])), SND_GAIN_MSG);
      break;
    case UIEventType::channelMessage:
      riftPlay(SND_CHAN, (int) (sizeof(SND_CHAN) / sizeof(SND_CHAN[0])), SND_GAIN_MSG);
      break;
    default:
      break;
  }
#endif

#if defined(PIN_BUZZER)
switch(t){
  case UIEventType::contactMessage:
    buzzer.play("MsgRcv3:d=4,o=6,b=200:32e,32g,32b,16c7");
    break;
  case UIEventType::channelMessage:
    buzzer.play("kerplop:d=16,o=6,b=120:32g#,32c#");
    break;
  case UIEventType::ack:
    buzzer.play("ack:d=32,o=8,b=120:c");
    break;
  case UIEventType::roomMessage:
  case UIEventType::newContactMessage:
  case UIEventType::none:
  default:
    break;
}
#endif

#ifdef PIN_VIBRATION
  if (t != UIEventType::none) {
    vibration.trigger();
  }
#endif
}

void UITask::startDirectMessage(const uint8_t* key6) {
  ((RiftCommsScreen *) nav_screens[RIFT_NAV_COMMS])->setDirectTarget(key6);
  nav_idx = RIFT_NAV_COMMS;
  setCurrScreen(nav_screens[RIFT_NAV_COMMS]);
}

// Called when the companion app drains its queue. The count is
// offline_queue_len - how far behind a connected phone is - which says nothing
// about what has been read on the device. Driving the nav dot from it lit the
// indicator for messages already read here, and gotoHomeScreen() let a phone
// syncing in the background navigate the terminal out from under its user.
void UITask::msgRead(int msgcount) {
  _companion_backlog = msgcount;
}

void UITask::newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) {
  newMsgConv(path_len, from_name, text, msgcount, RIFT_CONV_UNKNOWN, 0, NULL);
}

void UITask::newMsgConv(uint8_t path_len, const char* from_name, const char* text,
                        int msgcount, uint8_t conv_kind, uint8_t channel_idx,
                        const uint8_t* peer) {
  _companion_backlog = msgcount;

  RiftConvKey conv = riftConvUnknown();
  // The fingerprint is resolved here rather than passed across the boundary: MyMesh
  // hands over primitives so AbstractUITask stays independent of any one UI, and the
  // channel table is equally reachable from this side.
  if (conv_kind == RIFT_CONV_CHANNEL) conv = riftChannelConv(channel_idx);
  else if (conv_kind == RIFT_CONV_DM) conv = riftConvDM(peer);

  char origin[62];
  if (path_len == 0xFF) {
    sprintf(origin, "(D) %s:", from_name);
  } else {
    // path_len is Packet's encoding, not a count - see riftHopCount(). Printed
    // raw, a two-hop route at the 2-byte hash setting showed as "(66)".
    sprintf(origin, "(%d) %s:", (int) riftHopCount(path_len), from_name);
  }
  msg_log.add(rtc_clock.getCurrentTime(), conv, origin, text, false);
  // The text as well as the header. The message history already holds it, but the
  // log is where it sits next to the advert that preceded it and the ack that
  // followed, and that ordering is the thing the history cannot show.
  // path_len 0xFF means "no path recorded", and riftHopCount masks the low six bits,
  // so a direct message logged as "63h" - next to an origin that already said "(D)".
  // A diagnostic that knowingly misreports protocol metadata is worse than none.
  if (path_len == 0xFF) {
    riftLogf("rx direct %s: %s", origin, text);
  } else {
    riftLogf("rx %dh %s: %s", (int) riftHopCount(path_len), origin, text);
  }
  // Marked before the popup decision, and cleared again by COMMS on the next frame
  // if this is the conversation on screen. Marking unconditionally and letting the
  // frame retract it keeps one rule - what is displayed is read - rather than two
  // that have to agree.
  msg_unread.mark(conv);


  // Don't take the screen away from someone mid-input: a half-typed line in
  // COMMS, a channel name being entered, or a one-time key being read. Each
  // screen answers for itself now - this used to name COMMS and SYSTEM here and
  // reach into the latter to ask.
  //
  // Nothing stacks: a second message while the preview is already up must leave
  // it where it is, or dismissing would land back on the popup.
  //
  // The second condition is new, and it is what the history filter forced. COMMS
  // used to be able to say "always already showing it"; now it shows one
  // conversation, so a message arriving in a different one has to announce itself
  // even though COMMS is on screen. showsConversation() defaults to false, so every
  // other screen behaves exactly as before.
  if (_overlay == NULL && curr != NULL && !curr->isModal()
      && !curr->showsConversation(conv)) {
    pushOverlay(msg_preview);
  }

  if (_display != NULL) {
    // The only notification this board has. There is no sounder and no vibration
    // motor in this variant, so notify() compiles away to nothing - a message
    // arriving while the screen is dark has exactly one way to announce itself, and
    // this line is it.
    //
    // It used to be suppressed while hasConnection() was true, on the reasoning that
    // a companion app was attached and the phone would be notifying. That reasoning
    // was wrong, and it is the same wrong as isExternalPowered() earlier in this
    // project: hasConnection() is _serial->isConnected(), and with ENABLE_USB_INTERFACE
    // that is the USB CDC state - true as soon as any host opens the port. A PC used
    // for flashing, a serial monitor, a charger that enumerates: all of them made it
    // true with no companion app anywhere near the device, and the notification the
    // user cared about most was silently switched off.
    //
    // The costs are not symmetric. A wake that was not needed costs a lit screen. A
    // wake that was needed and suppressed costs the message. So it always wakes, and
    // the link state is logged rather than acted on - if a phone really is attached
    // and notifying, the line in the log says so and the duplicate is visible.
    if (!_display->isOn()) {
      _display->turnOn();
      if (rift_msg_wakes < 0xFFFF) rift_msg_wakes++;
      rift_last_wake_ms = (uint32_t) millis();
      riftLogf("wake: screen on%s", hasConnection() ? " (link up)" : "");
    }
    if (_display->isOn()) {
      _auto_off = millis() + AUTO_OFF_MILLIS;
      refreshNow();
    }
  }
}

void UITask::proximityAlert(const char* name, bool is_wifi) {
  char msg[48];
  snprintf(msg, sizeof(msg), "NEAR: %s", (name != NULL && name[0]) ? name : "device");
  // Longer than an ordinary alert: this one is the point of the feature, and the
  // user may not have been looking at the screen when it appeared.
  showAlert(msg, 5000);
#ifdef RIFT_SPEAKER
  // A different pattern from a message: this is about the world rather than
  // something addressed to you, and telling them apart without looking is the point.
  riftPlay(SND_PROX, (int) (sizeof(SND_PROX) / sizeof(SND_PROX[0])), SND_GAIN_PROX);
#endif
  riftLogf("watch NEAR %s %s", is_wifi ? "wifi" : "ble", name);
  if (_display != NULL) {
    // The screen coming on is the whole notification, as it is for a message.
    if (!_display->isOn()) _display->turnOn();
    _auto_off = millis() + AUTO_OFF_MILLIS;
    refreshNow();
  }
}

void UITask::msgDelivered(uint32_t ack_hash, uint32_t trip_time_millis) {
  ((RiftCommsScreen *) nav_screens[RIFT_NAV_COMMS])->onDelivered(ack_hash, trip_time_millis);
  refreshNow();   // reflect the new delivery state promptly
}

void UITask::userLedHandler() {
#ifdef PIN_STATUS_LED
  int cur_time = millis();
  if (cur_time > next_led_change) {
    if (led_state == 0) {
      led_state = 1;
      if (_msgcount > 0) {
        last_led_increment = LED_ON_MSG_MILLIS;
      } else {
        last_led_increment = LED_ON_MILLIS;
      }
      next_led_change = cur_time + last_led_increment;
    } else {
      led_state = 0;
      next_led_change = cur_time + LED_CYCLE_MILLIS - last_led_increment;
    }
    digitalWrite(PIN_STATUS_LED, led_state == LED_STATE_ON);
  }
#endif
}

// The single funnel every screen change passes through. It used to answer "is
// this real navigation?" twice here and a third time in newMsg(), each with its
// own expression and its own downcast to a concrete screen class - RADAR for the
// RF teardown, SYSTEM for the secret wipe. Both of those are now the screens'
// own onEnter/onLeave, and the rule lives in riftScreenTransition() where it can
// be tested.
void UITask::setCurrScreen(RiftScreen* c) {
  if (c == NULL) return;

  int xn = riftScreenTransition(curr == c,
                               curr != NULL && curr->isOverlay(),
                               c->isOverlay());

  if ((xn & RIFT_XN_LEAVE) && curr != NULL) curr->onLeave();

  curr = c;
  if (xn & RIFT_XN_ENTER) c->onEnter();

  refreshNow();
}

// A popup over the current screen. curr and nav_idx are untouched, so there is
// nothing to restore on dismissal and the screen underneath is never told it was
// left - which is what made the RADAR and SYSTEM guards necessary before.
void UITask::pushOverlay(RiftScreen* o) {
  if (o == NULL) return;
  _overlay = o;
  refreshNow();
}

void UITask::dismissOverlay() {
  _overlay = NULL;
  refreshNow();
}

// Starts a zero-hop repeater discovery and shows the result panel. Refuses rather
// than restarting a round already in progress: the tag identifies the round, so a
// restart would discard replies still arriving from the first one.
void UITask::startRepeaterDiscovery() {
  if (the_mesh.isDiscovering()) {
    if (discover_overlay != NULL) pushOverlay(discover_overlay);
    return;
  }
  if (!the_mesh.startRepeaterDiscovery()) {
    showAlert("No packet free - try again", 1400);
    return;
  }
  notify(UIEventType::ack);
  if (discover_overlay != NULL) pushOverlay(discover_overlay);
}

// Rename a watched RF device. Refuses quietly when the index is not a live watch,
// so the caller can offer the key without checking first.
void UITask::openRenameWatch(int watch_idx) {
  if (rename_watch == NULL || _overlay != NULL) return;
  if (!((RiftRenameWatchScreen *) rename_watch)->openFor(watch_idx)) return;
  pushOverlay(rename_watch);
}

// Repeater control.
//
// Every path out of here either opens the panel or says why. It refused quietly
// before, which is indistinguishable from a keypress that was never registered -
// and that is exactly how the feature read on the device: nothing happened, and
// nothing said anything.
void UITask::openRepeaterPanel(const uint8_t* pub_key) {
  if (repeater_panel == NULL) {
    showAlert("Repeater panel unavailable", 1600);
    return;
  }
  if (_overlay != NULL) {
    // Logged rather than ignored: a stuck overlay is invisible from here, and it
    // makes every later press do nothing for a reason nobody can see.
    riftLogf("repeater panel: overlay already up");
    return;
  }
  if (!((RiftRepeaterScreen *) repeater_panel)->openFor(pub_key)) {
    // openFor only fails when the node is not a contact, which is a heard node
    // that was never added - a real and common state on a busy mesh.
    riftLogf("repeater panel: not a contact");
    showAlert("Not a contact yet", 1600);
    return;
  }
  riftLogf("repeater panel opened");
  pushOverlay(repeater_panel);
}

void UITask::gotoCommsScreen() {
  dismissOverlay();
  nav_idx = RIFT_NAV_COMMS;
  setCurrScreen(nav_screens[RIFT_NAV_COMMS]);
}

/*
  hardware-agnostic pre-shutdown activity should be done here
*/
void UITask::shutdown(bool restart){

  // A clean shutdown has no reason to lose the last few messages, so the
  // debounce is skipped here. This is also the only path that makes the loss
  // window in RIFT_MSGLOG_FLUSH_MILLIS apply solely to power being pulled.
  if (msg_log.dirty) msg_log.save(RIFT_MSGLOG_PATH);

  #ifdef PIN_BUZZER
  buzzer.shutdown();
  uint32_t buzzer_timer = millis();
  while (buzzer.isPlaying() && (uint32_t) (millis() - buzzer_timer) < 2500)
    buzzer.loop();

  #endif // PIN_BUZZER

  if (restart) {
    _board->reboot();
  } else {
    _board->powerOff();
  }
}

bool UITask::isButtonPressed() const {
#ifdef PIN_USER_BTN
  return user_btn.isPressed();
#else
  return false;
#endif
}

void UITask::loop() {
  char c = 0;
#if UI_HAS_JOYSTICK
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_ENTER);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_ENTER);
  }
  ev = joystick_left.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_LEFT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_LEFT);
  }
  ev = joystick_right.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_RIGHT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_RIGHT);
  }
  ev = back_btn.check();
  if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
    c = handleTripleClick(KEY_SELECT);
  }
#elif defined(PIN_USER_BTN)
  int ev = user_btn.check();
  // A trackball with a click button should select, not page - so click is Enter,
  // the same as the keyboard. Screen changes are already covered by rolling the
  // trackball left/right and by tapping the nav bar, which is what freed this up.
  // Double-click stays "previous screen": as a back action it would do nothing
  // on most screens, and it keeps a navigation route that doesn't need the touch
  // panel or a precise roll.
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_ENTER);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_ENTER);
  } else if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
    c = handleDoubleClick(KEY_PREV);
  } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
    c = handleTripleClick(KEY_SELECT);
  }
#endif
#if defined(UI_HAS_ROTARY_INPUT)
  RotaryInputEvent rotaryEv = rotary_input.poll();
  if (c == 0 && _display != NULL && _display->isOn()) {
    if (rotaryEv == RotaryInputEvent::Next) {
      c = KEY_NEXT;
    } else if (rotaryEv == RotaryInputEvent::Prev) {
      c = KEY_PREV;
    }
  }
#endif
#if defined(PIN_USER_BTN_ANA)
  if (abs(millis() - _analogue_pin_read_millis) > 10) {
    int ev = analog_btn.check();
    if (ev == BUTTON_EVENT_CLICK) {
      c = checkDisplayOn(KEY_NEXT);
    } else if (ev == BUTTON_EVENT_LONG_PRESS) {
      c = handleLongPress(KEY_ENTER);
    } else if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
      c = handleDoubleClick(KEY_PREV);
    } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
      c = handleTripleClick(KEY_SELECT);
    }
    _analogue_pin_read_millis = millis();
  }
#endif
#ifdef RIFT_INPUT_TRACKBALL
  if (c == 0) {
    char tb = rift_trackball.poll();
    if (tb != 0) c = checkDisplayOn(tb);
  }
#endif
#ifdef RIFT_INPUT_KEYBOARD
  if (c == 0) {
    char key = rift_keyboard.poll();
    if (key != 0) c = checkDisplayOn(key);
  }

#endif
  // The screen that was showing when the key was pressed. Touch is polled after
  // the keyboard and can navigate elsewhere, so dispatching to curr afterwards
  // delivered the keystroke to a screen the user was not looking at when they
  // typed it - an ENTER meant for COMMS arriving at whatever the nav-bar tap had
  // just selected.
  // An overlay owns the keyboard while it is up, and it is captured here for the
  // same reason curr was: touch is polled below and can navigate, so resolving
  // the target afterwards delivered the keystroke to something else.
  RiftScreen* key_target = (_overlay != NULL) ? _overlay : curr;
  // Captured alongside it so the dispatch below can tell whether touch handling
  // moved the ground underneath the keystroke. Capturing key_target stopped a key
  // being delivered to a screen the tap had just navigated *to*; it did not stop
  // the mirror case, where a nav-bar tap calls dismissOverlay() and the keystroke
  // is then handed to an overlay the user can no longer see - so an ENTER meant
  // for the popup, or for the new screen, is silently eaten by a dismissed one.
  RiftScreen* key_overlay_before = _overlay;
  RiftScreen* key_curr_before = curr;

#ifdef RIFT_INPUT_TOUCH
  {
    // Drag first: the driver tracks the finger continuously and only reports a
    // tap on release, so movement has to be read from the live position. A tap
    // that moved is a scroll and must not also fire as a tap; the release below
    // decides that from the driver's travel count and from _drag_moved.
    if (rift_touch.isDown()) {
      if (!_dragging) {
        _dragging = true;
        _drag_moved = false;
        _drag_last_y = rift_touch.lastY();
        rift_touch.dragReset();
        _drag_applied = 0;
      } else {
        int dy = rift_touch.lastY() - _drag_last_y;
        if (dy != 0) {
          RiftScreen* t = (_overlay != NULL) ? _overlay : curr;
          if (t != NULL && t->handleDrag(dy)) {
            _drag_applied += dy;
            _drag_moved = true;
            _auto_off = millis() + AUTO_OFF_MILLIS;
            refreshNow();
          }
          _drag_last_y = rift_touch.lastY();
        }
      }
    } else {
      if (_dragging) {
        // One line per drag, on release, because the four things that could be
        // wrong here look identical from the outside and need different fixes:
        //
        //   travel much larger than applied  -> the UI is dropping movement
        //   max step far above the rest      -> the panel reported another contact
        //   few samples over a long drag     -> the frame is starving the sampler
        //   applied not matching the view    -> a clamp is moving it afterwards
        //
        // Reasoning could not separate them; this can.
        if (rift_touch.dragSamples() > 1) {
          riftLogf("drag %us %dtr %dmax %dap", (unsigned) rift_touch.dragSamples(),
                   rift_touch.dragTravel(), rift_touch.dragMaxStep(), _drag_applied);
        }
      }
      _dragging = false;
    }

    int tx, ty;
    if (rift_touch.poll(tx, ty)) {
      // A drag must not also arrive as a tap.
      //
      // This was an `else` in front of an unbraced if-chain, so it guarded the
      // single assignment on the next line and nothing else - the tap handling
      // below ran on every release. In COMMS that meant every drag ended with a
      // tap-to-page, and a release in the upper half of the history did
      // `_scroll += page`, which is the jump backwards away from the bottom.
      // The scroll wheel had no such problem because it raises no tap.
      //
      // Braced, and then found to be the wrong test. _drag_moved says a screen
      // consumed movement, and every cursor list declines movement that produces
      // no step - the cursor already on the last row, a drag shorter than a pitch
      // - so a long drag that changed nothing was still a tap on release, and
      // SYSTEM activated the row the finger lifted from. Which was "Delete
      // channel" once, and "Advert mesh" once.
      //
      // So the finger's own travel decides, from the driver's count for this
      // contact, with a few pixels of slop for a finger that does not lift straight
      // up. _drag_moved stays as the other half: COMMS scrolls by the pixel and
      // consumes a jitter smaller than the slop, and that release must not page.
      // The rule is riftDragIsMove() in RiftLogic.h, where it is tested.
      bool was_drag = _drag_moved
                   || riftDragIsMove(rift_touch.dragTravel(), RIFT_TAP_SLOP_PX);
      _drag_moved = false;
      if (was_drag) {
        // nothing: the gesture was a drag, and the screen has already had it
      } else {
      _touch_x = tx;   // kept for the SYSTEM readout while calibrating
      _touch_y = ty;

      if (_display != NULL && !_display->isOn()) {
        checkDisplayOn(0);          // first tap just wakes the screen
      } else if (ty >= _display->height() - 16) {
        // nav bar: jump straight to the tapped screen
        int col = (tx * RIFT_NAV_COUNT) / _display->width();
        if (col >= 0 && col < RIFT_NAV_COUNT) {
          // the nav bar stays live behind a popup, and using it puts the popup
          // away - navigating somewhere is an answer to it
          dismissOverlay();
          nav_idx = col;
          setCurrScreen(nav_screens[nav_idx]);
        }
        _auto_off = millis() + AUTO_OFF_MILLIS;
      } else if (_overlay != NULL) {
        _overlay->handleTouch(tx, ty);
        _auto_off = millis() + AUTO_OFF_MILLIS;
        refreshNow();
      } else if (curr) {
        curr->handleTouch(tx, ty);
        _auto_off = millis() + AUTO_OFF_MILLIS;
        refreshNow();
      }
      }   // closes the "not a drag" branch above
    }
  }
#endif
#if defined(BACKLIGHT_BTN)
  if (riftDue((uint32_t) millis(), next_backlight_btn_check)) {
    bool touch_state = digitalRead(PIN_BUTTON2);
#if defined(DISP_BACKLIGHT)
    digitalWrite(DISP_BACKLIGHT, !touch_state);
#elif defined(EXP_PIN_BACKLIGHT)
    expander.digitalWrite(EXP_PIN_BACKLIGHT, !touch_state);
#endif
    next_backlight_btn_check = millis() + 300;
  }
#endif

  if (c != 0) _last_key = (int) (unsigned char) c;
  // Dropped rather than delivered if touch changed what is on screen during this
  // same iteration. The keystroke was aimed at what the user was looking at, and
  // that is now gone; neither the old target nor the new one can be said to be
  // what they meant. Losing one keypress in a window of a few milliseconds costs
  // less than performing its action on the wrong screen - and the SYSTEM readout
  // still records it, so a key that vanishes this way is not invisible.
  bool nav_moved = (_overlay != key_overlay_before) || (curr != key_curr_before);
  if (c != 0 && key_target && !nav_moved) {
    key_target->handleInput(c);
    _auto_off = millis() + AUTO_OFF_MILLIS;
    refreshNow();
  } else if (c != 0 && nav_moved) {
    refreshNow();   // the tap that moved us still deserves an immediate repaint
  }

  userLedHandler();

#ifdef PIN_BUZZER
  if (buzzer.isPlaying())  buzzer.loop();
#endif

  // Tropo openings, announced from here rather than from the packet path: that
  // path gets a counter and nothing else. Edge-triggered on both sides, so one
  // opening is one line and one alert however many deep packets arrive.
  {
    RiftTropo& tr = riftTropoState();
    riftTropoTick(&tr, (uint32_t) millis());
    if (tr.active != _tropo_was_active) {
      _tropo_was_active = tr.active;
      if (tr.active) {
        riftLogf("TROPO open: %d deep, peak %d hops", (int) tr.deep_count, (int) tr.peak_hops);
        notify(UIEventType::ack);
        showAlert("Tropo conditions", 2500);
      } else {
        riftLogf("TROPO closed");
      }
    }
  }

  // Serviced here rather than from the panel's render, for the same reason RADAR
  // is: a state machine that only advances while it is on screen stops advancing
  // when the panel is closed. A request left outstanding then held its pending
  // state until the panel was reopened, and the first frame after that reported a
  // timeout that had really expired minutes earlier.
  if (riftRepeater().checkTimeout()) riftLogf("repeater: no answer");

#ifdef RIFT_RADAR
  // serviced unconditionally, not via curr->poll(), so RF teardown still runs
  // after navigating away from RADAR
  if (nav_screens[RIFT_NAV_RADAR] != NULL) ((RiftRadarScreen *) nav_screens[RIFT_NAV_RADAR])->service();
#endif

#ifdef RIFT_SPEAKER
  // Before the screen work, and it matters less than it used to: the DMA queue now
  // holds a whole alert, so a tone survives a long pass rather than depending on
  // this being called often. Still first, because the call is bounded by a zero
  // timeout and getting the audio queued before a 153 KB redraw is free.
  rift_speaker.loop();
  // The per-tone timeline that found the amplifier fault is no longer logged.
  // It was a line per notification in a log kept for mesh events, and the health
  // figure it existed to expose - passes per tone - is on the SYSTEM readings
  // page now. takeEvent() stays: it costs nothing idle and is one line away if
  // this needs watching again.
#endif

  if (curr) curr->poll();
  if (_overlay != NULL) _overlay->poll();

  // Debounced write of the message history. Deliberately not on every message:
  // a conversation arrives as a burst, this collapses the burst into one write,
  // and the filesystem being written to is the one holding the unrecoverable
  // private key. The timer restarts on each new message, so it fires once the
  // traffic settles rather than every 20 seconds during it.
  if (msg_log.dueToSave(millis())) {
    msg_log.save(RIFT_MSGLOG_PATH);
  }
  if (riftDue((uint32_t) millis(), _next_state_check)) {
    _next_state_check = (uint32_t) millis() + 4000;

    // In here rather than every pass through the loop. It sweeps up to 48 entries,
    // which is cheap but is still work in a loop that shares the SPI bus with the
    // radio, and a timeout reported within four seconds is as precise as anyone
    // needs it to be.
    msg_log.logTimeouts();

    if (hasGPSHardware()) {
      LocationProvider* nmea = sensors.getLocationProvider();
      bool fix = (nmea != NULL) && nmea->isValid();
      if (fix != _gps_had_fix) {
        _gps_had_fix = fix;
        if (fix) riftLogf("GPS fix, %ld sat", (long) nmea->satellitesCount());
        else     riftLogf("GPS fix lost");
      }
    }

    // Buckets rather than readings. A line every four seconds would bury everything
    // else in the ring, and only crossing downward is news - a brownout otherwise
    // leaves LAST RESET saying a reset happened and nothing saying why.
    uint16_t mv = getBattMilliVolts();
    if (mv > 0) {
      int pct = ((int) mv - BATT_MIN_MILLIVOLTS) * 100
                / (BATT_MAX_MILLIVOLTS - BATT_MIN_MILLIVOLTS);
      int8_t bucket = (pct <= 5) ? 0 : ((pct <= 10) ? 1 : ((pct <= 20) ? 2 : 3));
      if (_batt_bucket < 0) {
        _batt_bucket = bucket;          // first reading is the baseline, not an event
      } else if (bucket != _batt_bucket) {
        bool falling = bucket < _batt_bucket;
        _batt_bucket = bucket;
        // charging back up is tracked so the next fall is reported again, but it is
        // not itself worth a line
        if (falling) riftLogf("battery %d%% (%umV)", pct < 0 ? 0 : pct, (unsigned) mv);
      }
    }
  }

  // A screen dump was asked for: put the requested screen up, wake the panel if it
  // is asleep, and force a redraw. The frame is streamed after endFrame() below.
  bool dump_now = false;
  if (s_dump_pending && _display != NULL) {
    int nav = s_dump_nav;
    if (nav >= 0 && nav < RIFT_NAV_COUNT && nav != nav_idx) {
      dismissOverlay();
      nav_idx = nav;
      setCurrScreen(nav_screens[nav_idx]);
    }
    if (!_display->isOn()) _display->turnOn();
    _auto_off = millis() + AUTO_OFF_MILLIS;
    refreshNow();
    dump_now = true;
    s_dump_pending = false;
  }

  if (_display != NULL && _display->isOn()) {
    if (riftDue((uint32_t) millis(), _next_refresh) && curr) {
      // Once per frame, for the nav bar to draw. The title bar used to read the
      // ADC inside its own render, so this is no more work - just in one place.
      {
        int mv = (int) getBattMilliVolts();
        int pct = ((mv - BATT_MIN_MILLIVOLTS) * 100) / (BATT_MAX_MILLIVOLTS - BATT_MIN_MILLIVOLTS);
        rift_nav_batt_pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
      }
      _display->startFrame();
      int delay_millis = curr->render(*_display);
      // Popup, then alert box: three layers into one canvas, one bulk transfer
      // out of endFrame(). The overlay's cadence wins while it is up, since it is
      // what the user is looking at.
      if (_overlay != NULL) {
        int od = _overlay->render(*_display);
        if (od > 0) delay_millis = od;
      }
      // _alert[0] rather than _alert_expiry != 0. Zero was the "no alert"
      // sentinel, and a wrap-safe difference cannot express it: !riftDue(now, 0)
      // is false for the first half of the millis cycle and true for the second,
      // so past day 24.8 the box would have drawn with no alert to show. The
      // text is the state, and it is cleared below once the deadline passes.
      if (_alert[0] != 0 && !riftDue((uint32_t) millis(), _alert_expiry)) {
        // Same panel language as the message overlay: bg fill, accent border. It
        // used to fill with UIColor::popup_bkg, which riftApplyPalette points at
        // rift_pal.bar - the title-bar band, two percent from its own background
        // in both modes - so the box read as a stray outline rather than something
        // above the screen. bg occludes, and accent as a stroke is legal in both
        // palettes where accent as text is not.
        //
        // Sized to the text rather than to a third of the screen: the widest
        // string is "Name saved - send advert" at 24 chars, and the old box was
        // 306x80 with the text sitting 21px into it.
        _display->setTextSize(1);
        int tw = (int) strlen(_alert) * RIFT_CHAR_W;
        int bw = tw + 28;
        if (bw > _display->width() - 16) bw = _display->width() - 16;
        int bh = 8 + 14 * 2;
        int bx = (_display->width() - bw) / 2;
        int by = (_display->height() - bh) / 2;

        _display->setColor(rift_pal.bg);
        _display->fillRect(bx, by, bw, bh);
        // the same 2px rule frame as every overlay: an alert is a box on top, and
        // the accent is kept for what needs acting on
        _display->setColor(rift_pal.rule);
        _display->drawRect(bx, by, bw, bh);
        _display->drawRect(bx + 1, by + 1, bw - 2, bh - 2);
        _display->setColor(rift_pal.fg);
        _display->drawTextCentered(_display->width() / 2, by + 14, _alert);
        _next_refresh = _alert_expiry;
      } else {
        _alert[0] = 0;   // expired, or never set: stop testing a stale deadline
        _next_refresh = millis() + delay_millis;
      }
      _display->endFrame();
      if (dump_now) riftStreamFrame();
    }
#if AUTO_OFF_MILLIS > 0
#ifdef KEEP_DISPLAY_ON_USB
    if (board.isExternalPowered()) {
      _auto_off = millis() + AUTO_OFF_MILLIS;
    }
#endif
    // isExternalPowered() is HWCDC::isPlugged(), which reports a USB *host*. A
    // charger that never enumerates supplies power without being one, so the
    // device reads as on battery while it is charging. This is the setting for
    // exactly that case, and it needs nothing detected.
    if (rift_screen_always_on) {
      _auto_off = millis() + AUTO_OFF_MILLIS;
    }
    if (riftDue((uint32_t) millis(), _auto_off)) {
      // logged so a later "wake: screen on for message" has something to be read
      // against - without it there is no record that the screen was ever off
      riftLogf("screen off (idle)");
      _display->turnOff();
    }
#endif
  }

#ifdef PIN_VIBRATION
  vibration.loop();
#endif

#ifdef AUTO_SHUTDOWN_MILLIVOLTS
  if (riftDue((uint32_t) millis(), next_batt_chck)) {
    uint16_t milliVolts = getBattMilliVolts();
    if (milliVolts > 0 && milliVolts < AUTO_SHUTDOWN_MILLIVOLTS) {
      if(!board.isExternalPowered()) {
        if (_display != NULL) {
          _display->startFrame();
          _display->setTextSize(2);
          _display->setColor(UIColor::warning_txt);
          _display->drawTextCentered(_display->width() / 2, 20, "Low Battery.");
          _display->drawTextCentered(_display->width() / 2, 40, "Shutting Down!");
          _display->endFrame();
          if (_display->isEink() == false) { delay(3000); }
        }
        shutdown();
      }
    }
    next_batt_chck = millis() + 8000;
  }
#endif
}

char UITask::checkDisplayOn(char c) {
  if (_display != NULL) {
    if (!_display->isOn()) {
      _display->turnOn();
      c = 0;
    }
    _auto_off = millis() + AUTO_OFF_MILLIS;
    refreshNow();
  }
  return c;
}

char UITask::handleLongPress(char c) {
  if (millis() - ui_started_at < 8000) {
    // Logged, because this ran on every boot for three releases and nothing
    // said so. The age says whether a finger did it or the pin was already down
    // when the UI started - a real hold cannot complete inside the first second.
    riftLogf("CLI rescue: long press %lums after UI start",
             (unsigned long) (millis() - ui_started_at));
    the_mesh.enterCLIRescue();
    c = 0;
  }
  return c;
}

char UITask::handleDoubleClick(char c) {
  MESH_DEBUG_PRINTLN("UITask: double-click triggered");
  checkDisplayOn(c);
  return c;
}

char UITask::handleTripleClick(char c) {
  MESH_DEBUG_PRINTLN("UITask: triple click triggered");
  checkDisplayOn(c);
  toggleBuzzer();
  c = 0;
  return c;
}

bool UITask::hasGPSHardware() {
  if (_sensors == NULL) return false;
  int num = _sensors->getNumSettings();
  for (int i = 0; i < num; i++) {
    if (strcmp(_sensors->getSettingName(i), "gps") == 0) return true;
  }
  return false;
}

bool UITask::getGPSState() {
  if (_sensors != NULL) {
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
        return !strcmp(_sensors->getSettingValue(i), "1");
      }
    }
  }
  return false;
}

void UITask::toggleGPS() {
  if (_sensors != NULL) {
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
        if (strcmp(_sensors->getSettingValue(i), "1") == 0) {
          _sensors->setSettingValue("gps", "0");
          _node_prefs->gps_enabled = 0;
          notify(UIEventType::ack);
        } else {
          _sensors->setSettingValue("gps", "1");
          _node_prefs->gps_enabled = 1;
          notify(UIEventType::ack);
        }
        the_mesh.savePrefs();
        showAlert(_node_prefs->gps_enabled ? "GPS: Enabled" : "GPS: Disabled", 800);
        refreshNow();
        break;
      }
    }
  }
}

void UITask::toggleBuzzer() {
#ifdef PIN_BUZZER
  if (buzzer.isQuiet()) {
    buzzer.quiet(false);
    notify(UIEventType::ack);
  } else {
    buzzer.quiet(true);
  }
  _node_prefs->buzzer_quiet = buzzer.isQuiet();
  the_mesh.savePrefs();
  showAlert(buzzer.isQuiet() ? "Buzzer: OFF" : "Buzzer: ON", 800);
  refreshNow();
#endif
}
