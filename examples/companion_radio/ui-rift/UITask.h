#pragma once

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/UIScreen.h>
#include <helpers/SensorManager.h>
#include <helpers/MultiSerialInterface.h>
#include <Arduino.h>
#include <helpers/sensors/LPPDataHelpers.h>

#ifndef LED_STATE_ON
  #define LED_STATE_ON 1
#endif

#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
#endif
#ifdef PIN_VIBRATION
  #include <helpers/ui/GenericVibration.h>
#endif

#include "../AbstractUITask.h"
#include "../NodePrefs.h"

// Design palette. The shared UIColor class has no slot for the roles this design
// needs - dim, rule, a status green, and an accent that means something different
// on a light field - so RIFT carries its own table. riftApplyPalette() also
// assigns the UIColor statics, so screens not yet migrated stay coherent in both
// modes instead of half-changing.
struct RiftPalette {
  ColorVal bg, bar, fg, mid, dim, rule, accent, accent_txt, ok;
};
extern RiftPalette rift_pal;
extern bool rift_day_mode;
void riftApplyPalette(bool day);

// Unread count mirrored out of UITask so the nav bar - a free function called
// from thirteen places - can draw its dot without changing every signature.
extern int rift_nav_unread;

// Boot phase timing. setup() runs with no watchdog and several of its steps can
// block for a long time - the filesystem mount most of all - so a slow boot gives
// no clue which step is responsible. These marks record when each phase finished
// and are shown on SYSTEM, which is the only console a field device has.
#define RIFT_BOOT_MARKS 16
struct RiftBootMark { const char* name; uint32_t at_ms; };
extern RiftBootMark rift_boot_marks[RIFT_BOOT_MARKS];
extern uint8_t rift_boot_mark_count;
void riftBootMark(const char* name);
#define RIFT_MARK(n)  riftBootMark(n)

// Draws the RIFT boot screen. Lives outside UITask because main.cpp has to draw
// it during setup(), before the UI task exists - see the SPIFFS format probe
// there. Pass a status line, or NULL for none. Caller owns startFrame/endFrame.
void riftDrawBootScreen(DisplayDriver& display, const char* status);

// nav slot indices - named, because several call sites reach into
// nav_screens[] directly and silent index drift is easy otherwise
#define RIFT_NAV_MESH    0
#define RIFT_NAV_NODES   1
#define RIFT_NAV_RADAR   2
#define RIFT_NAV_COMMS   3
#define RIFT_NAV_SYSTEM  4
#define RIFT_NAV_COUNT   5

class UITask : public AbstractUITask {
  DisplayDriver* _display;
  SensorManager* _sensors;
#ifdef PIN_BUZZER
  genericBuzzer buzzer;
#endif
#ifdef PIN_VIBRATION
  GenericVibration vibration;
#endif
  unsigned long _next_refresh, _auto_off;
  NodePrefs* _node_prefs;
  char _alert[80];
  unsigned long _alert_expiry;
  int _msgcount;
  unsigned long ui_started_at, next_batt_chck;
  int next_backlight_btn_check = 0;
#ifdef PIN_STATUS_LED
  int led_state = 0;
  int next_led_change = 0;
  int last_led_increment = 0;
#endif

#ifdef PIN_USER_BTN_ANA
  unsigned long _analogue_pin_read_millis = millis();
#endif

#ifdef RIFT_INPUT_TOUCH
  int _touch_x = -1, _touch_y = -1;   // last tap, shown on SYSTEM while calibrating
public:
  int lastTouchX() const { return _touch_x; }
  int lastTouchY() const { return _touch_y; }
private:
#endif

  UIScreen* splash;
  UIScreen* msg_preview;
  UIScreen* nav_screens[RIFT_NAV_COUNT];   // indexed by RIFT_NAV_*
  int nav_idx;
  UIScreen* curr;

  void userLedHandler();

  // Button action handlers
  char checkDisplayOn(char c);
  char handleLongPress(char c);
  char handleDoubleClick(char c);
  char handleTripleClick(char c);

  void setCurrScreen(UIScreen* c);

public:

  UITask(mesh::MainBoard* board, MultiSerialInterface* serial) : AbstractUITask(board, serial), _display(NULL), _sensors(NULL) {
    next_batt_chck = _next_refresh = 0;
    ui_started_at = 0;
    nav_idx = 0;
    curr = NULL;
  }
  void begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs);

  void gotoHomeScreen() { nav_idx = RIFT_NAV_MESH; setCurrScreen(nav_screens[RIFT_NAV_MESH]); }
  void cycleNavScreen(int dir);
  // NODES offers ENTER: DM; this switches to COMMS with that node selected
  void startDirectMessage(const uint8_t* key6);
  void showAlert(const char* text, int duration_millis);
  int  getMsgCount() const { return _msgcount; }
  bool hasDisplay() const { return _display != NULL; }
  bool isButtonPressed() const;

  bool isBuzzerQuiet() {
#ifdef PIN_BUZZER
    return buzzer.isQuiet();
#else
    return true;
#endif
  }

  void toggleBuzzer();
  bool getGPSState();
  // whether GPS hardware was detected at boot; the "gps" setting only exists
  // when the sensor manager actually saw a receiver on the UART
  bool hasGPSHardware();
  void toggleGPS();


  // from AbstractUITask
  void msgRead(int msgcount) override;
  void newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) override;
  void msgDelivered(uint32_t ack_hash, uint32_t trip_time_millis) override;
  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;

  void shutdown(bool restart = false);
};
