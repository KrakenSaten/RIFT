#include "UITask.h"
#include <helpers/TxtDataHelpers.h>
#include "../MyMesh.h"
#include "target.h"

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

static const char* NAV_LABELS[RIFT_NAV_COUNT] = { "MESH", "RADAR", "COMMS", "SYSTEM" };

#ifndef RIFT_PUBLIC_CHANNEL_IDX
  #define RIFT_PUBLIC_CHANNEL_IDX 0
#endif

#define RIFT_MSG_LOG_SIZE  48
#define RIFT_CHAR_W         6   // Adafruit GFX classic font cell at setTextSize(1)
#define RIFT_LINE_H        12   // row pitch used throughout this codebase

// Shared in-memory message log. MeshCore keeps no message history of its own
// (DataStore holds identity/prefs/contacts/channels only, and MyMesh's offline
// queue is private raw protocol frames), so the UI owns this - same approach as
// ui-new, just shared between the popup and the COMMS terminal.
struct RiftMsgLog {
  struct Entry {
    uint32_t timestamp;
    char origin[62];
    char msg[78];
    bool outgoing;
    // delivery tracking, only meaningful for outgoing direct messages.
    // expected_ack == 0 means "no ACK possible" (channel sends, incoming) and
    // renders no delivery state at all.
    uint32_t expected_ack;
    uint32_t sent_at_ms;
    uint32_t timeout_ms;
    uint32_t trip_ms;
    bool delivered;
  };

  Entry entries[RIFT_MSG_LOG_SIZE];
  int count = 0;
  int head = RIFT_MSG_LOG_SIZE - 1;   // index of newest entry

  Entry* add(uint32_t timestamp, const char* origin, const char* msg, bool outgoing) {
    head = (head + 1) % RIFT_MSG_LOG_SIZE;
    if (count < RIFT_MSG_LOG_SIZE) count++;

    Entry* p = &entries[head];
    p->timestamp = timestamp;
    StrHelper::strncpy(p->origin, origin, sizeof(p->origin));
    StrHelper::strncpy(p->msg, msg, sizeof(p->msg));
    p->outgoing = outgoing;
    p->expected_ack = 0;
    p->sent_at_ms = 0;
    p->timeout_ms = 0;
    p->trip_ms = 0;
    p->delivered = false;
    return p;
  }

  // mark the pending outgoing message matching this ACK hash as delivered
  void markDelivered(uint32_t ack_hash, uint32_t trip_ms) {
    if (ack_hash == 0) return;
    for (int i = 0; i < count; i++) {
      Entry* p = &entries[(head - i + RIFT_MSG_LOG_SIZE * 2) % RIFT_MSG_LOG_SIZE];
      if (p->expected_ack == ack_hash && !p->delivered) {
        p->delivered = true;
        p->trip_ms = trip_ms;
        return;
      }
    }
  }

  // 0 = newest, 1 = next older, ...
  const Entry* peek(int back) const {
    if (back < 0 || back >= count) return NULL;
    return &entries[(head - back + RIFT_MSG_LOG_SIZE * 2) % RIFT_MSG_LOG_SIZE];
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

// Bottom navigation hint row shared by every RIFT nav screen.
static void renderNavBar(DisplayDriver& display, int curr_idx) {
  int y = display.height() - 12;
  display.setColor(UIColor::secondary_txt);
  display.drawRect(0, y - 2, display.width(), 1);   // separator line

  int col_w = display.width() / RIFT_NAV_COUNT;
  display.setTextSize(1);
  for (int i = 0; i < RIFT_NAV_COUNT; i++) {
    display.setColor(i == curr_idx ? UIColor::title_txt : UIColor::secondary_txt);
    display.drawTextCentered(col_w * i + col_w / 2, y, NAV_LABELS[i]);
  }
}

// Compact top status bar shared by every RIFT nav screen: title + battery %.
static void renderTitleBar(DisplayDriver& display, UITask* task, const char* subtitle) {
  display.setColor(UIColor::title_bkg);
  display.fillRect(0, 0, display.width(), 16);

  display.setColor(UIColor::title_txt);
  display.setTextSize(1);
  display.setCursor(2, 4);
  display.print("RIFT");

  display.setColor(UIColor::secondary_txt);
  display.setCursor(40, 4);
  display.print(subtitle);

  // battery percentage, top-right
#ifndef BATT_MIN_MILLIVOLTS
  #define BATT_MIN_MILLIVOLTS 3000
#endif
#ifndef BATT_MAX_MILLIVOLTS
  #define BATT_MAX_MILLIVOLTS 4200
#endif
  uint16_t mv = task->getBattMilliVolts();
  int pct = ((mv - BATT_MIN_MILLIVOLTS) * 100) / (BATT_MAX_MILLIVOLTS - BATT_MIN_MILLIVOLTS);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  char batt[8];
  sprintf(batt, "%d%%", pct);
  display.setColor(pct <= 15 ? UIColor::warning_txt : UIColor::title_txt);
  display.drawTextRightAlign(display.width() - 2, 4, batt);
}

class RiftSplashScreen : public UIScreen {
  UITask* _task;
  unsigned long dismiss_after;
  char _version_info[12];

public:
  RiftSplashScreen(UITask* task) : _task(task) {
    const char *ver = FIRMWARE_VERSION;
    const char *dash = strchr(ver, '-');

    int len = dash ? dash - ver : strlen(ver);
    if (len >= sizeof(_version_info)) len = sizeof(_version_info) - 1;
    memcpy(_version_info, ver, len);
    _version_info[len] = 0;

    dismiss_after = millis() + BOOT_SCREEN_MILLIS;
  }

  int render(DisplayDriver& display) override {
    display.setColor(UIColor::title_txt);
    display.setTextSize(3);
    display.drawTextCentered(display.width() / 2, display.height() / 2 - 30, "RIFT");

    display.setColor(UIColor::secondary_txt);
    display.setTextSize(1);
    display.drawTextCentered(display.width() / 2, display.height() / 2 + 4, "RADIO INTELLIGENCE");
    display.drawTextCentered(display.width() / 2, display.height() / 2 + 16, "& FIELD TERMINAL");

    display.setColor(UIColor::secondary_txt);
    display.drawTextCentered(display.width() / 2, display.height() - 20, _version_info);

    return 200;
  }

  void poll() override {
    if (millis() >= dismiss_after) {
      _task->gotoHomeScreen();
    }
  }
};

// MESH: home dashboard - mesh/battery status, node count, link stats, radar ping.
class RiftMeshScreen : public UIScreen {
  UITask* _task;
  NodePrefs* _node_prefs;
  int _tick;

public:
  RiftMeshScreen(UITask* task, NodePrefs* node_prefs)
     : _task(task), _node_prefs(node_prefs), _tick(0) { }

  int render(DisplayDriver& display) override {
    renderTitleBar(display, _task,
        _task->hasConnection() ? "CONNECTED" : (the_mesh.getBLEPin() != 0 ? "PAIRING" : "STANDBY"));

    // radar box, center
    int cx = display.width() / 2;
    int cy = 74;
    display.setColor(UIColor::primary_txt);
    display.drawRect(cx - 50, cy - 40, 100, 80);
    display.drawRect(cx - 33, cy - 27, 66, 54);
    display.drawRect(cx - 16, cy - 13, 32, 26);

    // rotating "blip" around the outer ring (8 discrete positions, non-blocking timer-driven)
    static const int8_t dx[8] = { 0, 35, 50, 35, 0, -35, -50, -35 };
    static const int8_t dy[8] = { -40, -28, 0, 28, 40, 28, 0, -28 };
    int pos = _tick % 8;
    display.setColor(UIColor::corp_blue);
    display.fillRect(cx + dx[pos] - 2, cy + dy[pos] - 2, 4, 4);
    _tick++;

    // node count + link stats
    char tmp[32];
    display.setTextSize(1);
    display.setColor(UIColor::primary_txt);
    sprintf(tmp, "NODES %d", the_mesh.getNumContacts());
    display.drawTextLeftAlign(4, 130, tmp);

    sprintf(tmp, "LINK %.0f / %.0f", radio_driver.getLastRSSI(), radio_driver.getLastSNR());
    display.drawTextRightAlign(display.width() - 4, 130, tmp);

    // radio config line
    display.setColor(UIColor::secondary_txt);
    sprintf(tmp, "%.3fMHz  SF%d  %ddBm", _node_prefs->freq, _node_prefs->sf, _node_prefs->tx_power_dbm);
    display.drawTextCentered(cx, 144, tmp);

    renderNavBar(display, 0);
    return 700;   // non-blocking periodic refresh, drives the radar animation (slow enough to avoid visible flicker from the full-screen redraw)
  }

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) { _task->cycleNavScreen(1); return true; }
    if (c == KEY_PREV || c == KEY_LEFT) { _task->cycleNavScreen(-1); return true; }
    return false;
  }
};

// SYSTEM: live keyboard/trackball diagnostics - proves the Step 2 input
// drivers work end-to-end. Real settings/diagnostics content lands later.
class RiftSystemScreen : public UIScreen {
  UITask* _task;
  char _last_key;

public:
  RiftSystemScreen(UITask* task) : _task(task), _last_key(0) { }

  void onKey(char c) { if (c >= 32 && c < 127) _last_key = c; }

  int render(DisplayDriver& display) override {
    renderTitleBar(display, _task, NAV_LABELS[3]);

    display.setColor(UIColor::primary_txt);
    display.setTextSize(2);
    display.drawTextCentered(display.width() / 2, display.height() / 2 - 30, "SYSTEM");

    display.setColor(UIColor::secondary_txt);
    display.setTextSize(1);
    char tmp[64];
    sprintf(tmp, "last key: %c", _last_key ? _last_key : '-');
    display.drawTextCentered(display.width() / 2, display.height() / 2 - 8, tmp);

#ifdef RIFT_INPUT_KEYBOARD
    sprintf(tmp, "keyboard: %s", rift_keyboard.isPresent() ? "detected" : "not found");
    display.drawTextCentered(display.width() / 2, display.height() / 2 + 8, tmp);

    strcpy(tmp, "I2C:");
    for (uint8_t i = 0; i < rift_keyboard.seenCount(); i++) {
      char hex[6];
      sprintf(hex, " %02X", rift_keyboard.seenAddr(i));
      strcat(tmp, hex);
    }
    if (rift_keyboard.seenCount() == 0) strcat(tmp, " (empty)");
    display.drawTextCentered(display.width() / 2, display.height() / 2 + 20, tmp);
#endif

    display.setColor(UIColor::title_txt);
    display.drawTextCentered(display.width() / 2, display.height() - 34, "ENTER: send advert");

    renderNavBar(display, 3);
    return 300;
  }

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) { _task->cycleNavScreen(1); return true; }
    if (c == KEY_PREV || c == KEY_LEFT) { _task->cycleNavScreen(-1); return true; }
    // Announce ourselves to the mesh. Other nodes cannot decrypt a direct
    // message from a node they've never heard an advert from - they look the
    // sender up in their contacts and silently drop it otherwise - so this is
    // a prerequisite for two-way DMs, not just a nicety.
    if (c == KEY_ENTER) {
      _task->notify(UIEventType::ack);
      _task->showAlert(the_mesh.advert() ? "Advert sent!" : "Advert failed", 1200);
      return true;
    }
    if (c >= 32 && c < 127) { onKey(c); return true; }
    return false;
  }
};

// Placeholder nav screen - visual only, real functionality lands in a later milestone.
class RiftPlaceholderScreen : public UIScreen {
  UITask* _task;
  int _nav_idx;
  const char* _title;
  const char* _detail;

public:
  RiftPlaceholderScreen(UITask* task, int nav_idx, const char* title, const char* detail)
     : _task(task), _nav_idx(nav_idx), _title(title), _detail(detail) { }

  int render(DisplayDriver& display) override {
    renderTitleBar(display, _task, NAV_LABELS[_nav_idx]);

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

class RiftMsgPreviewScreen : public UIScreen {
  UITask* _task;
  int num_unread;
  int _back;    // how far back in the shared log we're previewing

public:
  RiftMsgPreviewScreen(UITask* task) : _task(task) { num_unread = 0; _back = 0; }

  void onNewMsg() {
    if (num_unread < RIFT_MSG_LOG_SIZE) num_unread++;
    _back = 0;
  }

  int render(DisplayDriver& display) override {
    renderTitleBar(display, _task, "MESSAGE");

    char tmp[16];
    sprintf(tmp, "Unread: %d", num_unread);
    display.setColor(UIColor::corp_blue);
    display.setTextSize(1);
    display.drawTextLeftAlign(4, 22, tmp);

    auto p = msg_log.peek(_back);
    if (p == NULL) return 1000;

    display.setCursor(4, 40);
    display.setColor(UIColor::secondary_txt);
    char filtered_origin[sizeof(p->origin)];
    display.translateUTF8ToBlocks(filtered_origin, p->origin, sizeof(filtered_origin));
    display.print(filtered_origin);

    display.setColor(UIColor::primary_txt);
    char filtered_msg[sizeof(p->msg)];
    display.translateUTF8ToBlocks(filtered_msg, p->msg, sizeof(filtered_msg));
    wrapText(filtered_msg, display.width() - 8, 54, &display, 4);

    return 1000;
  }

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) {
      if (_back + 1 < msg_log.count) _back++;
      num_unread--;
      if (num_unread <= 0) {
        num_unread = 0;
        _task->gotoHomeScreen();
      }
      return true;
    }
    if (c == KEY_ENTER) {
      num_unread = 0;
      _task->gotoHomeScreen();
      return true;
    }
    return false;
  }
};

// COMMS: MeshCore text terminal - scrollable history plus a compose line.
// Sends either to the Public channel (flooded, no ACK possible) or direct to a
// chosen contact (ACKed, so delivery state is shown). ESC on an empty line
// opens the target picker.
#define RIFT_PICKER_MAX 24

class RiftCommsScreen : public UIScreen, ContactVisitor {
  UITask* _task;
  char _input[MAX_TEXT_LEN + 1];
  int _len;
  int _scroll;      // 0 = pinned to newest

  // current send target: Public channel, or a contact by pubkey prefix
  bool _target_is_channel;
  uint8_t _target_key[6];
  char _target_name[32];

  // target picker sub-view
  bool _picking;
  int _pick_idx;
  int _pick_scroll;   // index of the first visible row
  struct PickEntry {
    char name[32];
    uint8_t key[6];
  };
  PickEntry _picks[RIFT_PICKER_MAX];
  int _pick_count;

  static const int BODY_TOP = 20;
  static const int BODY_BOTTOM = 194;
  static const int INPUT_Y = 204;

  bool getPublicChannel(ChannelDetails& ch) {
    return the_mesh.getChannel(RIFT_PUBLIC_CHANNEL_IDX, ch) && ch.name[0] != 0;
  }

  // from ContactVisitor - called by scanRecentContacts(), already ordered by
  // last_advert_timestamp descending (most recently heard first)
  void onContactVisit(const ContactInfo& contact) override {
    if (_pick_count >= RIFT_PICKER_MAX) return;
    if (contact.name[0] == 0) return;   // skip the reserved anon slots

    // only nodes that can actually receive a text message - repeaters and
    // sensors have no one reading them
    if (contact.type != ADV_TYPE_CHAT && contact.type != ADV_TYPE_ROOM) return;

    PickEntry* e = &_picks[_pick_count++];
    StrHelper::strncpy(e->name, contact.name, sizeof(e->name));
    memcpy(e->key, contact.id.pub_key, 6);
  }

  void openPicker() {
    _pick_count = 0;
    // scan more than we can show, since repeaters/sensors get filtered out
    the_mesh.scanRecentContacts(0, this);
    _picking = true;
    _pick_idx = 0;
    _pick_scroll = 0;
  }

  void sendToChannel() {
    ChannelDetails ch;
    if (!getPublicChannel(ch)) {
      _task->showAlert("No channel!", 1200);
      return;
    }

    bool ok = the_mesh.sendGroupMessage(the_mesh.getRTCClock()->getCurrentTime(),
                                        ch.channel, the_mesh.getNodeName(),
                                        _input, _len);
    if (ok) {
      char origin[62];
      sprintf(origin, "%s:", the_mesh.getNodeName());
      msg_log.add(the_mesh.getRTCClock()->getCurrentTime(), origin, _input, true);
      clearInput();
    } else {
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
      _task->showAlert("Send failed", 1200);
      return;
    }
    // surface the routing choice - a stale stored path sends DIRECT into the
    // void, which looks identical to success until the ACK never arrives
    _task->showAlert(result == MSG_SEND_SENT_FLOOD ? "Sent (flood)" : "Sent (direct)", 1500);

    char origin[62];
    sprintf(origin, "to %s:", _target_name);
    auto p = msg_log.add(the_mesh.getRTCClock()->getCurrentTime(), origin, _input, true);
    p->expected_ack = expected_ack;
    p->sent_at_ms = millis();
    p->timeout_ms = est_timeout;
    clearInput();
  }

  void clearInput() {
    _input[0] = 0;
    _len = 0;
    _scroll = 0;
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
    if (p->timeout_ms > 0 && millis() > p->sent_at_ms + p->timeout_ms) return "no ack";
    return "...";
  }

  int pickerRows() const { return (BODY_BOTTOM - (BODY_TOP + 4)) / RIFT_LINE_H; }

  // name for row i of the combined list (row 0 is always the Public channel)
  const char* pickRowName(int i, ChannelDetails& ch) {
    if (i == 0) return getPublicChannel(ch) ? ch.name : "Public";
    return _picks[i - 1].name;
  }

  int renderPicker(DisplayDriver& display) {
    renderTitleBar(display, _task, "SELECT TARGET");
    display.setTextSize(1);

    int total = _pick_count + 1;
    int rows = pickerRows();
    int y = BODY_TOP + 4;
    ChannelDetails ch;

    for (int i = _pick_scroll; i < total && i < _pick_scroll + rows; i++, y += RIFT_LINE_H) {
      bool sel = (_pick_idx == i);
      display.setColor(sel ? UIColor::title_txt : UIColor::secondary_txt);
      char filtered[32];
      display.translateUTF8ToBlocks(filtered, pickRowName(i, ch), sizeof(filtered));
      display.setCursor(4, y);
      display.print(sel ? "> " : "  ");
      display.print(filtered);
    }

    if (_pick_count == 0) {
      display.setColor(UIColor::secondary_txt);
      display.drawTextLeftAlign(4, y + RIFT_LINE_H, "(no contacts heard yet)");
    }

    // scroll position indicator when the list doesn't fit
    if (total > rows) {
      display.setColor(UIColor::secondary_txt);
      char pos[16];
      sprintf(pos, "%d/%d", _pick_idx + 1, total);
      display.drawTextRightAlign(display.width() - 4, BODY_TOP + 4, pos);
    }

    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, INPUT_Y, "trackball up/down  ENTER pick  click back");

    renderNavBar(display, 2);
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
    int total = _pick_count + 1;   // +1 for Public
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
      if (_pick_idx == 0) {
        _target_is_channel = true;
      } else {
        _target_is_channel = false;
        PickEntry* e = &_picks[_pick_idx - 1];
        memcpy(_target_key, e->key, 6);
        StrHelper::strncpy(_target_name, e->name, sizeof(_target_name));
      }
      _picking = false;
      return true;
    }
    // no dedicated ESC key on this keyboard - trackball click backs out too
    if (c == KEY_CANCEL || c == KEY_NEXT || c == KEY_PREV) { _picking = false; return true; }
    return true;   // swallow everything else while picking
  }

public:
  RiftCommsScreen(UITask* task)
     : _task(task), _len(0), _scroll(0), _target_is_channel(true),
       _picking(false), _pick_idx(0), _pick_scroll(0), _pick_count(0) {
    _input[0] = 0;
    _target_name[0] = 0;
    memset(_target_key, 0, sizeof(_target_key));
  }

  bool isComposing() const { return _len > 0; }

  void onDelivered(uint32_t ack_hash, uint32_t trip_ms) {
    msg_log.markDelivered(ack_hash, trip_ms);
  }

  int render(DisplayDriver& display) override {
    if (_picking) return renderPicker(display);

    ChannelDetails ch;
    if (_target_is_channel) {
      renderTitleBar(display, _task, getPublicChannel(ch) ? ch.name : "NO CHANNEL");
    } else {
      renderTitleBar(display, _task, _target_name);
    }

    display.setTextSize(1);

    // History, newest at the bottom: lay entries out upward from the input line.
    int avail_px = display.width() - 8;
    int y = BODY_BOTTOM;
    for (int back = _scroll; back < msg_log.count; back++) {
      auto p = msg_log.peek(back);
      if (p == NULL) break;

      char filtered[sizeof(p->msg)];
      display.translateUTF8ToBlocks(filtered, p->msg, sizeof(filtered));

      char head_line[96];
      char filtered_origin[sizeof(p->origin)];
      display.translateUTF8ToBlocks(filtered_origin, p->origin, sizeof(filtered_origin));
      int hh = (p->timestamp / 3600) % 24;
      int mm = (p->timestamp / 60) % 60;

      char ack_buf[16];
      const char* ack = deliveryLabel(p, ack_buf, sizeof(ack_buf));
      if (ack != NULL) {
        sprintf(head_line, "%02d:%02d %s  %s", hh, mm, filtered_origin, ack);
      } else {
        sprintf(head_line, "%02d:%02d %s", hh, mm, filtered_origin);
      }

      int body_lines = wrapText(filtered, avail_px, 0, NULL, 0);
      int block_h = (body_lines + 1) * RIFT_LINE_H;

      y -= block_h;
      if (y < BODY_TOP) break;   // ran out of room going up

      display.setColor(p->outgoing ? UIColor::secondary_txt : UIColor::title_txt);
      display.setCursor(4, y);
      display.print(head_line);

      display.setColor(p->outgoing ? UIColor::secondary_txt : UIColor::primary_txt);
      wrapText(filtered, avail_px, y + RIFT_LINE_H, &display, 4);
    }

    // Compose line - show the tail once the text outgrows one line.
    display.setColor(UIColor::secondary_txt);
    display.drawRect(0, INPUT_Y - 4, display.width(), 1);

    if (_len == 0) {
      display.drawTextRightAlign(display.width() - 4, INPUT_Y, "ENTER: pick target");
    }

    int max_chars = (display.width() - 16) / RIFT_CHAR_W;
    const char* shown = _input;
    if (_len > max_chars) shown = _input + (_len - max_chars);

    display.setColor(UIColor::primary_txt);
    display.setCursor(4, INPUT_Y);
    display.print("> ");
    display.print(shown);
    display.print("_");

    renderNavBar(display, 2);
    return 1000;   // keystrokes force a repaint via _next_refresh; avoid flicker
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
    if (c == 8) {          // backspace - no KEY_* constant for it
      if (_len > 0) _input[--_len] = 0;
      return true;
    }

    if (c >= 32 && c < 127 && _len < MAX_TEXT_LEN) {
      _input[_len++] = c;
      _input[_len] = 0;
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
  rift_trackball.begin();
#endif
#ifdef RIFT_INPUT_KEYBOARD
  rift_keyboard.begin();
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

  splash = new RiftSplashScreen(this);
  msg_preview = new RiftMsgPreviewScreen(this);
  nav_screens[0] = new RiftMeshScreen(this, node_prefs);
  nav_screens[1] = new RiftPlaceholderScreen(this, 1, "RF RADAR", "Wi-Fi / BLE / RF scan");
  nav_screens[2] = new RiftCommsScreen(this);
  nav_screens[3] = new RiftSystemScreen(this);
  nav_idx = 0;
  setCurrScreen(splash);
}

void UITask::cycleNavScreen(int dir) {
  nav_idx = (nav_idx + dir + RIFT_NAV_COUNT) % RIFT_NAV_COUNT;
  setCurrScreen(nav_screens[nav_idx]);
}

void UITask::showAlert(const char* text, int duration_millis) {
  strcpy(_alert, text);
  _alert_expiry = millis() + duration_millis;
}

void UITask::notify(UIEventType t) {
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

void UITask::msgRead(int msgcount) {
  _msgcount = msgcount;
  if (msgcount == 0) {
    gotoHomeScreen();
  }
}

void UITask::newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) {
  _msgcount = msgcount;

  char origin[62];
  if (path_len == 0xFF) {
    sprintf(origin, "(D) %s:", from_name);
  } else {
    sprintf(origin, "(%d) %s:", (uint32_t) path_len, from_name);
  }
  msg_log.add(rtc_clock.getCurrentTime(), origin, text, false);
  ((RiftMsgPreviewScreen *) msg_preview)->onNewMsg();

  // Don't yank the user out of the COMMS terminal - the message is already
  // visible in its history there, and stealing focus would drop a half-typed
  // line. Everywhere else keeps the popup behaviour.
  if (curr != nav_screens[2]) {
    setCurrScreen(msg_preview);
  }

  if (_display != NULL) {
    if (!_display->isOn() && !hasConnection()) {
      _display->turnOn();
    }
    if (_display->isOn()) {
      _auto_off = millis() + AUTO_OFF_MILLIS;
      _next_refresh = 100;
    }
  }
}

void UITask::msgDelivered(uint32_t ack_hash, uint32_t trip_time_millis) {
  ((RiftCommsScreen *) nav_screens[2])->onDelivered(ack_hash, trip_time_millis);
  _next_refresh = 100;   // reflect the new delivery state promptly
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

void UITask::setCurrScreen(UIScreen* c) {
  curr = c;
  _next_refresh = 100;
}

/*
  hardware-agnostic pre-shutdown activity should be done here
*/
void UITask::shutdown(bool restart){

  #ifdef PIN_BUZZER
  buzzer.shutdown();
  uint32_t buzzer_timer = millis();
  while (buzzer.isPlaying() && (millis() - 2500) < buzzer_timer)
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
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_NEXT);
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
#if defined(BACKLIGHT_BTN)
  if (millis() > next_backlight_btn_check) {
    bool touch_state = digitalRead(PIN_BUTTON2);
#if defined(DISP_BACKLIGHT)
    digitalWrite(DISP_BACKLIGHT, !touch_state);
#elif defined(EXP_PIN_BACKLIGHT)
    expander.digitalWrite(EXP_PIN_BACKLIGHT, !touch_state);
#endif
    next_backlight_btn_check = millis() + 300;
  }
#endif

  if (c != 0 && curr) {
    curr->handleInput(c);
    _auto_off = millis() + AUTO_OFF_MILLIS;
    _next_refresh = 100;
  }

  userLedHandler();

#ifdef PIN_BUZZER
  if (buzzer.isPlaying())  buzzer.loop();
#endif

  if (curr) curr->poll();

  if (_display != NULL && _display->isOn()) {
    if (millis() >= _next_refresh && curr) {
      _display->startFrame();
      int delay_millis = curr->render(*_display);
      if (millis() < _alert_expiry) {
        _display->setTextSize(1);
        int y = _display->height() / 3;
        int p = _display->height() / 32;
        _display->setColor(UIColor::popup_bkg);
        _display->fillRect(p, y, _display->width() - p*2, y);
        _display->setColor(UIColor::popup_txt);
        _display->drawRect(p, y, _display->width() - p*2, y);
        _display->drawTextCentered(_display->width() / 2, y + p*3, _alert);
        _next_refresh = _alert_expiry;
      } else {
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
    if (millis() > _auto_off) {
      _display->turnOff();
    }
#endif
  }

#ifdef PIN_VIBRATION
  vibration.loop();
#endif

#ifdef AUTO_SHUTDOWN_MILLIVOLTS
  if (millis() > next_batt_chck) {
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
    _next_refresh = 0;
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
        _next_refresh = 0;
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
  _next_refresh = 0;
#endif
}
