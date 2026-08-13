#include "UITask.h"
#include <helpers/TxtDataHelpers.h>
#include "../MyMesh.h"
#include "target.h"

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

static const char* NAV_LABELS[RIFT_NAV_COUNT] = { "MESH", "NODES", "RADAR", "COMMS", "SYSTEM" };

// channel the COMMS screen starts on; any configured channel can be selected
// from the target picker at runtime
#ifndef RIFT_PUBLIC_CHANNEL_IDX
  #define RIFT_PUBLIC_CHANNEL_IDX 0
#endif

// The T-Deck keyboard has no ESC or dedicated back key, so backspace doubles as
// "one level back" wherever there is no text to delete. UIScreen.h has no
// constant for it - the raw byte is what the co-processor sends.
#define RIFT_KEY_BACK       8

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

// evenly spread directions for the radial plots (cos/sin * 100), shared by the
// RADAR scatter and the CONSTELLATION topology view
static const int8_t RF_DIR_X[16] = { 0, 38, 71, 92, 100, 92, 71, 38, 0, -38, -71, -92, -100, -92, -71, -38 };
static const int8_t RF_DIR_Y[16] = { -100, -92, -71, -38, 0, 38, 71, 92, 100, 92, 71, 38, 0, -38, -71, -92 };

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

    renderNavBar(display, RIFT_NAV_MESH);
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

  // A small action menu rather than hidden letter shortcuts - discoverable, and
  // it leaves the printable keys free for the text fields.
  enum Mode { MENU, EDIT_NAME, CH_NAME, CH_KEY_CHOICE, CH_KEY_ENTRY, CH_SHOW_KEY };
  enum Item { IT_ADVERT, IT_NAME, IT_CHANNEL, IT_COUNT };
  static const char* ITEM_LABELS[IT_COUNT];

  Mode _mode;
  int _sel;
  RiftTextInput _edit;
  char _ch_name[32];       // held while the key is being chosen
  char _ch_key[48];        // generated key, shown so it can be typed elsewhere
  int _key_choice;         // 0 = generate, 1 = enter existing

  void activate() {
    switch (_sel) {
      case IT_ADVERT:
        // Other nodes cannot decrypt a direct message from a node they have
        // never heard an advert from - they look the sender up in their
        // contacts and silently drop it - so this is a prerequisite for
        // two-way DMs, not a nicety.
        _task->notify(UIEventType::ack);
        _task->showAlert(the_mesh.advert() ? "Advert sent!" : "Advert failed", 1200);
        break;

      case IT_NAME:
        _edit.begin(the_mesh.getNodeName(), sizeof(((NodePrefs*)0)->node_name) - 1);
        _mode = EDIT_NAME;
        break;

      case IT_CHANNEL:
        _edit.begin("", sizeof(_ch_name) - 1);
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

    if (kind == 1) {
      _mode = CH_SHOW_KEY;   // let the user read the key off the screen
    } else {
      _mode = MENU;
      _task->showAlert("Channel added", 1500);
    }
  }

  int renderChannelName(DisplayDriver& display) {
    renderTitleBar(display, _task, "NEW CHANNEL");
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
    renderTitleBar(display, _task, "CHANNEL KEY");
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
    renderTitleBar(display, _task, "CHANNEL KEY");
    display.setTextSize(1);
    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, 30, "Paste the base64 key (24 or 44 chars):");
    _edit.render(display, 4, 54, display.width() - 8);
    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, 90, "ENTER add   BACKSPACE delete / back");
    renderNavBar(display, RIFT_NAV_SYSTEM);
    return 1000;
  }

  int renderShowKey(DisplayDriver& display) {
    renderTitleBar(display, _task, "CHANNEL ADDED");
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

  int renderEditName(DisplayDriver& display) {
    renderTitleBar(display, _task, "NODE NAME");
    display.setTextSize(1);

    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, 30, "This is the name other nodes see,");
    display.drawTextLeftAlign(4, 42, "and the sender name on channel messages.");

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
  }

  int render(DisplayDriver& display) override {
    switch (_mode) {
      case EDIT_NAME:      return renderEditName(display);
      case CH_NAME:        return renderChannelName(display);
      case CH_KEY_CHOICE:  return renderKeyChoice(display);
      case CH_KEY_ENTRY:   return renderKeyEntry(display);
      case CH_SHOW_KEY:    return renderShowKey(display);
      default: break;
    }

    renderTitleBar(display, _task, NAV_LABELS[RIFT_NAV_SYSTEM]);
    display.setTextSize(1);

    char tmp[72];

    // who we are, since it is now editable from here
    display.setColor(UIColor::title_txt);
    sprintf(tmp, "node: %s", the_mesh.getNodeName());
    display.drawTextLeftAlign(4, 24, tmp);

    // action menu
    int y = 46;
    for (int i = 0; i < IT_COUNT; i++, y += RIFT_LINE_H) {
      bool sel = (i == _sel);
      display.setColor(sel ? UIColor::title_txt : UIColor::secondary_txt);
      display.setCursor(4, y);
      display.print(sel ? "> " : "  ");
      display.print(ITEM_LABELS[i]);
    }

    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, y + 4, "up/down select   ENTER activate");

    // diagnostics footer
    y = 128;
    display.setColor(UIColor::secondary_txt);
    display.drawRect(0, y - 6, display.width(), 1);

#ifdef RIFT_INPUT_KEYBOARD
    sprintf(tmp, "keyboard: %s", rift_keyboard.isPresent() ? "detected" : "not found");
    display.drawTextLeftAlign(4, y, tmp);
    y += RIFT_LINE_H;

    strcpy(tmp, "I2C:");
    for (uint8_t i = 0; i < rift_keyboard.seenCount(); i++) {
      char hex[6];
      sprintf(hex, " %02X", rift_keyboard.seenAddr(i));
      strcat(tmp, hex);
    }
    if (rift_keyboard.seenCount() == 0) strcat(tmp, " (empty)");
    display.drawTextLeftAlign(4, y, tmp);
    y += RIFT_LINE_H;
#endif

    // free heap matters here: bringing up the Wi-Fi/BLE stacks for RADAR is by
    // far the largest allocation this firmware makes
    sprintf(tmp, "free heap: %u KB", (unsigned) (ESP.getFreeHeap() / 1024));
    display.drawTextLeftAlign(4, y, tmp);
    y += RIFT_LINE_H;

    sprintf(tmp, "power: %s", the_mesh.getNodePrefs() && board.isExternalPowered() ? "external" : "battery");
    display.drawTextLeftAlign(4, y, tmp);
    y += RIFT_LINE_H;

    // why we last restarted - distinguishes a software crash (PANIC) from a
    // power problem (BROWNOUT), which look identical from the outside
    const char* rr;
    switch (esp_reset_reason()) {
      case ESP_RST_POWERON:  rr = "power on"; break;
      case ESP_RST_SW:       rr = "sw restart"; break;
      case ESP_RST_PANIC:    rr = "PANIC (crash)"; break;
      case ESP_RST_INT_WDT:  rr = "int watchdog"; break;
      case ESP_RST_TASK_WDT: rr = "task watchdog"; break;
      case ESP_RST_WDT:      rr = "watchdog"; break;
      case ESP_RST_BROWNOUT: rr = "BROWNOUT (power)"; break;
      case ESP_RST_DEEPSLEEP: rr = "deep sleep"; break;
      case ESP_RST_EXT:      rr = "ext reset"; break;
      default:               rr = "unknown"; break;
    }
    sprintf(tmp, "last reset: %s", rr);
    display.drawTextLeftAlign(4, y, tmp);
    y += RIFT_LINE_H;

#ifdef RIFT_INPUT_TOUCH
    // shown while confirming the raw->display axis mapping is right
    if (rift_touch.isPresent()) {
      sprintf(tmp, "touch: %d,%d", _task->lastTouchX(), _task->lastTouchY());
    } else {
      strcpy(tmp, "touch: not found");
    }
    display.drawTextLeftAlign(4, y, tmp);
#endif

    renderNavBar(display, RIFT_NAV_SYSTEM);
    return 1000;
  }

  // menu rows start at y=46 with RIFT_LINE_H pitch (see render)
  bool handleTouch(int x, int y) override {
    if (_mode != MENU) return false;
    int row = (y - 46) / RIFT_LINE_H;
    if (row >= 0 && row < IT_COUNT) {
      _sel = row;
      activate();
      return true;
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

    if (_mode == CH_SHOW_KEY) {
      if (c == KEY_ENTER || c == RIFT_KEY_BACK || c == KEY_CANCEL) { _mode = MENU; return true; }
      return true;   // keep the key on screen until acknowledged
    }

    if (c == KEY_NEXT || c == KEY_RIGHT) { _task->cycleNavScreen(1); return true; }
    if (c == KEY_PREV || c == KEY_LEFT) { _task->cycleNavScreen(-1); return true; }
    if (c == KEY_UP) { _sel = (_sel + IT_COUNT - 1) % IT_COUNT; return true; }
    if (c == KEY_DOWN) { _sel = (_sel + 1) % IT_COUNT; return true; }
    if (c == KEY_ENTER) { activate(); return true; }
    return false;
  }
};

const char* RiftSystemScreen::ITEM_LABELS[RiftSystemScreen::IT_COUNT] = {
  "Send advert",
  "Edit node name",
  "Add channel",
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
#define RIFT_CONST_MAX 16

class RiftConstellationScreen : public UIScreen {
  UITask* _task;
  AdvertPath _paths[RIFT_CONST_MAX];
  int _plot_x[RIFT_CONST_MAX];   // where each node was drawn, for tap hit-testing
  int _plot_y[RIFT_CONST_MAX];
  int _count;
  int _sel;
  unsigned long _next_refresh;

  static const int CX_Y = 104;   // centre of the plot

  void refresh() {
    int n = the_mesh.getRecentlyHeard(_paths, RIFT_CONST_MAX);
    // getRecentlyHeard always returns max_num entries including empty slots,
    // and it sorts newest-first, so the live ones are a prefix
    int live = 0;
    for (int i = 0; i < n; i++) {
      if (_paths[i].recv_timestamp != 0 && _paths[i].name[0] != 0) live++;
      else break;
    }
    _count = live;
    if (_sel >= _count) _sel = _count > 0 ? _count - 1 : 0;
  }

public:
  RiftConstellationScreen(UITask* task) : _task(task), _count(0), _sel(0), _next_refresh(0) { }

  int render(DisplayDriver& display) override {
    if (millis() >= _next_refresh) {
      refresh();
      _next_refresh = millis() + 3000;
    }

    char tmp[64];
    sprintf(tmp, "%d HEARD", _count);
    renderTitleBar(display, _task, tmp);

    int cx = display.width() / 2;
    int cy = CX_Y;

    // hop rings
    display.setColor(UIColor::secondary_txt);
    for (int r = 1; r <= 3; r++) {
      int rad = r * 26;
      display.drawRect(cx - rad, cy - rad, rad * 2, rad * 2);
    }

    // us
    display.setColor(UIColor::title_txt);
    display.fillRect(cx - 2, cy - 2, 5, 5);
    display.drawTextCentered(cx, cy + 8, "YOU");

    uint32_t now = the_mesh.getRTCClock()->getCurrentTime();

    for (int i = 0; i < _count; i++) {
      AdvertPath* p = &_paths[i];

      int hops = p->path_len;         // 0 == heard directly
      int ring = hops + 1;
      if (ring > 3) ring = 3;
      int rad = ring * 26;

      // direction from the first hop, so nodes reached through the same
      // repeater cluster together; direct nodes spread by their own key
      uint8_t branch = (hops > 0) ? p->path[0] : p->pubkey_prefix[0];
      int d = branch & 15;

      int x = cx + (rad * RF_DIR_X[d]) / 100;
      int y = cy + (rad * RF_DIR_Y[d]) / 100;
      _plot_x[i] = x;
      _plot_y[i] = y;

      // recency: solid if heard in the last few minutes, dim if stale
      uint32_t age = (now > p->recv_timestamp) ? (now - p->recv_timestamp) : 0;
      bool fresh = age < 300;   // 5 minutes

      bool sel = (i == _sel);
      display.setColor(sel ? UIColor::warning_txt
                           : (fresh ? UIColor::primary_txt : UIColor::secondary_txt));

      // link line back toward centre, then the node itself
      display.fillRect(cx + (x - cx) / 2, cy + (y - cy) / 2, 1, 1);
      if (sel) {
        display.fillRect(x - 3, y - 3, 7, 7);
      } else {
        display.fillRect(x - 2, y - 2, 4, 4);
      }
    }

    if (_count == 0) {
      display.setColor(UIColor::secondary_txt);
      display.drawTextCentered(cx, cy + 40, "no adverts heard yet");
    }

    // detail for the selected node
    int y = 186;
    display.setColor(UIColor::secondary_txt);
    display.drawRect(0, y - 6, display.width(), 1);

    if (_count > 0) {
      AdvertPath* p = &_paths[_sel];
      char filtered[sizeof(p->name)];
      display.translateUTF8ToBlocks(filtered, p->name, sizeof(filtered));

      display.setColor(UIColor::title_txt);
      display.drawTextEllipsized(4, y, 180, filtered);

      display.setColor(UIColor::secondary_txt);
      if (p->path_len == 0) {
        strcpy(tmp, "direct");
      } else {
        sprintf(tmp, "%d hop%s", p->path_len, p->path_len == 1 ? "" : "s");
      }
      display.drawTextRightAlign(display.width() - 4, y, tmp);

      uint32_t age = (now > p->recv_timestamp) ? (now - p->recv_timestamp) : 0;
      if (age < 60) sprintf(tmp, "heard %lus ago", (unsigned long) age);
      else if (age < 3600) sprintf(tmp, "heard %lum ago", (unsigned long) (age / 60));
      else sprintf(tmp, "heard %luh ago", (unsigned long) (age / 3600));
      display.drawTextLeftAlign(4, y + RIFT_LINE_H, tmp);

      sprintf(tmp, "%d/%d", _sel + 1, _count);
      display.drawTextRightAlign(display.width() - 4, y + RIFT_LINE_H, tmp);
    }

    renderNavBar(display, RIFT_NAV_NODES);
    return 2000;
  }

  // tapping a node in the plot area selects the nearest one
  bool handleTouch(int x, int y) override {
    if (_count == 0 || y > 170) return false;
    int best = -1, best_d = 0;
    for (int i = 0; i < _count; i++) {
      int dx = x - _plot_x[i], dy = y - _plot_y[i];
      int d = dx * dx + dy * dy;
      if (best < 0 || d < best_d) { best = i; best_d = d; }
    }
    if (best >= 0 && best_d < 40 * 40) { _sel = best; return true; }
    return false;
  }

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) { _task->cycleNavScreen(1); return true; }
    if (c == KEY_PREV || c == KEY_LEFT) { _task->cycleNavScreen(-1); return true; }
    if (c == KEY_UP) {
      if (_count > 0) _sel = (_sel + _count - 1) % _count;
      return true;
    }
    if (c == KEY_DOWN) {
      if (_count > 0) _sel = (_sel + 1) % _count;
      return true;
    }
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
  if (slot < 0) {
    if (rf_count < RIFT_RF_MAX) {
      slot = rf_count++;
    } else {
      int weakest = 0;   // table full - let stronger signals displace weaker
      for (int i = 1; i < rf_count; i++) {
        if (rf_table[i].rssi < rf_table[weakest].rssi) weakest = i;
      }
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
class RiftRadarScreen : public UIScreen {
  UITask* _task;

  enum ScanState { OFF, START_WIFI, WIFI_RUNNING, START_BLE, BLE_RUNNING, STOPPING };
  enum View { VIEW_SCATTER, VIEW_WATERFALL };
  ScanState _state;
  View _view;
  bool _want_active;   // set by screen changes; acted on from the main loop
  bool _wifi_up, _ble_up;
  // Teardown completion must be tracked separately: _ble_up deliberately stays
  // true after teardown (BLEDevice::deinit is avoided), so using it as the
  // "needs teardown" test would restart the cycle forever.
  bool _torn_down;
  unsigned long _next_step;
  int _scroll;
  int _last_n;

  static const int LIST_TOP = 118;
  static const int LIST_BOTTOM = 208;

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
     : _task(task), _state(OFF), _view(VIEW_SCATTER), _want_active(false),
       _wifi_up(false), _ble_up(false), _torn_down(true),
       _next_step(0), _scroll(0), _last_n(0) { }

  // Only records intent. Screen changes can originate from a mesh callback
  // (an incoming message switches to the preview screen), and tearing the BT
  // controller down from inside the LoRa receive path crashes the device -
  // so the actual work happens in service(), on the main loop.
  void setActive(bool active) { _want_active = active; }

  // Ask the radios to stop, but do NOT deinit yet - BLEDevice::deinit() while a
  // scan is still winding down panics the device. The actual teardown happens
  // in the STOPPING state after a grace period.
  void beginTeardown() {
    if (_ble_up) {
      BLEDevice::getScan()->setAdvertisedDeviceCallbacks(NULL);   // no late callbacks
      BLEDevice::getScan()->stop();
    }
    _state = STOPPING;
    _next_step = millis() + RIFT_SCAN_STOP_GRACE_MILLIS;
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
    _state = OFF;
    _torn_down = true;
  }

  // Driven every main-loop iteration, whichever screen is showing, so that
  // teardown still happens after the user navigates away.
  void service() {
    if (!_want_active) {
      if (_state == STOPPING) {
        if (millis() >= _next_step) finishTeardown();
      } else if (!_torn_down) {
        beginTeardown();
      }
      return;
    }
    if (_state == OFF || _state == STOPPING) {
      _state = START_WIFI;   // came back before teardown finished
      _next_step = 0;
      _torn_down = false;
    }

    if (millis() < _next_step) return;

    switch (_state) {
      case START_WIFI:
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
        beginBle();
        _state = BLE_RUNNING;
        break;

      case BLE_RUNNING:
        if (ble_scan_done) {
          BLEDevice::getScan()->clearResults();   // keep the internal map bounded
          rfAgeOut();
          _state = START_WIFI;
          _next_step = millis() + RIFT_SCAN_GAP_MILLIS;
        }
        break;

      default:
        break;
    }
  }

  // colour ramp for observed signal strength; 0 means nothing heard
  static ColorVal wfColor(int8_t rssi) {
    if (rssi == 0) return 0;
    if (rssi >= -55) return UIColor::warning_txt;     // strong
    if (rssi >= -70) return UIColor::corp_blue;       // moderate
    if (rssi >= -85) return UIColor::primary_txt;     // weak
    return UIColor::secondary_txt;                    // barely there
  }

  int renderWaterfall(DisplayDriver& display) {
    const char* status = (_state == OFF) ? "IDLE" : "WATERFALL";
    renderTitleBar(display, _task, status);

    display.setTextSize(1);

    // channels across, time down (newest at top)
    const int left = 22;
    const int top = 30;
    const int cell_w = 21;
    const int cell_h = 4;
    const int channels = 13;

    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(2, top - 12, "now");

    for (int s = 0; s < wf_count && s < RIFT_WF_SLICES; s++) {
      int idx = (wf_head - s + RIFT_WF_SLICES * 2) % RIFT_WF_SLICES;
      int y = top + s * cell_h;
      if (y + cell_h > 186) break;

      for (int ch = 1; ch <= channels; ch++) {
        ColorVal c = wfColor(wf_hist[idx][ch]);
        if (c == 0) continue;   // nothing heard - leave background
        display.setColor(c);
        display.fillRect(left + (ch - 1) * cell_w, y, cell_w - 2, cell_h - 1);
      }
    }

    if (wf_count == 0) {
      display.setColor(UIColor::secondary_txt);
      display.drawTextCentered(display.width() / 2, top + 40, "waiting for first sweep...");
    }

    // channel axis
    display.setColor(UIColor::secondary_txt);
    char tmp[8];
    for (int ch = 1; ch <= channels; ch += 2) {
      sprintf(tmp, "%d", ch);
      display.drawTextCentered(left + (ch - 1) * cell_w + cell_w / 2, 190, tmp);
    }
    display.drawTextCentered(display.width() / 2, 202, "WIFI CHANNEL");

    renderNavBar(display, RIFT_NAV_RADAR);
    return 700;
  }

  int render(DisplayDriver& display) override {
    if (_view == VIEW_WATERFALL) return renderWaterfall(display);

    const char* status = "LIVE";
    if (_state == OFF) status = "IDLE";
    else if (!_wifi_up && !_ble_up) status = "INITIALISING";
    renderTitleBar(display, _task, status);

    // snapshot under the lock, then draw without holding it
    RfContact snap[RIFT_RF_MAX];
    int n;
    portENTER_CRITICAL(&rf_mux);
    n = rf_count;
    memcpy(snap, rf_table, sizeof(RfContact) * n);
    portEXIT_CRITICAL(&rf_mux);
    _last_n = n;

    // entries age out between renders, so a stale offset would leave the list
    // drawing nothing at all
    if (_scroll >= n) _scroll = (n > 0) ? n - 1 : 0;

    int wifi_n = 0, ble_n = 0;
    for (int i = 0; i < n; i++) {
      if (snap[i].is_wifi) wifi_n++; else ble_n++;
    }

    // scatter: distance from centre derived from signal strength
    int cx = display.width() / 2;
    int cy = 66;
    display.setColor(UIColor::secondary_txt);
    display.drawRect(cx - 46, cy - 44, 92, 88);
    display.drawRect(cx - 24, cy - 23, 48, 46);
    display.setColor(UIColor::title_txt);
    display.fillRect(cx - 1, cy - 1, 3, 3);   // us

    for (int i = 0; i < n; i++) {
      int s = snap[i].rssi;
      if (s < -100) s = -100;
      if (s > -30) s = -30;
      int radius = ((-s - 30) * 42) / 70;   // -30dBm near centre, -100 at edge
      int d = i & 15;
      display.setColor(snap[i].is_wifi ? UIColor::corp_blue : UIColor::primary_txt);
      display.fillRect(cx + (radius * RF_DIR_X[d]) / 100, cy + (radius * RF_DIR_Y[d]) / 100, 2, 2);
    }

    char tmp[64];
    display.setTextSize(1);
    display.setColor(UIColor::primary_txt);
    sprintf(tmp, "%d WIFI   %d BLE", wifi_n, ble_n);
    display.drawTextCentered(cx, 112, tmp);

    // live heap while the RF stacks are up - this is where memory gets tight
    display.setColor(UIColor::secondary_txt);
    sprintf(tmp, "%uK", (unsigned) (ESP.getFreeHeap() / 1024));
    display.drawTextRightAlign(display.width() - 4, 112, tmp);

    // strongest first
    for (int i = 0; i < n - 1; i++) {
      for (int j = i + 1; j < n; j++) {
        if (snap[j].rssi > snap[i].rssi) {
          RfContact t = snap[i]; snap[i] = snap[j]; snap[j] = t;
        }
      }
    }

    int y = LIST_TOP;
    display.setColor(UIColor::secondary_txt);
    display.drawTextLeftAlign(4, y, "strongest:");
    display.drawTextRightAlign(display.width() - 4, y, "ENTER: waterfall");
    y += RIFT_LINE_H;

    int type_x = display.width() - 108;
    for (int i = _scroll; i < n && y < LIST_BOTTOM; i++, y += RIFT_LINE_H) {
      char filtered[sizeof(snap[i].name)];
      display.translateUTF8ToBlocks(filtered, snap[i].name, sizeof(filtered));

      display.setColor(snap[i].is_wifi ? UIColor::corp_blue : UIColor::primary_txt);
      display.drawTextEllipsized(4, y, type_x - 8, filtered);

      display.setColor(UIColor::secondary_txt);
      if (snap[i].is_wifi) {
        sprintf(tmp, "WIFI c%d%s", snap[i].channel, snap[i].encrypted ? " *" : "");
      } else {
        strcpy(tmp, "BLE");
      }
      display.drawTextLeftAlign(type_x, y, tmp);

      sprintf(tmp, "%d", snap[i].rssi);
      display.drawTextRightAlign(display.width() - 4, y, tmp);
    }

    if (n == 0) {
      display.setColor(UIColor::secondary_txt);
      display.drawTextCentered(cx, LIST_TOP + RIFT_LINE_H * 2, "listening...");
    }

    renderNavBar(display, RIFT_NAV_RADAR);
    return 700;   // coarse: the TFT shares its SPI bus with the LoRa radio
  }

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) { _task->cycleNavScreen(1); return true; }
    if (c == KEY_PREV || c == KEY_LEFT) { _task->cycleNavScreen(-1); return true; }
    // both views are fed by the same scan cycle, so this is just a display swap
    if (c == KEY_ENTER) {
      _view = (_view == VIEW_SCATTER) ? VIEW_WATERFALL : VIEW_SCATTER;
      return true;
    }
    // waterfall is a level down from the scatter view
    if (c == RIFT_KEY_BACK && _view == VIEW_WATERFALL) {
      _view = VIEW_SCATTER;
      return true;
    }
    if (_view == VIEW_SCATTER) {
      if (c == KEY_UP) { if (_scroll > 0) _scroll--; return true; }
      if (c == KEY_DOWN) { if (_scroll + 1 < _last_n) _scroll++; return true; }
    }
    return false;
  }
};

#endif   // RIFT_RADAR

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
    // the preview is an overlay on whatever you were doing - back dismisses it
    if (c == RIFT_KEY_BACK) {
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

  // current send target: a group channel by index, or a contact by pubkey prefix
  bool _target_is_channel;
  uint8_t _target_channel_idx;
  uint8_t _target_key[6];
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

  static const int BODY_TOP = 20;
  static const int BODY_BOTTOM = 194;
  static const int INPUT_Y = 204;

  bool getTargetChannel(ChannelDetails& ch) {
    return the_mesh.getChannel(_target_channel_idx, ch) && ch.name[0] != 0;
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
    e->is_channel = false;
    e->channel_idx = 0;
    memcpy(e->key, contact.id.pub_key, 6);
  }

  void openPicker() {
    _pick_count = 0;

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
    // subtract rather than add: millis() wraps at ~49.7 days, and
    // (sent_at + timeout) would overflow and report a fresh send as timed out
    if (p->timeout_ms > 0 && millis() - p->sent_at_ms > p->timeout_ms) return "no ack";
    return "...";
  }

  int pickerRows() const { return (BODY_BOTTOM - (BODY_TOP + 4)) / RIFT_LINE_H; }

  int renderPicker(DisplayDriver& display) {
    renderTitleBar(display, _task, "SELECT TARGET");
    display.setTextSize(1);

    int total = _pick_count;
    int rows = pickerRows();
    int y = BODY_TOP + 4;

    for (int i = _pick_scroll; i < total && i < _pick_scroll + rows; i++, y += RIFT_LINE_H) {
      bool sel = (_pick_idx == i);
      display.setColor(sel ? UIColor::title_txt : UIColor::secondary_txt);
      char filtered[32];
      display.translateUTF8ToBlocks(filtered, _picks[i].name, sizeof(filtered));
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
  RiftCommsScreen(UITask* task)
     : _task(task), _len(0), _scroll(0),
       _target_is_channel(true), _target_channel_idx(RIFT_PUBLIC_CHANNEL_IDX),
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
      // read the name live rather than trusting the cached one, so a channel
      // reconfigured from the companion app doesn't show a stale label
      renderTitleBar(display, _task, getTargetChannel(ch) ? ch.name : "NO CHANNEL");
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

    renderNavBar(display, RIFT_NAV_COMMS);
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
    // backspace deletes while composing; with nothing to delete this is
    // already the top level, so there is nowhere further back to go
    if (c == RIFT_KEY_BACK) {
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
#ifdef RIFT_INPUT_TOUCH
  rift_touch.begin();
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
  nav_screens[RIFT_NAV_MESH] = new RiftMeshScreen(this, node_prefs);
  nav_screens[RIFT_NAV_NODES] = new RiftConstellationScreen(this);
#ifdef RIFT_RADAR
  nav_screens[RIFT_NAV_RADAR] = new RiftRadarScreen(this);
#else
  nav_screens[RIFT_NAV_RADAR] = new RiftPlaceholderScreen(this, RIFT_NAV_RADAR, "RF RADAR", "Wi-Fi / BLE / RF scan");
#endif
  nav_screens[RIFT_NAV_COMMS] = new RiftCommsScreen(this);
  nav_screens[RIFT_NAV_SYSTEM] = new RiftSystemScreen(this);
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
  if (curr != nav_screens[RIFT_NAV_COMMS]) {
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
  ((RiftCommsScreen *) nav_screens[RIFT_NAV_COMMS])->onDelivered(ack_hash, trip_time_millis);
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
#ifdef RIFT_RADAR
  // UIScreen has no enter/exit hook and this is the single funnel every screen
  // change passes through, so RADAR learns about focus here. This only records
  // intent - see RiftRadarScreen::setActive().
  //
  // Only real navigation counts: a transient popup (an incoming message
  // switching to the preview screen) must not tear the RF stacks down, both
  // because the user is still "on" RADAR and because doing it mid-scan used to
  // panic the device.
  if (nav_screens[RIFT_NAV_RADAR] != NULL) {
    bool is_nav = false;
    for (int i = 0; i < RIFT_NAV_COUNT; i++) {
      if (c == nav_screens[i]) { is_nav = true; break; }
    }
    if (is_nav) ((RiftRadarScreen *) nav_screens[RIFT_NAV_RADAR])->setActive(c == nav_screens[RIFT_NAV_RADAR]);
  }
#endif
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
          nav_idx = col;
          setCurrScreen(nav_screens[nav_idx]);
        }
        _auto_off = millis() + AUTO_OFF_MILLIS;
      } else if (curr) {
        curr->handleTouch(tx, ty);
        _auto_off = millis() + AUTO_OFF_MILLIS;
        _next_refresh = 100;
      }
    }
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

#ifdef RIFT_RADAR
  // serviced unconditionally, not via curr->poll(), so RF teardown still runs
  // after navigating away from RADAR
  if (nav_screens[RIFT_NAV_RADAR] != NULL) ((RiftRadarScreen *) nav_screens[RIFT_NAV_RADAR])->service();
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
