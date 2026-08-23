#include "UITask.h"
#include "RiftLogic.h"
#include "RiftEventLog.h"
#include "RiftRxLog.h"
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
#define RIFT_HOP_COLS  4

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

  // 0 = newest, 1 = next older, ...
  const Entry* peek(int back) const {
    if (back < 0 || back >= count) return NULL;
    return &entries[count - 1 - back];
  }

  // How many bytes follow the kind byte in a stored record. One place, because a
  // writer and reader disagreeing about it would shift every field after it.
  static uint8_t convPayloadLen(uint8_t kind) {
    if (kind == RIFT_CONV_CHANNEL) return 1;
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
  static const uint8_t FILE_VERSION = 2;
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
      if (p->conv.kind == RIFT_CONV_CHANNEL) r[12] = p->conv.channel_idx;
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
        || (hdr[4] != FILE_VERSION && hdr[4] != 1)) {
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
        uint8_t clen = convPayloadLen(rec[11]);
        if (clen > 0 && f.read(payload, clen) != clen) break;
        if (rec[11] == RIFT_CONV_CHANNEL) conv = riftConvChannel(payload[0]);
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

  void begin(const char* initial, int capacity) {
    cap = (capacity < (int) sizeof(buf) - 1) ? capacity : (int) sizeof(buf) - 1;
    StrHelper::strncpy(buf, initial ? initial : "", sizeof(buf));
    len = strlen(buf);
    if (len > cap) { len = cap; buf[len] = 0; }
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
    const char* shown = buf;
    if (len > max_chars && max_chars > 0) shown = buf + (len - max_chars);

    display.setColor(UIColor::primary_txt);
    display.setCursor(x, y);
    display.print("> ");
    display.print(shown);
    display.print("_");
  }
};

// Bottom navigation hint row shared by every RIFT nav screen.
// Nav bar - y 226..239, per the design handoff. Label centres are given rather
// than computed: 64px columns would centre SYSTEM at 288, which clips against the
// right edge, so the spec nudges the outer two inward.
static const int NAV_CENTRE_X[RIFT_NAV_COUNT] = { 20, 81, 145, 209, 270 };

static void renderNavBar(DisplayDriver& display, int curr_idx) {
  const int y_rule = 226;

  display.setColor(rift_pal.rule);
  display.fillRect(0, y_rule, display.width(), 1);

  // Active tab carried by colour alone before this; in sunlight the grey step
  // between active and inactive disappears. The underline is the second cue.
  display.setColor(rift_pal.accent);
  display.fillRect(curr_idx * 64, y_rule, 64, 2);

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
        display.setColor(rift_pal.accent);
        display.fillRect(0, y_rule + 2, 64, 12);
        display.setColor(0xFFFF);
      } else {
        display.setColor(rift_pal.accent);
      }
    } else {
      display.setColor(i == curr_idx ? rift_pal.fg : rift_pal.dim);
    }
    display.drawTextCentered(NAV_CENTRE_X[i], 228, NAV_LABELS[i]);
  }

  // unread was not surfaced in the nav at all - you had to open COMMS to find out
  if (rift_nav_unread > 0) {
    display.setColor(rift_pal.accent);
    display.fillRect(246, 227, 3, 3);
  }

  // Battery, moved down from the title bar into space that was already here:
  // SYSTEM is centred at 270 and ends at 288, leaving 32px to the right edge, and
  // "100%" is 24px. The active-tab underline runs to 320 under it but sits at
  // y 226..228 while this text occupies 228..236, so they do not touch.
  //
  // mid rather than fg: this is chrome, and fg would make it compete with the
  // active nav label. accent when low, which is the one case worth interrupting.
  char batt[8];
  if (curr_idx == RIFT_NAV_SYSTEM) {
    // SYSTEM has two pages and nowhere else to say which one you are on. The
    // battery is on the other four screens, so nothing is lost by borrowing the
    // slack here for the page number.
    sprintf(batt, "%d/2", rift_system_page + 1);
    display.setColor(rift_pal.mid);
  } else {
    sprintf(batt, "%d%%", rift_nav_batt_pct);
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
// it is 3.5:1 and may only be a fill with white reversed out of it.
static const RiftPalette RIFT_NIGHT = {
  /* bg     */ 0x0000,   // #000000
  /* bar    */ 0x18C3,   // #1A1A1A
  /* fg     */ 0xFFFF,   // #FFFFFF
  /* mid    */ 0x9CD3,   // #9A9A9A
  /* dim    */ 0x8C51,   // #8A8A8A
  /* rule   */ 0x738E,   // #707070
  /* accent */ 0xFA00,   // #FF4100
  /* acc_tx */ 0xFA00,   // legible as text on black
  /* ok     */ 0x3E40    // #39C800
};
static const RiftPalette RIFT_DAY = {
  0xFFFF, 0xEF5D, 0x0000, 0x5ACB, 0x6B4D, 0x8C51, 0xFA00,
  /* acc_tx */ 0x2104,   // #202020 - the accent itself is unreadable as text here
  // #428610, not the night green. #39CB00 is 2.16:1 on white - below even the 3:1
  // non-text floor - and COMMS draws "ACK 1.2s" in it, so in day mode the one label
  // that says a message arrived was the least readable thing on the screen. Found by
  // computing the palette's contrast rather than by looking at it, which is the only
  // way this kind of failure gets noticed: it looks like a colour choice.
  //
  // This is the lightest green at that hue that still clears 4.5:1 on white: L
  // 0.1825, 4.52:1. Per-mode, exactly as acc_tx already is - the palette swaps
  // roles by design, so a value that cannot work in one mode is replaced rather
  // than compromised in both.
  /* ok     */ 0x4422
};

RiftPalette rift_pal = RIFT_NIGHT;
bool rift_day_mode = false;
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
static const TDeckSpeaker::Step SND_DM[]   = { {523,45}, {659,55} };
static const TDeckSpeaker::Step SND_CHAN[] = { {440,50} };
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
#define RIFT_SETTINGS_VERSION 1

void riftLoadSettings() {
#if defined(ESP32)
  File f = SPIFFS.open(RIFT_SETTINGS_PATH, "r");
  if (!f) return;
  uint8_t b[4];
  if (f.read(b, sizeof(b)) == sizeof(b)
      && b[0] == RIFT_SETTINGS_MAGIC0 && b[1] == RIFT_SETTINGS_MAGIC1
      && b[2] == RIFT_SETTINGS_VERSION) {
    riftApplyPalette((b[3] & 1) != 0);
    rift_screen_always_on = (b[3] & 2) != 0;
#ifdef RIFT_SPEAKER
    // bit 4; the low four were already spoken for by day mode, always-on and the
    // radar source, so this needed no version bump
    rift_sound_on = (b[3] & 16) != 0;
#endif
#ifdef RIFT_RADAR
    {
      uint8_t src = (b[3] >> 2) & 3;
      // 3 is not a state this ever writes; treat it as both rather than trusting it
      rift_radar_src = (src == RIFT_SRC_WIFI || src == RIFT_SRC_BLE) ? src : RIFT_SRC_BOTH;
    }
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
  }
  f.close();
#endif
}

void riftSaveSettings() {
#if defined(ESP32)
  File f = SPIFFS.open(RIFT_SETTINGS_PATH, "w");
  if (!f) return;
  uint8_t b[4] = { RIFT_SETTINGS_MAGIC0, RIFT_SETTINGS_MAGIC1, RIFT_SETTINGS_VERSION,
                   (uint8_t) ((rift_day_mode ? 1 : 0) | (rift_screen_always_on ? 2 : 0)
#ifdef RIFT_RADAR
                               // bits 2-3, which were spare
                               | ((rift_radar_src & 3) << 2)
#endif
#ifdef RIFT_SPEAKER
                               | (rift_sound_on ? 16 : 0)   // bit 4
#endif
                             ) };
  f.write(b, sizeof(b));
#ifdef RIFT_RADAR
  // Appended rather than versioned. The reader below stops when the file runs out,
  // so a settings file written before this existed still loads its flags - bumping
  // the version would have discarded a working day/night choice to add a feature.
  uint8_t nw = (uint8_t) rf_watch_count;
  f.write(&nw, 1);
  for (int i = 0; i < rf_watch_count; i++) {
    f.write(rf_watch[i].key, 6);
    uint8_t flag = rf_watch[i].is_wifi ? 1 : 0;
    f.write(&flag, 1);
    f.write((const uint8_t*) rf_watch[i].name, sizeof(rf_watch[i].name));
  }
#endif
  f.close();
#endif
}
int rift_nav_unread = 0;
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

void riftDrawBootScreen(DisplayDriver& display, const char* status) {
  const int x = 32;                  // left margin, matching the design
  const ColorVal white = 0xFFFF;     // RGB565; the shared palette has no white

  display.setColor(white);
  display.setTextSize(3);
  display.drawTextLeftAlign(x, 78, "RIFT");

  // Wordmark 2c: the letters cut along a shallow diagonal with the accent lying in
  // the cut, edge to edge. The horizontal blue rule that used to be here was the
  // stand-in for a slope the driver could not draw; a slope per column can be, and
  // the design now asks for one.
  //
  // seamY is the design's: y 96 at x=0 falling to y 48 at x=319, about -8.8 degrees.
  //
  // DEVIATION FROM THE SPEC, deliberately. The recipe asks for two text draws offset
  // 3px so the two halves shear along the seam. That cannot be composed with this
  // driver: it has fillRect and drawRect and no clipping, so whichever text is drawn
  // second spills across the seam, and the blank that removes the spill also removes
  // the half it was meant to keep - in either order. The rendering was produced in a
  // browser, where clipping exists. What is here is the same cut, the same accent in
  // the same gap, edge to edge, crossing no letter stem - without the lateral shear.
  // Adding the shear needs a hand-pixelled bitmap or a clip the driver lacks.
  #define RIFT_SEAM_Y(px) (92 - ((px) - 30) * 12 / 78)

  // the gap. 4px, so the 2px accent has a pixel of air either side - which is what
  // keeps it reading as a seam rather than as a line struck through the glyphs.
  // Bounded to the glyphs themselves (x 32..104 at setTextSize(3)) plus a margin,
  // rather than the whole width: past them it would be blanking background with
  // background, which is 50 wasted rects on a screen drawn during boot.
  display.setColor(rift_pal.bg);
  for (int px = x - 2; px < x + 80; px++) {
    display.fillRect(px, RIFT_SEAM_Y(px) - 1, 1, 4);
  }

  // The seam runs from the left edge to the middle and stops. Edge to edge read as
  // a rule with the wordmark sitting on it; stopping halfway makes the wordmark the
  // thing the line is part of, and leaves the right half of the screen for the
  // strapline underneath rather than putting a diagonal above it.
  display.setColor(rift_pal.accent);
  for (int px = 0; px < display.width() / 2; px++) {
    display.fillRect(px, RIFT_SEAM_Y(px), 1, 2);
  }

  // wide tracking, as drawn - Adafruit GFX has no letter-spacing control
  display.setColor(UIColor::secondary_txt);
  display.setTextSize(1);
  display.drawTextLeftAlign(x, 118, "R A D I O  I N T E L L I G E N C E");
  display.drawTextLeftAlign(x, 132, "$  F I E L D  T E R M I N A L");

  if (status != NULL) {
    display.setColor(white);
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
  int _btn_x0 = 0, _btn_x1 = 0;

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
      // network - so it is worth saying which of the two this is
      strcpy(row, "NOTHING HEARD SINCE BOOT");
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
    sprintf(tmp, "%d STORED %s %d HEARD", the_mesh.getNumContacts(),
            RIFT_DOT, the_mesh.getPathCacheUsed());
    display.drawTextLeftAlign(2, 170, tmp);

    sprintf(tmp, "LINK %.0f / %.0f", radio_driver.getLastRSSI(), radio_driver.getLastSNR());
    display.drawTextRightAlign(display.width() - 2, 170, tmp);

    display.setColor(rift_pal.mid);
    sprintf(tmp, "%.3fMHz  SF%d  %ddBm", _node_prefs->freq, _node_prefs->sf, _node_prefs->tx_power_dbm);
    display.drawTextCentered(display.width() / 2, 182, tmp);

    // The one action on this screen, drawn as a button because it is one - an
    // outlined box with the accent, rather than a line of hint text that happens to
    // be pressable. Both ENTER and a tap on it start a round.
    //
    // Zero-hop is in the label on purpose. It asks direct neighbours only, and a
    // name that implied it searched the mesh would be a promise it cannot keep.
    {
      const char* label = the_mesh.isDiscovering() ? "DISCOVERING..." : "ENTER: DISCOVER 0-HOP REPEATERS";
      int lw = (int) strlen(label) * RIFT_CHAR_W + 12;
      int bx = (display.width() - lw) / 2;
      display.setColor(rift_pal.accent);
      display.drawRect(bx, DISCOVER_BTN_Y, lw, 14);
      display.setColor(the_mesh.isDiscovering() ? rift_pal.accent : rift_pal.fg);
      display.drawTextCentered(display.width() / 2, DISCOVER_BTN_Y + 3, label);
      _btn_x0 = bx;
      _btn_x1 = bx + lw;
    }

    renderNavBar(display, RIFT_NAV_MESH);
    // while a round is open the button label and the result count both move, so
    // refresh faster than the blip alone would need
    return the_mesh.isDiscovering() ? 300 : 700;
  }

  static const int DISCOVER_BTN_Y = 198;

  bool handleInput(char c) override {
    if (c == KEY_ENTER) { _task->startRepeaterDiscovery(); return true; }
    if (c == KEY_NEXT || c == KEY_RIGHT) { _task->cycleNavScreen(1); return true; }
    if (c == KEY_PREV || c == KEY_LEFT) { _task->cycleNavScreen(-1); return true; }
    return false;
  }

  bool handleTouch(int x, int y) override {
    if (y < DISCOVER_BTN_Y - 2 || y > DISCOVER_BTN_Y + 14) return false;
    if (_btn_x0 <= 0 || x < _btn_x0 || x > _btn_x1) return false;   // not drawn yet, or a miss
    _task->startRepeaterDiscovery();
    return true;
  }
};

// SYSTEM: live keyboard/trackball diagnostics - proves the Step 2 input
// drivers work end-to-end. Real settings/diagnostics content lands later.
class RiftSystemScreen : public RiftScreen {
  UITask* _task;

  // A small action menu rather than hidden letter shortcuts - discoverable, and
  // it leaves the printable keys free for the text fields.
  enum Mode { MENU, EDIT_NAME, CH_NAME, CH_KEY_CHOICE, CH_KEY_ENTRY, CH_SHOW_KEY,
              CH_DELETE, CH_DELETE_CONFIRM, LOG, SET_TIME, RXLOG };
  enum Item { IT_ADVERT, IT_ADVERT_FLOOD, IT_NAME, IT_CHANNEL, IT_DELCHANNEL,
              IT_PATHMODE, IT_SCREEN, IT_SOUND, IT_DAYMODE, IT_SETTIME, IT_LOG, IT_RXLOG,
              IT_COUNT };

  int _log_scroll = 0;   // 0 = pinned to the newest line

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
      "Send advert (neighbours)",
      "Send advert (whole mesh)",
      "Edit node name",
      "Add channel",
      "Delete channel",
    };
    if (i == IT_RXLOG) {
      snprintf(buf, len, "View RX log (%u)", (unsigned) riftRxLog().total);
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
      case IT_ADVERT:
        // Other nodes cannot decrypt a direct message from a node they have
        // never heard an advert from - they look the sender up in their
        // contacts and silently drop it - so this is a prerequisite for
        // two-way DMs, not a nicety.
        _task->notify(UIEventType::ack);
        {
          bool ok = the_mesh.advert();
          riftLogf("advert neighbours: %s", ok ? "sent" : "FAILED");
          _task->showAlert(ok ? "Advert sent (direct)" : "Advert failed", 1200);
        }
        break;

      case IT_ADVERT_FLOOD:
        // reaches nodes beyond direct RF range, which is what they need before
        // they can decrypt a DM from us
        _task->notify(UIEventType::ack);
        {
          bool ok = the_mesh.advertFlood();
          riftLogf("advert whole mesh: %s", ok ? "sent" : "FAILED");
          _task->showAlert(ok ? "Advert flooded!" : "Advert failed", 1200);
        }
        break;

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
        display.setColor(0xFFFF);
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
    display.drawTextLeftAlign(4, 70, "Messages already received stay in the");
    display.drawTextLeftAlign(4, 82, "history. The key is gone from this");
    display.drawTextLeftAlign(4, 94, "device - rejoining needs it again.");

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
    renderHeading(display, "RX LOG");
    display.setTextSize(1);

    RiftRxLog& log = riftRxLog();
    char tmp[64];

    display.setColor(rift_pal.mid);
    snprintf(tmp, sizeof(tmp), "%u heard", (unsigned) log.total);
    display.drawTextLeftAlign(2, 20, tmp);
    // drawn at the same x as the data below it, not as one right-aligned string:
    // a header that does not sit over its column is worse than no header
    display.drawTextLeftAlign(48, 20, "TYPE");
    display.drawTextLeftAlign(108, 20, "RT HOP");
    display.drawTextRightAlign(206, 20, "RSSI");
    display.drawTextRightAlign(262, 20, "SNR");
    display.drawTextRightAlign(316, 20, "LEN");

    display.setColor(rift_pal.rule);
    display.fillRect(0, 32, display.width(), 1);

    if (log.count == 0) {
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(2, 40, "Nothing heard yet.");
      display.setColor(rift_pal.dim);
      display.drawTextLeftAlign(2, 56, "This counts every packet on the air,");
      display.drawTextLeftAlign(2, 68, "not only messages for this node.");
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
      display.drawTextRightAlign(42, y, tmp);

      uint8_t pt = riftHeaderPayloadType(e->header);
      display.setColor(rift_pal.fg);
      display.drawTextLeftAlign(48, y, riftPayloadTypeName(pt));

      display.setColor(rift_pal.mid);
      if (e->path_len == 0xFF) {
        // no path recorded, which is not 63 hops
        snprintf(tmp, sizeof(tmp), "%s  -", riftRouteTypeName(riftHeaderRouteType(e->header)));
      } else {
        snprintf(tmp, sizeof(tmp), "%s %uh", riftRouteTypeName(riftHeaderRouteType(e->header)),
                 (unsigned) riftHopCount(e->path_len));
      }
      display.drawTextLeftAlign(108, y, tmp);

      // RSSI and SNR are what say whether a packet was comfortable or marginal, and
      // a marginal one that decoded is the interesting case
      display.setColor(rift_pal.fg);
      snprintf(tmp, sizeof(tmp), "%d", (int) e->rssi);
      display.drawTextRightAlign(206, y, tmp);
      // Formatted from the magnitude with an explicit sign. Dividing a negative by
      // four truncates toward zero, so an SNR of -0.5 came out as "0.5" - the sign
      // vanished for exactly the marginal packets this column exists to show.
      {
        int q = (int) e->snr4;
        bool neg = q < 0;
        if (neg) q = -q;
        snprintf(tmp, sizeof(tmp), "%s%d.%d", neg ? "-" : "", q / 4, (q % 4) * 25 / 10);
      }
      display.drawTextRightAlign(262, y, tmp);
      snprintf(tmp, sizeof(tmp), "%u", (unsigned) e->len);
      display.drawTextRightAlign(316, y, tmp);
    }

    display.setColor(rift_pal.rule);
    display.fillRect(0, 200, display.width(), 1);
    display.setColor(rift_pal.dim);
    display.drawTextLeftAlign(2, 206, "up/down line  L/R page  ENTER back");
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
        display.setColor(0xFFFF);
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
          case IT_ADVERT:     note = "direct RF only - use whole mesh before a first DM"; break;
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

#ifdef RIFT_INPUT_KEYBOARD
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(CX, y, "KEYBOARD");
    // status is the only thing colour carries here, and only pass or fail
    display.setColor(rift_keyboard.isPresent() ? rift_pal.ok : rift_pal.accent);
    display.drawTextRightAlign(CR, y,
                               rift_keyboard.isPresent() ? "ok" : "not found");
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
      sprintf(tmp, "%d,%d", _task->lastTouchX(), _task->lastTouchY());
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

    // Occupancy and pressure on the path cache. 16 slots is a guess until it is
    // measured against a real mesh, and an eviction count is what says whether the
    // number is too small - a cache at 16/16 with evictions climbing is a mesh this
    // screen cannot show all of, which is a different problem from a layout that
    // cannot fit it.
    display.setColor(rift_pal.mid);
    group(display, "MESH", CX, y + 4);
    y += RIFT_LINE_H + 6;

    display.drawTextLeftAlign(CX, y, "PATH CACHE");
    {
      int used = the_mesh.getPathCacheUsed(), size = the_mesh.getPathCacheSize();
      unsigned evicted = the_mesh.getPathEvictions();
      display.setColor(evicted > 0 ? rift_pal.accent : rift_pal.fg);
      if (evicted > 0) snprintf(tmp, sizeof(tmp), "%d/%d, %u lost", used, size, evicted);
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
      default:                rr = "unknown"; break;
    }
    display.setColor((reason == ESP_RST_PANIC || reason == ESP_RST_BROWNOUT)
                     ? rift_pal.accent : rift_pal.fg);
    display.drawTextRightAlign(CR, y, rr);
    y += RIFT_LINE_H;

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

      display.setColor(rift_pal.accent);
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
      if (c == RIFT_KEY_BACK || c == KEY_CANCEL) { _mode = CH_NAME; return true; }
      return true;
    }

    if (_mode == CH_KEY_ENTRY) {
      if (c == KEY_ENTER) { finishChannel(2); return true; }
      if (_edit.handleKey(c)) return true;
      if (c == RIFT_KEY_BACK || c == KEY_CANCEL) { _mode = CH_KEY_CHOICE; return true; }
      return true;
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
      if (!_edit.handleKey(c)) _mode = MENU;   // backspace on an empty field backs out
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
        bool ok = (_del_sel < _del_count) && the_mesh.removeChannel(_del_idx[_del_sel]);
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
#define RIFT_HOP_ROWS 3
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
  // Where each row was actually drawn, so touch hits what the eye sees rather than
  // what the layout intended - the selected row is 36px tall, so a fixed pitch
  // would put every tap below it on the wrong node.
  int _row_y[RIFT_CONST_MAX];
  unsigned long _last_refresh;
  bool _refreshed_once;

  static const int BUCKET_X[4];
  static const char* BUCKET_LABEL[4];

  // Which bucket a hop count belongs in, or -1 for none. A node whose route is not
  // known is in no bucket, so the four counts can sum to less than the heard total -
  // that difference is real and the `?` rows in the list are where it lives.
  static int bucketOf(uint8_t path_len) {
    if (path_len == 0xFF) return -1;            // flood, route unknown
    int h = (int) riftHopCount(path_len);
    if (h == 0) return 0;
    if (h <= 2) return 1;
    if (h <= 5) return 2;
    return 3;
  }

  // Relative, and four characters wide. "14:32" says nothing about how long ago that
  // was without the reader doing arithmetic, which is the question the list asks.
  // Monotonic, so it is right whether or not the clock has ever been set.
  static void ageText(uint32_t recv_millis, char* out, size_t out_size) {
    uint32_t s = ((uint32_t) millis() - recv_millis) / 1000u;
    if (s < 60u)         StrHelper::strncpy(out, "now", out_size);
    else if (s < 5400u)  snprintf(out, out_size, "%um", (unsigned) (s / 60u));
    else if (s < 86400u) snprintf(out, out_size, "%uh", (unsigned) (s / 3600u));
    else if (s < 864000u) snprintf(out, out_size, "%ud", (unsigned) (s / 86400u));
    else                 StrHelper::strncpy(out, "9d+", out_size);
  }

  int resolveHash(const uint8_t* hash, uint8_t len) {
    if (len > sizeof(AdvertPath::pubkey_prefix)) len = sizeof(AdvertPath::pubkey_prefix);
    const uint8_t* candidates[RIFT_CONST_MAX];
    int n = (_count < RIFT_CONST_MAX) ? _count : RIFT_CONST_MAX;
    for (int i = 0; i < n; i++) candidates[i] = _paths[i].pubkey_prefix;
    return riftResolveHash(hash, len, candidates, n);
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
      int via = resolveHash(&p->path[k * hsz], hsz);
      if (via == RIFT_HASH_AMBIGUOUS) (*ambiguous)++;
      else if (via >= 0 && via < _count) label = _paths[via].name;

      size_t need = strlen(label) + (used ? 3 : 0);
      if (used + need >= out_size - 4) {              // room for " ..."
        StrHelper::strncpy(out + used, " ...", out_size - used);
        return;
      }
      if (used) { memcpy(out + used, " > ", 3); used += 3; }
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
    const int TOP = 68, BOTTOM = 226;
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
    int counts[4] = { 0, 0, 0, 0 };
    int maxhop = -1;
    for (int i = 0; i < _count; i++) {
      int b = bucketOf(_paths[i].path_len);
      if (b >= 0) counts[b]++;
      if (_paths[i].path_len != 0xFF) {
        int h = (int) riftHopCount(_paths[i].path_len);
        if (h > maxhop) maxhop = h;
      }
    }

    // ---- heading
    display.setColor(rift_pal.mid);
    // "RECENT", not "HEARD". The cache holds a bounded number of the most recently
    // heard nodes, so on a mesh larger than the cache "28 HEARD" read as a claim that
    // 28 is all there are. When it is at capacity the eviction count says so, because
    // that is the number which decides whether the cache is too small.
    int cache_used = the_mesh.getPathCacheUsed(), cache_size = the_mesh.getPathCacheSize();
    if (cache_used >= cache_size && the_mesh.getPathEvictions() > 0) {
      snprintf(tmp, sizeof(tmp), "%d RECENT, %u LOST", _count,
               (unsigned) the_mesh.getPathEvictions());
    } else {
      snprintf(tmp, sizeof(tmp), "%d RECENT", _count);
    }
    display.drawTextLeftAlign(2, 2, tmp);
    if (maxhop >= 0) {
      snprintf(tmp, sizeof(tmp), "MAX %d HOPS", maxhop);
      display.drawTextRightAlign(318, 2, tmp);
    }

    // ---- bucket band. The bars compare the four with each other, not against an
    // absolute scale: on a mesh of six nodes an absolute scale draws four stubs.
    int maxc = 0;
    for (int b = 0; b < 4; b++) if (counts[b] > maxc) maxc = counts[b];
    for (int b = 0; b < 4; b++) {
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(BUCKET_X[b], 14, BUCKET_LABEL[b]);
      if (counts[b] > 0 && maxc > 0) {
        int w = (counts[b] * 64 + maxc / 2) / maxc;
        if (w < 2) w = 2;
        display.setColor(rift_pal.fg);
        display.fillRect(BUCKET_X[b], 26, w, 6);
      }
      display.setColor(rift_pal.fg);
      snprintf(tmp, sizeof(tmp), "%d", counts[b]);
      display.drawTextLeftAlign(BUCKET_X[b], 36, tmp);
    }

    display.setColor(rift_pal.rule);
    display.fillRect(0, 52, display.width(), 1);

    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(14, 56, "NODE");
    display.drawTextRightAlign(278, 56, "HOPS");
    display.drawTextRightAlign(318, 56, "HEARD");

    for (int i = 0; i < RIFT_CONST_MAX; i++) _row_y[i] = -1;

    if (_count == 0) {
      display.setColor(rift_pal.mid);
      // "since boot" is part of the claim: this cache is cleared at startup, so an
      // empty list after a restart is the normal state and not a fault.
      display.drawTextLeftAlign(2, 68, "No adverts heard since boot");
      renderNavBar(display, RIFT_NAV_NODES);
      return 1000;
    }

    clampScroll();

    const int BOTTOM = 226;
    int y = 68;
    int shown = 0;
    for (int i = _scroll; i < _count; i++) {
      int h = rowHeight(i);
      if (y + h > BOTTOM) break;
      _row_y[i] = y;
      shown++;

      bool sel = (i == _sel);
      AdvertPath* p = &_paths[i];
      bool known = (p->path_len != 0xFF);
      int hops = known ? (int) riftHopCount(p->path_len) : 0;

      if (sel) {
        display.setColor(rift_pal.accent);
        display.fillRect(0, y, display.width(), 12);
      }
      uint16_t ink = sel ? 0xFFFF : rift_pal.fg;

      // Freshness is shape, not brightness: four grey levels collapse into each
      // other in sunlight, a filled versus hollow square does not.
      display.setColor(ink);
      uint32_t age_s = ((uint32_t) millis() - p->recv_millis) / 1000u;
      if (age_s < 1800u) display.fillRect(2, y + 3, 5, 5);
      else               display.drawRect(2, y + 3, 5, 5);

      char shown_name[24];
      riftTranslateUTF8(shown_name, p->name, sizeof(shown_name));
      display.setColor(ink);
      display.drawTextEllipsized(14, y + 2, 120, shown_name);

      if (known) {
        snprintf(tmp, sizeof(tmp), "%d", hops);
        display.setColor(ink);
      } else {
        StrHelper::strncpy(tmp, "?", sizeof(tmp));
        display.setColor(sel ? 0xFFFF : rift_pal.mid);
      }
      display.drawTextRightAlign(278, y + 2, tmp);

      ageText(p->recv_millis, tmp, sizeof(tmp));
      display.setColor(ink);
      display.drawTextRightAlign(318, y + 2, tmp);

      if (sel) {
        int dy = y + 12;
        int ambiguous = 0;
        char route[64];
        routeText(p, route, sizeof(route), &ambiguous);

        display.setColor(rift_pal.mid);
        if (!known) {
          display.drawTextLeftAlign(14, dy + 2, "flood, route unknown");
        } else if (ambiguous > 0) {
          // "ambiguous hops", not "candidates": this counts positions in the route
          // that could not be resolved, and one such position may itself have had
          // several candidate nodes behind it.
          snprintf(tmp, sizeof(tmp), "%d ambiguous hop%s", ambiguous, ambiguous == 1 ? "" : "s");
          display.drawTextLeftAlign(14, dy + 2, tmp);
        } else if (route[0]) {
          snprintf(tmp, sizeof(tmp), "via %s", route);
          display.drawTextEllipsized(14, dy + 2, 300, tmp);
        } else {
          display.drawTextLeftAlign(14, dy + 2, "heard direct");
        }

        // Type and absolute time. The RSSI the design asks for here does not exist:
        // adverts are cached without it, so the segment is omitted rather than
        // filled with a number this device does not have.
        ContactInfo* contact = the_mesh.lookupContactByPubKey(p->pubkey_prefix, 6);
        const char* type = (contact != NULL) ? riftAdvertTypeName(contact->type) : "unknown";
        uint32_t clk = the_mesh.getRTCClock()->getCurrentTime();
        char detail[48];
        if (clk >= 1600000000u && p->recv_timestamp >= 1600000000u) {
          int hh, mm;
          riftCivilFromEpoch(p->recv_timestamp, NULL, NULL, NULL, &hh, &mm);
          snprintf(detail, sizeof(detail), "%s %s %02d:%02d", type, RIFT_DOT, hh, mm);
        } else {
          // no invented time, and no --:-- either
          StrHelper::strncpy(detail, type, sizeof(detail));
        }
        display.setColor(rift_pal.mid);
        display.drawTextLeftAlign(14, dy + 14, detail);

        // Offered only when it would work. A repeater cannot receive a message, and
        // an action that will be refused is worse than no action shown.
        if (contact != NULL && riftCanDirectMessage(contact->type)) {
          if (rift_day_mode) {
            display.setColor(rift_pal.accent);
            display.fillRect(240, dy + 12, 78, 12);
            display.setColor(0xFFFF);
          } else {
            display.setColor(rift_pal.accent);
          }
          display.drawTextRightAlign(316, dy + 14, "ENTER: message");
        }
      }

      y += h;
    }

    int remaining = _count - _scroll - shown;
    if (remaining > 0 && y + 12 <= BOTTOM) {
      display.setColor(rift_pal.dim);
      snprintf(tmp, sizeof(tmp), "%d more", remaining);
      display.drawTextLeftAlign(14, y + 2, tmp);
    }

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

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) { _task->cycleNavScreen(1); return true; }
    if (c == KEY_PREV || c == KEY_LEFT) { _task->cycleNavScreen(-1); return true; }
    // Not wrapped: this is a position in a list, and jumping from the last node to
    // the first reads as the list having moved rather than the cursor.
    if (c == KEY_UP)   { if (_sel > 0) { _sel--; captureSelection(); } return true; }
    if (c == KEY_DOWN) { if (_sel + 1 < _count) { _sel++; captureSelection(); } return true; }
    if (c == KEY_ENTER) {
      if (_count == 0) return true;
      const uint8_t* key = _paths[_sel].pubkey_prefix;
      ContactInfo* contact = the_mesh.lookupContactByPubKey((uint8_t*) key, 6);
      if (contact == NULL) {
        _task->showAlert("Not a contact yet", 1400);
        return true;
      }
      if (!riftCanDirectMessage(contact->type)) {
        _task->showAlert("Repeaters can't receive a DM", 1600);
        return true;
      }
      _task->startDirectMessage(key);
      return true;
    }
    return false;
  }
};

const int RiftConstellationScreen::BUCKET_X[4] = { 2, 81, 160, 239 };
const char* RiftConstellationScreen::BUCKET_LABEL[4] = { "DIRECT", "1-2", "3-5", "6+" };

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
      if (rssi <= rf_table[weakest].rssi) { portEXIT_CRITICAL(&rf_mux); return; }
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
static void rfAgeOut() {
  unsigned long now = millis();
  portENTER_CRITICAL(&rf_mux);
  int w = 0;
  for (int i = 0; i < rf_count; i++) {
    if (now - rf_table[i].seen_at <= RIFT_RF_AGE_MILLIS) {
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
  int _scroll;
  int _last_n;
  int _watch_sel;      // cursor in the watch list

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

  void beginBle() {
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
    // the function-pointer overload returns immediately; start(duration, bool)
    // would block for the whole duration
    scan->start(RIFT_BLE_DWELL_SECS, onBleScanComplete, false);
  }

public:
  RiftRadarScreen(UITask* task)
     : _task(task), _state(OFF), _view(VIEW_BANDS), _want_active(false),
       _wifi_up(false), _ble_up(false), _torn_down(true),
       _wait_since(0), _wait_ms(0), _scroll(0), _last_n(0),
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
        if (!riftScanBle()) {
          // BLE is off, so the cycle ends here instead of in BLE_RUNNING. Ageing,
          // the presence check and the gap all have to happen exactly once per
          // cycle, so they are duplicated here rather than being skipped.
          rfAgeOut();
          rfWatchCheck(_task);
          _state = START_WIFI;
          _wait_since = millis();
          _wait_ms = RIFT_SCAN_GAP_MILLIS;
          break;
        }
        beginBle();
        _state = BLE_RUNNING;
        break;

      case BLE_RUNNING:
        if (ble_scan_done) {
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
    if (!renderWatchLamp(display)) {
      if (_state == OFF) renderHeading(display, "IDLE");
      else if (!_wifi_up && !_ble_up) renderHeading(display, "INITIALISING");
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

    int wifi_n = 0, ble_n = 0, new_n = 0;
    for (int i = 0; i < n; i++) {
      if (snap[i].is_wifi) wifi_n++; else ble_n++;
      if (now_ms - snap[i].first_seen < 60000) new_n++;
    }

    char tmp[64];

    // The one large value on the screen. "Is there anything around me" is the
    // question RADAR exists for, and it was previously answered by counting
    // dots in a scatter plot.
    display.setTextSize(3);
    display.setColor(rift_pal.fg);
    sprintf(tmp, "%d", n);
    display.drawTextLeftAlign(2, 22, tmp);

    display.setTextSize(1);
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(44, 24, "DEVICES NEARBY");
    // A disabled radio is dimmed and labelled rather than hidden. Hiding it would
    // make "0 ble" and "not looking for ble" the same reading, which is the
    // distinction the whole switch exists to make.
    display.setColor(riftScanWifi() ? rift_pal.fg : rift_pal.dim);
    snprintf(tmp, sizeof(tmp), riftScanWifi() ? "%d wifi" : "wifi off", wifi_n);
    display.drawTextLeftAlign(44, 36, tmp);
    display.setColor(riftScanBle() ? rift_pal.fg : rift_pal.dim);
    snprintf(tmp, sizeof(tmp), riftScanBle() ? "%d ble" : "ble off", ble_n);
    display.drawTextLeftAlign(120, 36, tmp);

    // "and is that changing" - the second half of the question
    if (new_n > 0) {
      sprintf(tmp, "+%d new", new_n);
      if (rift_day_mode) {
        display.setColor(rift_pal.accent);
        display.fillRect(280, 23, 40, 10);
        display.setColor(0xFFFF);
      } else {
        display.setColor(rift_pal.accent);
      }
      display.drawTextRightAlign(display.width() - 2, 24, tmp);
      display.setColor(rift_pal.mid);
      display.drawTextRightAlign(display.width() - 2, 36, "in 60s");
    }

    // Distance bands replace the scatter. The scatter placed each blip by
    // `i & 15` - its index in a table that compacts whenever a device ages out -
    // so blips moved with nothing having moved. One countable cell per device
    // in a signal-strength band says the same thing and stays still.
    display.setColor(rift_pal.rule);
    display.fillRect(0, 54, display.width(), 1);

    static const int BAND_Y[3] = { 62, 78, 94 };
    static const char* BAND_LABEL[3] = { "CLOSE", "MID", "FAR" };
    static const char* BAND_RANGE[3] = { "-30..-60", "-60..-80", "-80..-100" };
    const int CELL_X = 116;
    const int CELL_SHOWN = 24;   // (320-116-2)/8 = 25 slots; the last marks overflow

    for (int b = 0; b < 3; b++) {
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(2, BAND_Y[b], BAND_LABEL[b]);
      display.drawTextLeftAlign(46, BAND_Y[b], BAND_RANGE[b]);

      int cnt = 0;
      for (int i = 0; i < n; i++) {
        int s = snap[i].rssi;
        int band = (s > -60) ? 0 : (s > -80 ? 1 : 2);
        if (band == b) cnt++;
      }

      display.setColor(rift_pal.fg);
      int drawn = (cnt > CELL_SHOWN) ? CELL_SHOWN : cnt;
      for (int c = 0; c < drawn; c++) {
        int x = CELL_X + c * 8;
        // FAR is hollow rather than a dimmer grey: brightness steps disappear
        // under reflected light outdoors, form does not
        if (b == 2) display.drawRect(x, BAND_Y[b], 6, 8);
        else        display.fillRect(x, BAND_Y[b], 6, 8);
      }
      if (cnt > CELL_SHOWN) {
        // more than the row can hold - say so rather than silently clipping
        display.setColor(rift_pal.accent);
        display.fillRect(CELL_X + CELL_SHOWN * 8, BAND_Y[b], 6, 8);
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
    const int ROWS = (192 - 120) / RIFT_LINE_H + 1;
    if (_sel_idx < _scroll) _scroll = _sel_idx;
    if (_sel_idx >= _scroll + ROWS) _scroll = _sel_idx - ROWS + 1;
    if (_scroll < 0) _scroll = 0;

    display.setColor(rift_pal.rule);
    display.fillRect(0, 112, display.width(), 1);

    int y = 120;
    for (int i = _scroll; i < n && y <= 192; i++, y += RIFT_LINE_H) {
      bool top = (i == 0);
      bool is_sel = _have_sel && (i == _sel_idx);
      bool watched = rfWatchFind(snap[i].key, snap[i].is_wifi) >= 0;

      // cursor left of the RSSI column, and a filled block for a watched device.
      // Two marks rather than one colour: a watched device that is also selected
      // has to read as both, and colour cannot say two things in one cell.
      if (is_sel) {
        display.setColor(rift_pal.fg);
        display.drawTextLeftAlign(-4, y, ">");
      }
      if (watched) {
        display.setColor(rift_pal.accent);
        display.fillRect(24, y + 1, 4, 6);
      }
      char filtered[sizeof(snap[i].name)];
      riftTranslateUTF8(filtered, snap[i].name, sizeof(filtered));

      display.setColor(top ? rift_pal.fg : rift_pal.mid);
      sprintf(tmp, "%d", snap[i].rssi);
      display.drawTextLeftAlign(2, y, tmp);
      display.drawTextEllipsized(32, y, 180, filtered);

      if (snap[i].is_wifi) {
        sprintf(tmp, "WIFI c%d%s", snap[i].channel, snap[i].encrypted ? " *" : "");
      } else {
        strcpy(tmp, "BLE");
      }
      display.setColor(top ? (rift_day_mode ? 0x2104 : rift_pal.accent) : rift_pal.mid);
      display.drawTextRightAlign(display.width() - 2, y, tmp);
    }

    if (n == 0) {
      display.setColor(rift_pal.mid);
      display.drawTextLeftAlign(2, 120, "listening...");
    }

    display.setColor(rift_pal.rule);
    display.fillRect(0, 196, display.width(), 1);
    display.setColor(rift_pal.mid);
    {
      char foot[44];
      int watched = rf_watch_count;
      // the count doubles as the way in: it is the only hint that a list of them
      // exists, and W is how you reach it
      if (watched > 0) snprintf(foot, sizeof(foot), "ENTER watch  W wave/%d  S src", watched);
      else             snprintf(foot, sizeof(foot), "ENTER watch  W wave  S src");
      display.drawTextLeftAlign(2, 206, foot);
    }
    // a claim the user is entitled to see on the device, not only in the README
    display.drawTextRightAlign(display.width() - 2, 206, "nothing transmitted");

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
      // Reversed out of the fill. On white the accent is 3.5:1 - a legal fill and an
      // illegal text colour - so it is never the text here; on black the reverse
      // would work but one treatment for both keeps the lamp reading identically.
      display.setColor(0xFFFF);
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
      display.drawTextLeftAlign(2, 40, "Nothing marked.");
      display.setColor(rift_pal.dim);
      display.drawTextLeftAlign(2, 56, "W goes back. ENTER on a device");
      display.drawTextLeftAlign(2, 68, "in that list marks it.");
      renderNavBar(display, RIFT_NAV_RADAR);
      return 1000;
    }

    if (_watch_sel >= rf_watch_count) _watch_sel = rf_watch_count - 1;
    if (_watch_sel < 0) _watch_sel = 0;

    int y = 30;
    for (int i = 0; i < rf_watch_count; i++, y += RIFT_LINE_H * 2) {
      bool sel = (i == _watch_sel);
      if (sel) {
        display.setColor(rift_pal.accent);
        display.fillRect(0, y - 2, display.width(), 13);
      }
      uint16_t ink = sel ? 0xFFFF : rift_pal.fg;

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
      display.setColor(sel ? 0xFFFF : rift_pal.mid);
      display.drawTextRightAlign(display.width() - 2, y, tag);

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

    display.setColor(rift_pal.rule);
    display.fillRect(0, 196, display.width(), 1);
    display.setColor(rift_pal.mid);
    display.drawTextLeftAlign(2, 206, "ENTER unwatch  N rename  up/down  W back");
    renderNavBar(display, RIFT_NAV_RADAR);
    return 1000;
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
    if (c == 'w' || c == 'W') {
      // bands -> waterfall -> watches -> bands. Three on one key rather than a second
      // letter: they are all "which view of the same scan", and the footer names it.
      _view = (_view == VIEW_BANDS) ? VIEW_WATERFALL
            : (_view == VIEW_WATERFALL) ? VIEW_WATCHES : VIEW_BANDS;
      return true;
    }
    if (c == KEY_ENTER && _view == VIEW_BANDS) {
      if (!_have_sel) {
        _task->showAlert("Nothing to watch", 1200);
      } else {
        char msg[40];
        switch (rfWatchToggle(_sel_key, _sel_is_wifi, _sel_name)) {
          case RF_WATCH_ADDED:
            riftSaveSettings();
            snprintf(msg, sizeof(msg), "Watching %s", _sel_name);
            _task->showAlert(msg, 1600);
            riftLogf("watch + %s %s", _sel_is_wifi ? "wifi" : "ble", _sel_name);
            break;
          case RF_WATCH_REMOVED:
            riftSaveSettings();
            _task->showAlert("Watch removed", 1400);
            riftLogf("watch - %s", _sel_name);
            break;
          case RF_WATCH_FULL:
            // nothing changed, so nothing is saved either
            snprintf(msg, sizeof(msg), "Watch list full (%d)", RIFT_WATCH_MAX);
            _task->showAlert(msg, 1800);
            break;
        }
      }
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
  int num_unread;

public:
  RiftMsgPreviewScreen(UITask* task) : _task(task) { num_unread = 0; }

  // A popup over whatever the user is doing, not somewhere they navigated to.
  // This is what keeps RADAR's teardown and SYSTEM's secret wipe from firing
  // when a message arrives; both used to, and both were bugs.
  bool isOverlay() const override { return true; }

  void onNewMsg() {
    if (num_unread < RIFT_MSG_LOG_SIZE) num_unread++;
    rift_nav_unread = num_unread;   // what the nav dot means: unread *here*
  }

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
    display.setColor(rift_pal.accent);
    display.drawRect(x, y, w, h);

    display.setTextSize(1);
    display.setColor(rift_pal.accent);
    display.drawTextLeftAlign(x + 6, y + 6, "MESSAGES");

    char tmp[40];
    sprintf(tmp, "Unread: %d", num_unread);
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
  // Dismissing zeroes the count even when more than RIFT_PREVIEW_ROWS arrived, so
  // strictly it claims more was read than was shown. Accepted deliberately: the
  // full history is in COMMS and the nav dot means "unread here". Leaving the dot
  // lit for what scrolled past would recreate the nagging this replaced.
  void clearUnread() {
    num_unread = 0;
    rift_nav_unread = 0;
  }
};

// COMMS: MeshCore text terminal - scrollable history plus a compose line.
// Sends either to the Public channel (flooded, no ACK possible) or direct to a
// chosen contact (ACKed, so delivery state is shown). ESC on an empty line
// opens the target picker.
// Channels can occupy 40 slots and contacts up to MAX_CONTACTS, so this list
// cannot hold everything. Channels are added first so they are never crowded
// out, and the UI says when contacts were cut rather than silently hiding them.
#define RIFT_PICKER_MAX 48

class RiftCommsScreen : public RiftScreen, ContactVisitor {
  UITask* _task;
  char _input[MAX_TEXT_LEN + 1];
  int _len;
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
  }

  // Unconditional, and this is the right answer rather than a compromise.
  //
  // It was left as an open question here, on the reading that `_picking || _len > 0`
  // was the honest condition and that an idle history view had no business
  // suppressing a popup. That reading assumed the popup would add something. It
  // does not: the history loop in render() walks the whole message log without
  // filtering by channel or contact, so an arriving message is already on screen,
  // at the bottom of the list, on the refresh that newMsg() forces. A popup would
  // cover the view that is showing the message.
  //
  // So both cases point the same way. While composing or picking, a popup would
  // cost a half-typed line; while idle, it would obscure and duplicate. If the
  // history is ever filtered per target, this has to be revisited - that is the
  // change that would make an idle COMMS screen able to miss something.
  bool isModal() const override { return true; }
private:
  char _target_name[32];

  // target picker sub-view. Channels and contacts share one list; each entry
  // carries what it needs to become the send target.
  bool _picking;
  int _pick_idx;
  int _pick_scroll;   // index of the first visible row
  struct PickEntry {
    char name[32];
    bool is_channel;
    uint8_t channel_idx;   // channels only
    uint8_t key[6];        // contacts only
  };
  PickEntry _picks[RIFT_PICKER_MAX];
  int _pick_count;
  bool _pick_truncated;

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
        // accent fill with the label reversed out of it - the active channel has
        // to survive being read in sunlight, where a fill one shade off the
        // background does not
        display.setColor(rift_pal.accent);
        display.fillRect(x, _tabs_y - 2, tw - 2, 13);
        display.setColor(0xFFFF);
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

  // from ContactVisitor - called by scanRecentContacts(), already ordered by
  // last_advert_timestamp descending (most recently heard first)
  void onContactVisit(const ContactInfo& contact) override {
    if (contact.name[0] == 0) return;   // skip the reserved anon slots

    // only nodes that can actually receive a text message - repeaters and
    // sensors have no one reading them
    if (!riftCanDirectMessage(contact.type)) return;

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

    // then contacts, most recently heard first (scanRecentContacts sorts them)
    the_mesh.scanRecentContacts(0, this);

    _picking = true;
    _pick_idx = 0;
    _pick_scroll = 0;
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
                  riftConvChannel((uint8_t) _target_channel_idx), origin, sent, true);
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
      snprintf(buf, buf_len, "ACK %.1fs", p->trip_ms / 1000.0f);
      return buf;
    }
    // subtract rather than add: millis() wraps at ~49.7 days, and
    // (sent_at + timeout) would overflow and report a fresh send as timed out
    if (p->timeout_ms > 0 && millis() - p->sent_at_ms > p->timeout_ms) return "no ack";
    return "...";
  }

  int pickerRows() const { return (BODY_BOTTOM - (BODY_TOP + 4)) / RIFT_LINE_H; }

  int renderPicker(DisplayDriver& display) {
    renderHeading(display, "SELECT TARGET");
    display.setTextSize(1);

    int total = _pick_count;
    int rows = pickerRows();
    int y = BODY_TOP + 4;

    for (int i = _pick_scroll; i < total && i < _pick_scroll + rows; i++, y += RIFT_LINE_H) {
      bool sel = (_pick_idx == i);
      display.setColor(sel ? UIColor::title_txt : UIColor::secondary_txt);
      char filtered[32];
      riftTranslateUTF8(filtered, _picks[i].name, sizeof(filtered));
      display.setCursor(4, y);
      display.print(sel ? "> " : "  ");
      display.print(filtered);

      // channels are broadcast, contacts are addressed - worth distinguishing,
      // not least because only contacts can report delivery
      display.setColor(sel ? UIColor::title_txt : UIColor::secondary_txt);
      display.drawTextRightAlign(display.width() - 4, y, _picks[i].is_channel ? "channel" : "direct");
    }

    if (_pick_count == 0) {
      display.setColor(UIColor::secondary_txt);
      display.drawTextLeftAlign(4, y + RIFT_LINE_H, "(no channels or contacts)");
    } else if (_pick_truncated) {
      display.setColor(UIColor::warning_txt);
      display.drawTextLeftAlign(4, INPUT_Y - RIFT_LINE_H, "list full - some contacts not shown");
    }

    // scroll position indicator when the list doesn't fit
    if (total > rows) {
      display.setColor(UIColor::secondary_txt);
      char pos[16];
      snprintf(pos, sizeof(pos), "%d/%d", _pick_idx + 1, total);
      display.drawTextRightAlign(display.width() - 4, BODY_TOP + 4, pos);
    }

    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, INPUT_Y, "trackball up/down  ENTER pick  click back");

    renderNavBar(display, RIFT_NAV_COMMS);
    return 1000;
  }

  // keep the selected row inside the visible window
  void ensurePickVisible() {
    int rows = pickerRows();
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
    if (c == KEY_ENTER) {
      PickEntry* e = &_picks[_pick_idx];
      _target_is_channel = e->is_channel;
      if (e->is_channel) {
        _target_channel_idx = e->channel_idx;
      } else {
        memcpy(_target_key, e->key, 6);
      }
      StrHelper::strncpy(_target_name, e->name, sizeof(_target_name));
      _picking = false;
      return true;
    }
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
    ChannelDetails ch;
    bool headed = true;
    if (!_target_is_channel) {
      renderHeading(display, _target_name);
    } else if (!getTargetChannel(ch)) {
      // nothing in the strip is filled in this state, so say why
      renderHeading(display, "NO CHANNEL");
    } else {
      headed = false;
    }

    _tabs_y = headed ? TABS_Y_HEADED : TABS_Y_BARE;
    _hist_top = _tabs_y + 16;

    renderTabs(display);

    display.setTextSize(1);

    // History, newest at the bottom: lay entries out upward from the input line.
    int avail_px = display.width() - 8;
    int y = BODY_BOTTOM;
    for (int back = _scroll; back < msg_log.count; back++) {
      auto p = msg_log.peek(back);
      if (p == NULL) break;

      char filtered[sizeof(p->msg)];
      riftTranslateUTF8(filtered, p->msg, sizeof(filtered));

      char filtered_origin[sizeof(p->origin)];
      riftTranslateUTF8(filtered_origin, p->origin, sizeof(filtered_origin));
      int hh = (p->timestamp / 3600) % 24;
      int mm = (p->timestamp / 60) % 60;

      char ack_buf[16];
      const char* ack = deliveryLabel(p, ack_buf, sizeof(ack_buf));

      int body_lines = wrapText(filtered, avail_px, 0, NULL, 0);
      int block_h = (body_lines + 1) * RIFT_LINE_H;

      y -= block_h;
      if (y < _hist_top) break;   // ran out of room going up

      // Own messages carry a 2px accent bar down the left edge rather than being
      // right-aligned: on a 320px screen right alignment costs half the width for
      // every outgoing line, and this costs two pixels.
      if (p->outgoing) {
        display.setColor(rift_pal.accent);
        display.fillRect(0, y, 2, block_h - 2);
      }

      char tbuf[8];
      sprintf(tbuf, "%02d:%02d", hh, mm);
      display.setColor(rift_pal.dim);
      display.drawTextLeftAlign(6, y, tbuf);
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
      uint16_t chan_col = originColour(p->origin);
      display.setColor(chan_col != RIFT_CHAN_COL_NONE ? chan_col : rift_pal.mid);
      display.drawTextEllipsized(38, y, 232, filtered_origin);

      // The right end of this row answers "what happened to this packet", and the
      // hop count is the same kind of answer as the delivery state - so it goes
      // here rather than in front of the name, where "14:52 (6) Public:" read as
      // two clock times. Delivery wins the slot when there is one: a channel
      // message has no ack truth, so the hop count stands alone there.
      if (ack != NULL) {
        // green for delivered, accent for a send that never landed
        bool ok = (strncmp(ack, "ACK", 3) == 0);
        display.setColor(ok ? rift_pal.ok : (ack[0] == '.' ? rift_pal.mid : rift_pal.accent));
        display.drawTextRightAlign(display.width() - 2, y, ack);
      } else {
        int hops = 0;
        bool direct = false;
        if (riftOriginHops(p->origin, &hops, &direct)) {
          char hb[12];
          if (direct)        strcpy(hb, "direct");
          else if (hops == 1) strcpy(hb, "1 hop");
          else                snprintf(hb, sizeof(hb), "%d hops", hops);
          display.setColor(rift_pal.mid);
          display.drawTextRightAlign(display.width() - 2, y, hb);
        }
      }

      display.setColor(rift_pal.fg);
      wrapText(filtered, avail_px, y + RIFT_LINE_H, &display, 6);
    }

    // Compose line - show the tail once the text outgrows one line.
    display.setColor(rift_pal.rule);
    display.fillRect(0, INPUT_Y - 4, display.width(), 1);

    display.setColor(rift_pal.dim);
    if (_len == 0) {
      display.drawTextRightAlign(display.width() - 2, INPUT_Y, "ENTER: pick target");
    } else {
      // MeshCore truncates at MAX_TEXT_LEN, so show how close the message is
      char cnt[12];
      int cap = channelCapacity();
      sprintf(cnt, "%d/%d", _len, cap);
      display.setColor(_len >= cap ? rift_pal.accent : rift_pal.dim);
      display.drawTextRightAlign(display.width() - 2, INPUT_Y, cnt);
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
    if (_picking) return false;
    if (_tabs_y <= 0) return false;   // strip hasn't been drawn yet
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
    return true;
  }

  bool handleInput(char c) override {
    if (_picking) return handlePickerInput(c);

    if (c == KEY_NEXT) { _task->cycleNavScreen(1); return true; }
    if (c == KEY_PREV) { _task->cycleNavScreen(-1); return true; }
    if (c == KEY_RIGHT) { _task->cycleNavScreen(1); return true; }
    if (c == KEY_LEFT) { _task->cycleNavScreen(-1); return true; }

    if (c == KEY_UP) {     // scroll back through history
      if (_scroll + 1 < msg_log.count) _scroll++;
      return true;
    }
    if (c == KEY_DOWN) {
      if (_scroll > 0) _scroll--;
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
    display.setColor(rift_pal.accent);
    display.drawRect(x, y, w, h);

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
    display.setColor(rift_pal.accent);
    display.drawRect(x, y, w, h);

    display.setTextSize(1);
    display.setColor(rift_pal.dim);
    display.drawTextLeftAlign(x + 6, y + 4, "ENTER  BKSP");

    for (int i = 0; i < _count; i++) {
      int cx = x + 6 + i * cell;
      if (i == _sel) {
        // accent fill with the glyph reversed out - legal in both palettes, where
        // accent as text is not
        display.setColor(rift_pal.accent);
        display.fillRect(cx - 2, y + 14, cell - 2, 22);
        display.setColor(0xFFFF);
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
    display.setColor(rift_pal.accent);
    display.drawRect(x, y, w, h);

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
  if (conv_kind == RIFT_CONV_CHANNEL) conv = riftConvChannel(channel_idx);
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
  ((RiftMsgPreviewScreen *) msg_preview)->onNewMsg();

  // Don't take the screen away from someone mid-input: a half-typed line in
  // COMMS, a channel name being entered, or a one-time key being read. Each
  // screen answers for itself now - this used to name COMMS and SYSTEM here and
  // reach into the latter to ask.
  //
  // Nothing stacks: a second message while the preview is already up must leave
  // it where it is, or dismissing would land back on the popup.
  if (_overlay == NULL && curr != NULL && !curr->isModal()) {
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
    int tx, ty;
    if (rift_touch.poll(tx, ty)) {
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

#ifdef RIFT_RADAR
  // serviced unconditionally, not via curr->poll(), so RF teardown still runs
  // after navigating away from RADAR
  if (nav_screens[RIFT_NAV_RADAR] != NULL) ((RiftRadarScreen *) nav_screens[RIFT_NAV_RADAR])->service();
#endif

#ifdef RIFT_SPEAKER
  // Before the screen work: it writes at most one DMA buffer with a zero timeout, so
  // it is bounded, and a tone that stutters because a redraw ran first is worse than
  // a redraw that waits a few hundred microseconds.
  rift_speaker.loop();
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
        _display->setColor(rift_pal.accent);
        _display->drawRect(bx, by, bw, bh);
        _display->setColor(rift_pal.fg);
        _display->drawTextCentered(_display->width() / 2, by + 14, _alert);
        _next_refresh = _alert_expiry;
      } else {
        _alert[0] = 0;   // expired, or never set: stop testing a stale deadline
        _next_refresh = millis() + delay_millis;
      }
      _display->endFrame();
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
