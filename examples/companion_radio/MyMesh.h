#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include "AbstractUITask.h"

/*------------ Frame Protocol --------------*/
#define FIRMWARE_VER_CODE 13

#ifndef FIRMWARE_BUILD_DATE
#define FIRMWARE_BUILD_DATE "9 Aug 2026"
#endif

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "v1.17.0"
#endif

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
#include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
#include <LittleFS.h>
#elif defined(ESP32)
#include <SPIFFS.h>
#endif

#include "DataStore.h"
#include "NodePrefs.h"

#include <RTClib.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/BaseSerialInterface.h>
#include <helpers/IdentityStore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <target.h>

/* ---------------------------------- CONFIGURATION ------------------------------------- */

#ifndef LORA_FREQ
#define LORA_FREQ 915.0
#endif
#ifndef LORA_BW
#define LORA_BW 250
#endif
#ifndef LORA_SF
#define LORA_SF 10
#endif
#ifndef LORA_CR
#define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
#define LORA_TX_POWER 20
#endif
#ifndef MAX_LORA_TX_POWER
#define MAX_LORA_TX_POWER LORA_TX_POWER
#endif

#ifndef MAX_CONTACTS
#define MAX_CONTACTS 100
#endif

#ifndef OFFLINE_QUEUE_SIZE
#define OFFLINE_QUEUE_SIZE 16
#endif

#ifndef BLE_NAME_PREFIX
#define BLE_NAME_PREFIX "MeshCore-"
#endif

#include <helpers/BaseChatMesh.h>
#include <helpers/TransportKeyStore.h>

/* -------------------------------------------------------------------------------------- */

#define REQ_TYPE_GET_STATUS             0x01 // same as _GET_STATS
#define REQ_TYPE_KEEP_ALIVE             0x02
#define REQ_TYPE_GET_TELEMETRY_DATA     0x03

struct AdvertPath {
  uint8_t pubkey_prefix[7];
  uint8_t path_len;
  char    name[32];
  // Wall clock, for showing an absolute time. Zero when the RTC has never been set,
  // which is the normal state of a node with no companion app and no GPS fix - so it
  // cannot be used to decide slot occupancy or to measure age.
  uint32_t recv_timestamp;
  // Monotonic, from millis(). This is what recency, eviction and age are computed
  // from. Using the wall clock for them meant that with the RTC unset every entry
  // carried timestamp 0, the eviction scan found "oldest" on the first slot and
  // never moved, and every advert overwrote slot 0 - so the cache held exactly one
  // node however many were heard. Age came out as 0 - 0 = 0, so every node read as
  // just heard, forever.
  uint32_t recv_millis;
  // Explicit, rather than inferring occupancy from a timestamp or a name. Both of
  // those were tried and both were wrong for a value that is legitimately zero.
  bool valid;
  uint8_t path[MAX_PATH_SIZE];
};

class MyMesh : public BaseChatMesh, public DataStoreHost {
public:
  MyMesh(mesh::Radio &radio, mesh::RNG &rng, mesh::RTCClock &rtc, SimpleMeshTables &tables, DataStore& store, AbstractUITask* ui=NULL);

  void begin(bool has_display);
  void startInterface(BaseSerialInterface &serial);

  const char *getNodeName();
  NodePrefs *getNodePrefs();
  uint32_t getBLEPin();

  void loop();
  void handleCmdFrame(size_t len);
  bool advert();
  void enterCLIRescue();

  // advert() sends zero-hop, so only direct neighbours hear it. This floods the
  // advert through the mesh instead, which is what distant nodes need before
  // they can decrypt a direct message from us. Mirrors the companion protocol's
  // CMD_SEND_SELF_ADVERT with its flood flag set.
  bool advertFlood();

  int  getRecentlyHeard(AdvertPath dest[], int max_num);

  // How many ordinary contacts the table can hold. The anonymous slots are
  // pre-allocated by resetContacts() and never handed to a normal contact, so they are
  // not part of this - and getNumContacts() already excludes them, which is what makes
  // the two comparable.
  int getContactsCapacity() const { return MAX_CONTACTS; }

  // Has an advert been refused for want of a slot, and when.
  //
  // Being full is not a state the table itself reports - allocateContactSlot() simply
  // returns NULL - so it is recorded when it happens. Cleared by nothing: once a node
  // has been turned away, that stays true until the table has room again, which
  // nothing here can undo on its own.
  // Two different questions, and they were one value. "Is the table full right now"
  // decides whether the UI should say FULL; "has it ever refused a node" is history
  // worth keeping. The latch answered the second and was read as the first, so after
  // deleting a contact to make room the screen still said FULL.
  bool     contactsFullNow() const { return getNumContacts() >= getContactsCapacity(); }
  bool     contactsEverRefused() const { return _contacts_refused; }
  uint32_t contactsRefusedAt() const { return _contacts_refused_at; }

  // Resolve a path hash to a node, against every identity this node knows.
  //
  // Lives here rather than in the UI because this is the layer that has both sets.
  // The screen used to resolve against the recent advert cache alone, which is the
  // smaller of the two by an order of magnitude - up to 96 entries against 358 stored
  // contacts - so a hash that was unique among recently heard nodes reported UNIQUE
  // while a stored contact it collided with went unseen. Saying "via ALPHA" when the
  // honest answer is "ALPHA or BRAVO" is the failure this whole three-state result
  // exists to prevent, and the candidate set was quietly undermining it.
  //
  // Deduplicated, and that is load-bearing rather than tidy: a node is normally in
  // both sets, so counting matches without it would report almost every known node as
  // ambiguous with itself.
  //
  // Returns RIFT_RESOLVE_UNIQUE with out_name filled, or NONE / AMBIGUOUS - all
  // defined in ui-rift/RiftLogic.h, along with the accumulator that holds the rule.
  // RIFT-only, like logTx below: nothing else asks, and the constants live in a header
  // the other builds do not include.
#ifdef RIFT_VERSION
  int resolvePathHash(const uint8_t* hash, uint8_t hash_len, char* out_name, size_t name_len);
#endif
  // occupancy and pressure on the path cache, for the SYSTEM diagnostics
  int  getPathCacheUsed() const;
  int  getPathCacheSize() const;   // defined out of line: the table size is #defined below
  uint16_t getPathEvictions() const { return path_evictions; }

  // ------------------------------------------------- zero-hop repeater discovery
  //
  // An active question the rest of this firmware cannot ask: which repeaters can
  // hear *me*, right now, and how well in both directions.
  //
  // A DISCOVER_REQ control packet is sent zero-hop with a type filter, and every
  // repeater in direct range answers with its own SNR reading of our request plus
  // its identity. So a response carries both halves of the link - how they heard
  // us and how we heard them - and asymmetric links are common. Adverts only ever
  // tell us the inbound half.
  //
  // The wire format is not invented here: examples/simple_repeater/MyMesh.cpp
  // implements the responder and its own `discover.neighbors`, and this mirrors it
  // byte for byte. docs/payloads.md documents it.
  //
  // Responses trickle in. Repeaters deliberately answer after a widened random
  // delay, because many of them reply at once, so the window stays open for
  // RIFT_DISCOVER_WINDOW_MS rather than expecting an immediate answer.
  struct DiscoveredRepeater {
    uint8_t pubkey[32];
    int8_t  snr_they_heard_us;   // from the response payload, SNR*4
    int8_t  snr_we_heard_them;   // from the radio on the response itself, SNR*4
    uint32_t at_millis;
  };
  #define MAX_DISCOVERED_REPEATERS  16
  #define RIFT_DISCOVER_WINDOW_MS   30000

  // Starts a round. Returns false if the packet could not be allocated.
  bool startRepeaterDiscovery();
  bool isDiscovering() const;
  uint32_t discoveryElapsedMs() const;
  int  getDiscoveredCount() const { return discovered_count; }
  const DiscoveredRepeater* getDiscovered(int i) const {
    return (i >= 0 && i < discovered_count) ? &discovered[i] : NULL;
  }

  // Mesh receive activity. Nothing tracked this before, so the only "is the
  // network there" signal any UI could reach was the USB/BLE companion link -
  // a different question entirely.
  //
  // Fed from logRxRaw(), which Dispatcher calls for every raw radio reception
  // before parsing, so this counts packets addressed to other nodes and
  // packets that fail to decrypt. Not a contact or message count.
  //
  // hasHeardMesh() is false until the first reception; the caller needs it
  // because millis() 0 is a legitimate timestamp. Ages are computed as
  // millis() - getLastRxMillis() in unsigned arithmetic, which is wrap-safe.
  bool          hasHeardMesh() const { return _rx_ever; }
  unsigned long getLastRxMillis() const { return _last_rx_millis; }
  uint32_t      getRxCount() const { return _rx_count; }

  // Send a text message to a contact from local UI code. Mirrors the companion
  // app's CMD_SEND_TXT_MSG path, including registering the expected ACK, which
  // callers outside this class cannot do (expected_ack_table is private).
  // 'recipient' must be a live pointer from lookupContactByPubKey(), not a copy.
  // Returns MSG_SEND_FAILED / MSG_SEND_SENT_FLOOD / MSG_SEND_SENT_DIRECT.
  int  sendTextTo(ContactInfo* recipient, const char* text, uint32_t& expected_ack, uint32_t& est_timeout);

  // Add a group channel from local UI code, and persist it.
  //
  // BaseChatMesh::addChannel() deliberately isn't used: it writes at
  // num_channels, which only counts channels added through that method and
  // stays 0 for channels restored from storage - so it would silently overwrite
  // an existing channel. These find a genuinely free slot instead.
  //
  // Both return the slot index, or -1 on failure (bad key, or no free slot).
  int  addGroupChannelFromBase64(const char* name, const char* psk_base64);
  bool removeChannel(int idx);   // refuses slot 0, which is Public
  // hashtag channel: the key is derived from the name, so anyone who knows the
  // name can read the traffic. Encrypted on air, but not secret.
  int  addGroupChannelHashtag(const char* name);
  // generates a fresh random key; psk_base64_out receives it so the UI can show
  // the key for entering on other nodes
  int  addGroupChannelRandom(const char* name, char* psk_base64_out, int out_len);

protected:
  float getAirtimeBudgetFactor() const override;
  int getInterferenceThreshold() const override;
  bool getCADEnabled() const override;
  int calcRxDelay(float score, uint32_t air_time) const override;
  uint32_t getRetransmitDelay(const mesh::Packet *packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet *packet) override;
  uint8_t getExtraAckTransmitCount() const override;
  bool filterRecvFloodPacket(mesh::Packet* packet) override;
  bool allowPacketForward(const mesh::Packet* packet) override;

  void sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis);
  void sendFloodScoped(const ContactInfo& recipient, mesh::Packet* pkt, uint32_t delay_millis=0) override;
  void sendFloodScoped(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t delay_millis=0) override;

  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override;

  // The transmit side of the air log. Upstream declares both of these empty, so
  // overriding them costs no divergence - the log was receive-only because nothing
  // here had asked, not because the hooks were missing.
  void logTx(mesh::Packet* packet, int len) override;
  void logTxFail(mesh::Packet* packet, int len) override;

  // getTotalAirTime() is cumulative and outbound_start is private in Dispatcher, so
  // this packet's air time is the delta since the last transmit. logTx() is called
  // immediately after Dispatcher adds to the total, so the delta is exactly this send.
  unsigned long _last_air_total = 0;
  bool isAutoAddEnabled() const override;
  bool shouldAutoAddContactType(uint8_t type) const override;
  bool shouldOverwriteWhenFull() const override;
  uint8_t getAutoAddMaxHops() const override;
  void onContactsFull() override;

  // millis() when a contact was last turned away, 0 if never. Monotonic on purpose:
  // this has to work on a node whose clock was never set, which is the normal state
  // of a standalone RIFT.
  // An explicit flag rather than a zero timestamp as the sentinel: a refusal in the
  // first millisecond after boot would have read as "never happened".
  bool     _contacts_refused = false;
  uint32_t _contacts_refused_at = 0;
  void onContactOverwrite(const uint8_t* pub_key) override;
  bool onContactPathRecv(ContactInfo& from, uint8_t* in_path, uint8_t in_path_len, uint8_t* out_path, uint8_t out_path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) override;
  void onContactPathUpdated(const ContactInfo &contact) override;
  ContactInfo* processAck(const uint8_t *data) override;
  void queueMessage(const ContactInfo &from, uint8_t txt_type, mesh::Packet *pkt, uint32_t sender_timestamp,
                    const uint8_t *extra, int extra_len, const char *text);

  void onMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp,
                     const char *text) override;
  void onCommandDataRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp,
                         const char *text) override;
  void onSignedMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp,
                           const uint8_t *sender_prefix, const char *text) override;
  void onChannelMessageRecv(const mesh::GroupChannel &channel, mesh::Packet *pkt, uint32_t timestamp,
                            const char *text) override;
  void onChannelDataRecv(const mesh::GroupChannel &channel, mesh::Packet *pkt, uint16_t data_type,
                         const uint8_t *data, size_t data_len) override;

  uint8_t onContactRequest(const ContactInfo &contact, uint32_t sender_timestamp, const uint8_t *data,
                           uint8_t len, uint8_t *reply) override;
  void onContactResponse(const ContactInfo &contact, const uint8_t *data, uint8_t len) override;
  void onControlDataRecv(mesh::Packet *packet) override;
  void onRawDataRecv(mesh::Packet *packet) override;
  void onTraceRecv(mesh::Packet *packet, uint32_t tag, uint32_t auth_code, uint8_t flags,
                   const uint8_t *path_snrs, const uint8_t *path_hashes, uint8_t path_len) override;

  uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override;
  uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const override;
  void onSendTimeout() override;

  // DataStoreHost methods
  bool onContactLoaded(const ContactInfo& contact) override { return addContact(contact); }
  bool getContactForSave(uint32_t idx, ContactInfo& contact) override { return getContactByIdx(idx, contact); }
  bool onChannelLoaded(uint8_t channel_idx, const ChannelDetails& ch) override { return setChannel(channel_idx, ch); }
  bool getChannelForSave(uint8_t channel_idx, ChannelDetails& ch) override { return getChannel(channel_idx, ch); }

  int findFreeChannelSlot();
  int installChannel(int idx, const char* name, const uint8_t* psk, int psk_len);

  void clearPendingReqs() {
    pending_login = pending_status = pending_telemetry = pending_discovery = pending_req = 0;
  }

public:
  void savePrefs() {
    _prefs.node_lat = sensors.node_lat;
    _prefs.node_lon = sensors.node_lon;
    _store->savePrefs(_prefs);
  }

#if ENV_INCLUDE_GPS == 1
  void applyGpsPrefs() {
    sensors.setSettingValue("gps", _prefs.gps_enabled ? "1" : "0");
    if (_prefs.gps_interval > 0) {
      char interval_str[12];  // Max: 24 hours = 86400 seconds (5 digits + null)
      sprintf(interval_str, "%u", _prefs.gps_interval);
      sensors.setSettingValue("gps_interval", interval_str);
    }
  }
#endif

  // To check if there is pending work
  bool hasPendingWork() const;

private:
  void writeOKFrame();
  void writeErrFrame(uint8_t err_code);
  void writeDisabledFrame();
  void writeContactRespFrame(uint8_t code, const ContactInfo &contact);
  void updateContactFromFrame(ContactInfo &contact, uint32_t& last_mod, const uint8_t *frame, int len);
  void addToOfflineQueue(const uint8_t frame[], int len);
  int getFromOfflineQueue(uint8_t frame[]);
  int getBlobByKey(const uint8_t key[], int key_len, uint8_t dest_buf[]) override { 
    return _store->getBlobByKey(key, key_len, dest_buf);
  }
  bool putBlobByKey(const uint8_t key[], int key_len, const uint8_t src_buf[], int len) override {
    return _store->putBlobByKey(key, key_len, src_buf, len);
  }

  void checkCLIRescueCmd();
  void checkSerialInterface();
  bool isValidClientRepeatFreq(uint32_t f) const;

  // helpers, short-cuts
  void saveChannels() { _store->saveChannels(this); }
  void saveContacts();

  DataStore* _store;
  NodePrefs _prefs;
  uint32_t pending_login;
  uint32_t pending_status;
  uint32_t pending_telemetry, pending_discovery;   // pending _TELEMETRY_REQ
  uint32_t pending_req;   // pending _BINARY_REQ
  BaseSerialInterface *_serial;
  AbstractUITask* _ui;

  ContactsIterator _iter;
  uint32_t _iter_filter_since;
  uint32_t _most_recent_lastmod;
  uint32_t _active_ble_pin;
  bool _iter_started;
  bool _cli_rescue;
  bool send_unscoped;   // force un-scoped flood (instead of using send_scope)
  char cli_command[80];
  uint8_t app_target_ver;
  uint8_t *sign_data;
  uint32_t sign_data_len;
  unsigned long dirty_contacts_expiry;

  TransportKey send_scope;

  DiscoveredRepeater discovered[MAX_DISCOVERED_REPEATERS];
  int discovered_count;
  uint32_t discover_tag;        // 0 when no round is open
  unsigned long discover_until;

  bool _rx_ever;
  unsigned long _last_rx_millis;
  uint32_t _rx_count;

  uint8_t cmd_frame[MAX_FRAME_SIZE + 1];
  uint8_t out_frame[MAX_FRAME_SIZE + 1];
  CayenneLPP telemetry;

  struct Frame {
    uint8_t len;
    uint8_t buf[MAX_FRAME_SIZE];

    bool isChannelMsg() const;
  };
  int offline_queue_len;
  Frame offline_queue[OFFLINE_QUEUE_SIZE];

  struct AckTableEntry {
    unsigned long msg_sent;
    uint32_t ack;
    ContactInfo* contact;
  };
  #define EXPECTED_ACK_TABLE_SIZE 8
  AckTableEntry expected_ack_table[EXPECTED_ACK_TABLE_SIZE]; // circular table
  int next_ack_idx;

  #define ADVERT_PATH_TABLE_SIZE   96
  AdvertPath advert_paths[ADVERT_PATH_TABLE_SIZE]; // circular table
  // How many times a live entry has been dropped to make room. Exposed because it
  // is the number that says whether 16 slots is enough for the mesh in front of you,
  // and the alternative to guessing is measuring it in the field.
  uint16_t path_evictions = 0;
};

extern MyMesh the_mesh;
