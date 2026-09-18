#pragma once

#include <Arduino.h>

#include "UITask.h"         // rfWatchCheck raises an alert through the task
#include "RiftEventLog.h"   // riftLogf, for the watch transitions

// RADAR's service half: what the radios find, and what is being watched for.
//
// Step one of the split the review asks for, and deliberately not all of it. What is
// here is everything that is not the screen - the result table shared between the
// Bluedroid task and the loop task, the waterfall, the proximity watch list, the BLE
// callbacks, and the constants that time a sweep. Nothing was edited on the way
// across.
//
// What is NOT here is the scan state machine. It still lives in RiftRadarScreen,
// still driven by onEnter and onLeave, so scanning still stops when the screen is
// left. Moving that is moving who owns the lifecycle, which is a real change rather
// than a move, and it is the thing that has to happen before RADAR can watch
// anything in the background. It gets its own commit so that this one can be checked
// by diffing.
//
// The table is touched from two cores. BLE advertisement callbacks fire on the
// Bluedroid task while rendering happens on the loop task, which is why every access
// is inside the spinlock - see the note above rf_table.

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

#ifdef RIFT_RADAR

// Shared scan result table. BLE advertisement callbacks fire on the Bluedroid
// task (core 0) while rendering happens on the loop task (core 1), so every
// touch of this table is inside the spinlock.
//
// 88 bytes a finding: 44 for RfContact, doubled because the render keeps a
// snapshot of the same size. Measured by building at 48 and 96 and taking the
// difference. 48 was reported full almost all the time in an ordinary urban
// place, which makes the number a floor on what is audible rather than a count of
// it - and RIFT_RF_AGE_MILLIS already forgets anything unheard for 45s, so this is
// a window on what is around right now, not a log that fills up.
//
// The constraint used to be the stack, not RAM: the render's snapshot was a local,
// 2.1KB of the loop task's 8KB, so doubling this would have taken half the stack
// before it bought a single extra finding. That snapshot is static now, which is
// what makes this number free to raise.
//
// Not taken past 96 yet for one reason: RADAR is the screen that scans Wi-Fi and
// BLE, so it is where the heap is under most pressure, and FREE HEAP on the
// diagnostics screen has not been read while a scan is running. 128 would cost
// 67.6% against 66.7%, so the room is there if that number says it is safe.
#define RIFT_RF_MAX 96

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


// ------------------------------------------------------------- the scan itself
//
// Step two of the split. The state machine used to live in RiftRadarScreen and was
// reached from UITask::loop() by casting the nav screen back to its own type - so the
// loop already drove it on every iteration whichever screen was up, and had to,
// because teardown has to keep running after the user navigates away.
//
// What changes here is who owns it, not when it runs. The screen now says only
// whether scanning is wanted; the service decides what that means and how to get
// there. The bodies below moved across unedited.
//
// This is what point 6 of the review needs before it can be written. Watched devices
// are useless while COMMS is up because scanning stops when RADAR is left, and that
// stop is onLeave clearing a flag - a screen deciding a radio's lifecycle. The flag
// is still cleared from onLeave today, so behaviour has not moved; what has moved is
// that it is now a request to a service rather than a field of the thing drawing.
class RiftRadarService {
  enum ScanState { OFF, START_WIFI, WIFI_RUNNING, START_BLE, BLE_RUNNING, STOPPING };

  UITask* _task = NULL;
  ScanState _state = OFF;
  bool _want_active = false;
  bool _wifi_up = false, _ble_up = false;
  // Teardown completion must be tracked separately: _ble_up deliberately stays
  // true after teardown (BLEDevice::deinit is avoided), so using it as the
  // "needs teardown" test would restart the cycle forever.
  bool _torn_down = false;
  // wrap-safe pacing: when the wait started, and how long to wait (0 = ready
  // now). A future deadline breaks at the millis() wrap, and here it would leave
  // the state machine spinning.
  unsigned long _wait_since = 0;
  unsigned long _wait_ms = 0;
  unsigned long _ble_started = 0;   // when the running BLE scan was started
  // Whether any sweep has finished since the last teardown. The screen reads it to
  // tell "listening, nothing yet" from "nothing is there".
  bool _scanned_once = false;

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

 public:
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

  // Called once from UITask::begin. rfWatchCheck raises the proximity alert through
  // the task, which is the service's one reach back into the UI.
  void attach(UITask* task) { _task = task; }

  // The radio source changed under a running sweep. Every line of this is about
  // radios and the cycle, which is why it lives here and not in the screen that
  // happens to offer the key.
  //
  // Wi-Fi is genuinely powered down when it is not wanted; BLE is only stopped,
  // because BLEDevice::deinit() panics in this ESP32 core once a scan has been
  // active - see finishTeardown(), where the same limit applies. Stopped is enough:
  // nothing is collected and nothing is transmitted.
  void sourceChanged() {
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
  }

  // Intent only. Screen changes can originate from a mesh callback, and tearing the
  // BT controller down from inside the LoRa receive path crashes the device - so the
  // work happens in service(), on the main loop.
  void setWanted(bool on) { _want_active = on; }

  bool idle() const { return _state == OFF; }
  bool anyRadioUp() const { return _wifi_up || _ble_up; }
  bool scannedOnce() const { return _scanned_once; }
};

inline RiftRadarService& riftRadarSvc() {
  static RiftRadarService s;
  return s;
}

#endif   // RIFT_RADAR
