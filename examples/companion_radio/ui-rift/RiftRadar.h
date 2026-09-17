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

#endif   // RIFT_RADAR
