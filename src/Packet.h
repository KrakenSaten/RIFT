#pragma once

#include <MeshCore.h>

namespace mesh {

// Packet::header values
#define PH_ROUTE_MASK     0x03   // 2-bits
#define PH_TYPE_SHIFT        2
#define PH_TYPE_MASK      0x0F   // 4-bits
#define PH_VER_SHIFT         6
#define PH_VER_MASK       0x03   // 2-bits

#define ROUTE_TYPE_TRANSPORT_FLOOD   0x00    // flood mode + transport codes
#define ROUTE_TYPE_FLOOD             0x01    // flood mode, needs 'path' to be built up (max 64 bytes)
#define ROUTE_TYPE_DIRECT            0x02    // direct route, 'path' is supplied
#define ROUTE_TYPE_TRANSPORT_DIRECT  0x03    // direct route + transport codes

#define PAYLOAD_TYPE_REQ         0x00    // request (prefixed with dest/src hashes, MAC) (enc data: timestamp, blob)
#define PAYLOAD_TYPE_RESPONSE    0x01    // response to REQ or ANON_REQ (prefixed with dest/src hashes, MAC) (enc data: timestamp, blob)
#define PAYLOAD_TYPE_TXT_MSG     0x02    // a plain text message (prefixed with dest/src hashes, MAC) (enc data: timestamp, text)
#define PAYLOAD_TYPE_ACK         0x03    // a simple ack
#define PAYLOAD_TYPE_ADVERT      0x04    // a node advertising its Identity
#define PAYLOAD_TYPE_GRP_TXT     0x05    // an (unverified) group text message (prefixed with channel hash, MAC) (enc data: timestamp, "name: msg")
#define PAYLOAD_TYPE_GRP_DATA    0x06    // an (unverified) group datagram (prefixed with channel hash, MAC) (enc data: data_type(uint16), data_len, blob)
#define PAYLOAD_TYPE_ANON_REQ    0x07    // generic request (prefixed with dest_hash, ephemeral pub_key, MAC) (enc data: ...)
#define PAYLOAD_TYPE_PATH        0x08    // returned path (prefixed with dest/src hashes, MAC) (enc data: path, extra)
#define PAYLOAD_TYPE_TRACE       0x09    // trace a path, collecting SNR for each hop
#define PAYLOAD_TYPE_MULTIPART   0x0A    // packet is one of a set of packets
#define PAYLOAD_TYPE_CONTROL     0x0B    // a control/discovery packet
//...
#define PAYLOAD_TYPE_RAW_CUSTOM   0x0F    // custom packet as raw bytes, for applications with custom encryption, payloads, etc

#define PAYLOAD_VER_1       0x00   // 1-byte src/dest hashes, 2-byte MAC
#define PAYLOAD_VER_2       0x01   // FUTURE (eg. 2-byte hashes, 4-byte MAC ??)
#define PAYLOAD_VER_3       0x02   // FUTURE
#define PAYLOAD_VER_4       0x03   // FUTURE

/**
 * \brief  The fundamental transmission unit.
*/
class Packet {
public:
  Packet();

  uint8_t header;
  uint16_t payload_len, path_len;
  uint16_t transport_codes[2];
  uint8_t path[MAX_PATH_SIZE];
  uint8_t payload[MAX_PACKET_PAYLOAD];
  int8_t _snr;

  /**
   * \brief calculate the hash of payload + type
   * \param  dest_hash   destination to store the hash (must be MAX_HASH_SIZE bytes)
   */
  void calculatePacketHash(uint8_t* dest_hash) const;

  /**
   * \returns  one of ROUTE_ values
   */
  uint8_t getRouteType() const { return header & PH_ROUTE_MASK; }

  bool isRouteFlood() const { return getRouteType() == ROUTE_TYPE_FLOOD || getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD; }
  bool isRouteDirect() const { return getRouteType() == ROUTE_TYPE_DIRECT || getRouteType() == ROUTE_TYPE_TRANSPORT_DIRECT; }

  bool hasTransportCodes() const { return getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD || getRouteType() == ROUTE_TYPE_TRANSPORT_DIRECT; }

  /**
   * \returns  one of PAYLOAD_TYPE_ values
   */
  uint8_t getPayloadType() const { return (header >> PH_TYPE_SHIFT) & PH_TYPE_MASK; }

  /**
   * \returns  one of PAYLOAD_VER_ values
   */
  uint8_t getPayloadVer() const { return (header >> PH_VER_SHIFT) & PH_VER_MASK; }

  // path_len packs the hash size into bits 6-7 and the hop count into bits 0-5.
  // Exposed as statics too, because callers that hold a stored path_len rather
  // than a Packet - the advert cache, and the UI drawing hop counts from it -
  // were otherwise repeating the bit twiddling, and one of them got it wrong.
  // uint16_t, not uint8_t: the member below is uint16_t, and narrowing it here
  // would change what these return for any value above 255 rather than just
  // relocating the arithmetic.
  static uint8_t pathHashSize(uint16_t path_len) { return (path_len >> 6) + 1; }
  static uint8_t pathHashCount(uint16_t path_len) { return path_len & 63; }

  uint8_t getPathHashSize() const { return pathHashSize(path_len); }
  uint8_t getPathHashCount() const { return pathHashCount(path_len); }
  uint8_t getPathByteLen() const { return getPathHashCount() * getPathHashSize(); }
  void setPathHashCount(uint8_t n) { path_len &= ~63; path_len |= n; }
  void setPathHashSizeAndCount(uint8_t sz, uint8_t n) { path_len = ((sz - 1) << 6) | (n & 63); }

  static uint8_t copyPath(uint8_t* dest, const uint8_t* src, uint8_t path_len);  // returns path_len
  static size_t writePath(uint8_t* dest, const uint8_t* src, uint8_t path_len);  // returns byte length written
  static bool isValidPathLen(uint8_t path_len);

  void markDoNotRetransmit() { header = 0xFF; }
  bool isMarkedDoNotRetransmit() const { return header == 0xFF; }

  float getSNR() const { return ((float)_snr) / 4.0f; }

  /**
   * \returns  the encoded/wire format length of this packet
   */
  int getRawLength() const;

  /**
   * \brief  save entire packet as a blob
   * \param dest  (OUT) destination buffer (assumed to be MAX_MTU_SIZE)
   * \returns  the packet length
   */
  uint8_t writeTo(uint8_t dest[]) const;

  /**
   * \brief  restore this packet from a blob (as created using writeTo())
   * \param  src  (IN) buffer containing blob
   * \param  len  the packet length (as returned by writeTo())
   */
  bool readFrom(const uint8_t src[], uint8_t len);
};

}
