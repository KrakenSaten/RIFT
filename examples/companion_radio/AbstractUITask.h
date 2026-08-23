#pragma once

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/UIScreen.h>
#include <helpers/SensorManager.h>
#include <helpers/MultiSerialInterface.h>
#include <Arduino.h>

#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
#endif

#include "NodePrefs.h"

enum class UIEventType {
    none,
    contactMessage,
    channelMessage,
    roomMessage,
    newContactMessage,
    ack
};

class AbstractUITask {
protected:
  mesh::MainBoard* _board;
  MultiSerialInterface* _interfaceManager;
  bool _connected;

  AbstractUITask(mesh::MainBoard* board, MultiSerialInterface* interfaceManager) : _board(board), _interfaceManager(interfaceManager) {
    _connected = false;
  }

public:
  void setHasConnection(bool connected) { _connected = connected; }
  bool hasConnection() const { return _connected; }
  uint16_t getBattMilliVolts() const { return _board->getBattMilliVolts(); }
  bool isBluetoothEnabled() const { return _interfaceManager->isBluetoothEnabled(); }
  void enableBluetooth() { _interfaceManager->enableBluetooth(); }
  void disableBluetooth() { _interfaceManager->disableBluetooth(); }
  virtual void msgRead(int msgcount) = 0;
  virtual void newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) = 0;

  // As newMsg(), plus which conversation the message belongs to.
  //
  // newMsg() carries a display name and nothing else, so a UI that groups messages
  // by conversation has to recover the identity by matching that name against its
  // configured channels and contacts. RIFT did exactly that, and the function doing
  // it shipped wrong - every row drew in the fallback colour and three readings of
  // the code did not find it. The caller in MyMesh has the channel index or the
  // sender's public key in hand at both sites and was throwing it away.
  //
  // Kinds are 0 unknown, 1 channel, 2 direct, 3 room, matching RIFT_CONV_* in
  // ui-rift/RiftLogic.h. Passed as plain integers rather than a struct so this
  // shared header stays independent of any one UI, and peer points at six bytes of
  // public key prefix or is NULL.
  //
  // Default forwards, so ui-new, ui-orig and ui-tiny need no change and lose
  // nothing: they were never told the conversation and never asked.
  virtual void newMsgConv(uint8_t path_len, const char* from_name, const char* text,
                          int msgcount, uint8_t conv_kind, uint8_t channel_idx,
                          const uint8_t* peer) {
    (void) conv_kind; (void) channel_idx; (void) peer;
    newMsg(path_len, from_name, text, msgcount);
  }

  // an outgoing direct message was acknowledged by its recipient. Intentionally
  // not pure - UIs that don't surface delivery state need no implementation.
  virtual void msgDelivered(uint32_t ack_hash, uint32_t trip_time_millis) { }
  virtual void notify(UIEventType t = UIEventType::none) = 0;

  // Whether this UI wants message notifications while a companion app is connected.
  //
  // The message paths in MyMesh suppress notify() when the serial link is up, on the
  // reasonable assumption that an attached app is doing the notifying. That holds for
  // upstream's UIs, which are companions to a phone. It does not hold for a UI that
  // is the client itself: isConnected() becomes true as soon as any host opens the
  // port, so a device sitting on USB power went silent - which is exactly when it was
  // being tested.
  //
  // Defaults to upstream's behaviour, so nothing changes for ui-new, ui-orig or
  // ui-tiny.
  virtual bool notifiesWhileConnected() const { return false; }
  virtual void loop() = 0;
};
