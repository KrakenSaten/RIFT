#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string.h>

#include "RiftLogic.h"
#include <helpers/UTF8Helpers.h>   // validUtf8PrefixLength, for cutting a CLI line

// Logging into a repeater and reading it back, from the device rather than from
// a phone.
//
// The mesh layer already does all of the work: BaseChatMesh::sendLogin,
// sendRequest and sendCommandData send, and onContactResponse and
// onCommandDataRecv receive. Upstream routes every one of those replies out the
// serial port to the companion app and keeps nothing, so on a T-Deck with no
// phone attached the answers arrived and were discarded. This keeps them.
//
// It observes rather than intercepts: the replies still go to the companion app
// exactly as before, so a phone-attached device behaves the same.
//
// That claim used to be made about the send path too, and it was wrong there.
// The panel first reused the companion pending slots and called
// clearPendingReqs() like the companion handlers do, which meant a local
// request discarded whatever the phone was waiting for. The panel now owns
// separate matching state in MyMesh - see rift_pending_kind - and touches none
// of the five companion slots.
//
// One target at a time. The panel shows one node, and a second session would be
// state with nothing on screen to represent it.

#define RIFT_REP_KEY_LEN     6     // the prefix onContactResponse reports
#define RIFT_REP_CLI_LINES   8
#define RIFT_REP_CLI_TEXT   64
#define RIFT_REP_TELEM      10

// What we are waiting for. One at a time, because the panel offers one action
// at a time and refuses a second while one is outstanding. These values are also
// what MyMesh::rift_pending_kind holds, so the two ends agree on the request in
// flight without a second vocabulary.
#define RIFT_REP_IDLE        0
#define RIFT_REP_LOGIN       1
#define RIFT_REP_STATUS      2
#define RIFT_REP_TELEMETRY   3
#define RIFT_REP_CLI         4
#define RIFT_REP_REGIONS     5

// Login outcome, kept apart from the pending kind so a failure stays on screen
// after the wait has ended.
#define RIFT_LOGIN_NONE      0
#define RIFT_LOGIN_WAITING   1
#define RIFT_LOGIN_OK        2
#define RIFT_LOGIN_FAILED    3
#define RIFT_LOGIN_TIMEOUT   4

struct RiftTelemReading {
  uint8_t channel;
  uint8_t type;
  float   value;
};

class RiftRepeaterSession {
  uint8_t _key[RIFT_REP_KEY_LEN];
  bool _have_target;

  uint8_t _login;
  uint8_t _perms;
  uint8_t _acl;
  uint8_t _fw_level;

  uint8_t _pending;
  uint32_t _deadline;      // millis; compare with riftDue()

  RiftRepeaterStats _stats;
  bool _have_stats;
  uint32_t _stats_at;      // millis, for an age readout

  RiftTelemReading _telem[RIFT_REP_TELEM];
  int _telem_n;
  bool _have_telem;

  char _cli[RIFT_REP_CLI_LINES][RIFT_REP_CLI_TEXT];
  int _cli_n;
  // Deadline until which every CLI reply from this node is redacted, not a flag
  // consumed by the first reply that arrives.
  //
  // A CLI response carries no request tag - it is matched on the sender alone -
  // so with a phone attached to the same radio there is no way to tell whose
  // command a reply answers. As a one-shot flag this leaked: RIFT asks for
  // `get guest.password`, the phone sends something harmless to the same
  // repeater, the harmless reply arrives first and consumes the flag, and the
  // bare password that follows is then treated as ordinary text.
  //
  // Confidentiality first. While a secret read is outstanding, everything from
  // that node is withheld. It can hide a line that was not secret; the
  // alternative puts a private key on the screen.
  uint32_t _secret_until;

  bool isTarget(const uint8_t* pub_key) const {
    return _have_target && pub_key != NULL
        && memcmp(_key, pub_key, RIFT_REP_KEY_LEN) == 0;
  }

  // Newest at index 0. Shifts before writing so a full ring drops the oldest.
  void pushLine(const char* src, int n) {
    if (n > RIFT_REP_CLI_TEXT - 1) {
      // Cut on a code point boundary, not a byte. A raw cut splits a two-byte
      // Nordic character and leaves a dangling lead byte, which matters most for
      // `neighbors` - node names are exactly where those characters live.
      n = (int) mesh::validUtf8PrefixLength(src, (size_t) (RIFT_REP_CLI_TEXT - 1));
    }
    if (_cli_n < RIFT_REP_CLI_LINES) _cli_n++;
    for (int i = _cli_n - 1; i > 0; i--) {
      memcpy(_cli[i], _cli[i - 1], RIFT_REP_CLI_TEXT);
    }
    memcpy(_cli[0], src, (size_t) n);
    _cli[0][n] = 0;
  }

public:
  RiftRepeaterSession() { reset(); }

  void reset() {
    memset(_key, 0, sizeof(_key));
    _have_target = false;
    _login = RIFT_LOGIN_NONE;
    _perms = _acl = _fw_level = 0;
    _pending = RIFT_REP_IDLE;
    _deadline = 0;
    memset(&_stats, 0, sizeof(_stats));
    _have_stats = false;
    _stats_at = 0;
    _telem_n = 0;
    _have_telem = false;
    _cli_n = 0;
    _secret_until = 0;
    // Zeroed, not just counted down to nothing. A redaction keeps a secret off
    // the screen; it does not help if the bytes are still sitting in this buffer
    // afterwards, and clearing the count alone left them there.
    memset(_cli, 0, sizeof(_cli));
  }

  // Switching target drops everything: stats and a CLI reply belong to the node
  // that sent them, and showing them under another name is the worst failure
  // this screen could have.
  void setTarget(const uint8_t* pub_key) {
    if (pub_key == NULL) { reset(); return; }
    if (isTarget(pub_key)) return;
    reset();
    memcpy(_key, pub_key, RIFT_REP_KEY_LEN);
    _have_target = true;
  }

  bool haveTarget() const { return _have_target; }
  const uint8_t* target() const { return _key; }

  uint8_t loginState() const { return _login; }
  bool isAdmin() const { return _login == RIFT_LOGIN_OK && (_perms & 1) != 0; }
  uint8_t permissions() const { return _perms; }
  uint8_t aclPermissions() const { return _acl; }
  uint8_t firmwareLevel() const { return _fw_level; }

  uint8_t pending() const { return _pending; }
  uint32_t deadline() const { return _deadline; }

  bool haveStats() const { return _have_stats; }
  const RiftRepeaterStats& stats() const { return _stats; }
  uint32_t statsAt() const { return _stats_at; }

  bool haveTelemetry() const { return _have_telem; }
  int telemetryCount() const { return _telem_n; }
  const RiftTelemReading& telemetry(int i) const { return _telem[i]; }

  int cliCount() const { return _cli_n; }
  const char* cliLine(int i) const { return _cli[i]; }

  // ---- issuing ----
  //
  // No password is stored or logged anywhere in here. It is a shared secret for
  // somebody else's repeater, and the event log is drawn on screen and read
  // over shoulders in the field. It is passed to sendLogin and forgotten.
  void beginPending(uint8_t kind, uint32_t est_timeout_millis) {
    _pending = kind;
    // The estimate from the mesh layer is airtime, not the time the repeater
    // takes to think, so it gets a floor and some slack. A wait that ends too
    // early reports a timeout for a reply that is still in flight.
    uint32_t wait = est_timeout_millis + 4000;
    if (wait < 8000) wait = 8000;
    _deadline = millis() + wait;
    if (kind == RIFT_REP_LOGIN) _login = RIFT_LOGIN_WAITING;
  }

  void cancelPending() { _pending = RIFT_REP_IDLE; }

  // Called from the UI poll. Returns true if a wait has just expired, so the
  // caller can say so once rather than on every frame.
  bool checkTimeout() {
    if (_pending == RIFT_REP_IDLE) return false;
    if (!riftDue(millis(), _deadline)) return false;
    if (_pending == RIFT_REP_LOGIN) _login = RIFT_LOGIN_TIMEOUT;
    _pending = RIFT_REP_IDLE;
    return true;
  }

  // ---- receiving ----

  void onLogin(const uint8_t* pub_key, bool ok, uint8_t perms, uint8_t acl, uint8_t fw) {
    if (!isTarget(pub_key)) return;
    _login = ok ? RIFT_LOGIN_OK : RIFT_LOGIN_FAILED;
    _perms = ok ? perms : 0;
    _acl = ok ? acl : 0;
    _fw_level = ok ? fw : 0;
    if (_pending == RIFT_REP_LOGIN) _pending = RIFT_REP_IDLE;
  }

  void onStatus(const uint8_t* pub_key, const uint8_t* data, int len) {
    if (!isTarget(pub_key)) return;
    RiftRepeaterStats s;
    // A reply too short to decode keeps the previous one rather than blanking
    // the panel: the old numbers are still true, just older.
    if (!riftDecodeRepeaterStats(data, len, &s)) return;
    _stats = s;
    _have_stats = true;
    _stats_at = millis();
    if (_pending == RIFT_REP_STATUS) _pending = RIFT_REP_IDLE;
  }

  void beginTelemetry() { _telem_n = 0; }

  void addTelemetry(uint8_t channel, uint8_t type, float value) {
    if (_telem_n >= RIFT_REP_TELEM) return;
    _telem[_telem_n].channel = channel;
    _telem[_telem_n].type = type;
    _telem[_telem_n].value = value;
    _telem_n++;
  }

  void endTelemetry(const uint8_t* pub_key) {
    if (!isTarget(pub_key)) { _telem_n = 0; return; }
    _have_telem = _telem_n > 0;
    if (_pending == RIFT_REP_TELEMETRY) _pending = RIFT_REP_IDLE;
  }

  // A CLI reply can be several lines in one blob of text. Split on newlines so
  // the screen can print it without measuring anything.
  void onCliReply(const uint8_t* pub_key, const char* text) {
    if (!isTarget(pub_key) || text == NULL) return;
    if (_pending == RIFT_REP_CLI) _pending = RIFT_REP_IDLE;

    // A reply can carry a secret two ways: because we asked for one, or because
    // upstream echoes a new password back unasked. Either way the whole reply is
    // replaced rather than edited - it is not worth trying to find the secret
    // inside a string whose shape belongs to somebody else's firmware.
    // Not cleared here: the window stands until it expires, because the reply
    // that consumed it might have been the phone's.
    bool secret = (_secret_until != 0 && !riftDue(millis(), _secret_until))
                  || riftCliReplyEchoesSecret(text);
    if (secret) {
      const char* safe = "(secret reply not shown)";
      pushLine(safe, (int) strlen(safe));
      return;
    }

    const char* p = text;
    while (*p) {
      const char* nl = strchr(p, '\n');
      int n = nl ? (int) (nl - p) : (int) strlen(p);
      if (n > 0) pushLine(p, n);
      if (!nl) break;
      p = nl + 1;
    }
  }

  // The regions a repeater will flood for, from an anonymous REGIONS request.
  //
  // Comma-separated on the wire, one line each here, because that is how they are
  // read. "*" is the wildcard - the repeater floods anything - and is passed
  // through as it arrives rather than renamed, since it is upstream's notation
  // and a friendlier word here would make two vocabularies for one thing.
  void onRegions(const uint8_t* pub_key, const char* names) {
    if (!isTarget(pub_key)) return;
    if (_pending == RIFT_REP_REGIONS) _pending = RIFT_REP_IDLE;

    if (names == NULL || names[0] == 0) {
      const char* none = "regions: none advertised";
      pushLine(none, (int) strlen(none));
      return;
    }
    const char* p = names;
    while (*p) {
      const char* comma = strchr(p, ',');
      int n = comma ? (int) (comma - p) : (int) strlen(p);
      if (n > 0) {
        char line[RIFT_REP_CLI_TEXT];
        snprintf(line, sizeof(line), "region %.*s", n, p);
        pushLine(line, (int) strlen(line));
      }
      if (!comma) break;
      p = comma + 1;
    }
  }

  // Echo of what was sent, so the panel reads as a transcript rather than as a
  // list of answers to questions nobody can see.
  //
  // Redacted on the way in, not on the way out: the secret must never be in
  // _cli at all, because that buffer outlives the screen it was drawn on.
  void noteCliSent(const char* text) {
    if (text == NULL) return;
    bool is_secret = riftCliIsSecret(text);
    if (is_secret) {
      // Long enough to cover a slow answer over several hops, short enough that
      // the panel is not silently withholding replies minutes later.
      _secret_until = millis() + 30000;
    }

    char safe[RIFT_REP_CLI_TEXT];
    if (is_secret) {
      riftRedactCliCommand(text, safe, sizeof(safe));
    } else {
      snprintf(safe, sizeof(safe), "%s", text);
    }
    char line[RIFT_REP_CLI_TEXT];
    snprintf(line, sizeof(line), "> %s", safe);
    pushLine(line, (int) strlen(line));
  }
};

inline RiftRepeaterSession& riftRepeater() {
  static RiftRepeaterSession s;
  return s;
}
