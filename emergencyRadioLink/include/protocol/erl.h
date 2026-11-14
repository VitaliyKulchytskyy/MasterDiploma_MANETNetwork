#ifndef ERL_H
#define ERL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ERL_PROTOCOL_PAYLOAD_MAX_SIZE           128
#define ERL_PROTOCOL_CRC8_POLYNOMIAL            0x4A
#define ERL_PROTOCOL_SYNC_BYTE                  '*'
#define ERL_PROTOCOL_VERSION                    'a'
#define ERL_PROTOCOL_RECEIVER_BROADCAST         0xFF


typedef enum: uint8_t {
    PACKET_STATUS_OKAY = 0,
    PACKET_STATUS_WRONG_CHECKSUM = 1,
    PACKET_STATUS_UNKNOWN_TYPE = 2,
    PACKET_STATUS_DIFF_VERSION = 3,
    PACKET_STATUS_WRONG_BUFFER_SIZE = 4,
    PACKET_STATUS_WRONG_PAYLOAD_SIZE = 5,
} packet_status_t;

typedef enum: uint8_t {
    PACKET_TYPE_RESPONSE = 0,
    PACKET_TYPE_COMMAND = 1,
    PACKET_TYPE_NETWORK = 2,
    PACKET_TYPE_POSITION = 3,
    PACKET_TYPE_USER_MESSAGE = 5,
} packet_type_t;

// [* - sync] ['a' version - 1 byte] [sender addr - 2 bytes] [receiver addr - 2 bytes] [type - 1 byte] [len - 1 byte] [PAYLOAD - len bytes] [checksum - crc8]
typedef struct __attribute__((__packed__)) {
    uint8_t sync;
    uint8_t version;
    uint16_t address;
    uint16_t receiver;
    packet_type_t type;
    uint8_t length;
    uint8_t payload[];
    // the last byte - checksum crc8
} erl_packet_t;

// Payload of a PACKET_TYPE_NETWORK packet (the periodic heartbeat): tells peers this device
// is alive and how long it has been running. rssi is not included here, since it is only
// meaningful to whoever receives the packet (see erl_handler_touch_peer()); position is
// carried separately, by PACKET_TYPE_POSITION.
typedef struct __attribute__((__packed__)) {
    uint32_t uptime;     // seconds since this device's own boot
    uint32_t config: 24; // bits 0-15: heartbeat sequence number (+1 per heartbeat, wraps), so receivers
                         // count lost heartbeats (MERL_CMD_LINK_STATS_GET); bits 16-23 reserved, 0.
                         // Firmware before it sent 0: every gap looks like none
} erl_network_info_t;

// Payload of a PACKET_TYPE_POSITION packet, native byte order (ERL does not byte-swap).
// time: milliseconds since UTC midnight of the fix, 0 if the position was set manually over MERL.
// longitude/latitude: degrees * 1e6 (microdegrees); west/south are negative.
typedef struct __attribute__((__packed__)) {
    uint32_t time;
    int32_t longitude;
    int32_t latitude;
} erl_position_t;

// Appended to a PACKET_TYPE_POSITION packet, right after erl_position_t, whenever a compass
// reading is known - not a packet type of its own, it always rides with position (native byte
// order, ERL does not byte-swap). Raw QMC5883 magnetometer axes (signed ADC counts, not
// calibrated or scaled to gauss/degrees).
typedef struct __attribute__((__packed__)) {
    int16_t x;
    int16_t y;
    int16_t z;
} erl_compass_t;

// In this layer the device already know its address and network params (subnetwork mask etc.)

// Send heartbeat to all in the subnetwork (at least half in a minute)
// subnet mask: 0xFFE0
// [*][a][0xC701][0xC71F][0x00][0x00][checksum]

// Send emergency to all in the subnetwork
// subnet mask: 0xFFE0
// [*][a][0xC701][0xC71F][0x00][0x01][0xFF][checksum]

// Send coordinates to all in the subnetwork
// subnet mask: 0xFFE0
// [*][a][0xC70A][0xC71F][0x01][0x0B][time, longitude, latitude][checksum]

// Send custom msg to all in the subnetwork
// subnet mask: 0xFFE0
// [*][a][0xC701][0xC71F][0x05][0x05]['H', 'i', '!', '\r', '\n'][checksum]

#ifdef __cplusplus
}
#endif

#endif
