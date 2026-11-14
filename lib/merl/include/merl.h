#ifndef MERL_H
#define MERL_H

// MasterERL (MERL): the link between the main unit (master, Luckfox Lyra) and the
// emergency unit (slave, RP2040). Shared by both sides, so it is plain portable C:
// no platform headers, no C23 (the Lyra toolchain is gcc 10).
//
// [* sync]['a' version]['<' answer / '>' request][command - 2 bytes][len - 1 byte][PAYLOAD - len bytes][checksum - crc8]
//
// Multi-byte fields are big-endian on the wire, except the peer table entries of the
// MERL_CMD_PEERS_GET answer, which are packed little-endian (see merl_peer_t).
// The slave MUST answer every request. The requests the slave sends itself are
// MERL_CMD_USER_MESSAGE_SEND (a user message received over ERL) and
// MERL_CMD_USER_MESSAGE_PASSTHROUGH; the master answers them with MERL_CMD_RESPONSE_OK.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MERL_PROTOCOL_HEADER_SIZE               6
#define MERL_PROTOCOL_RX_TIMEOUT_MS             100     // a half-received frame is dropped after this
// the length byte and crc8 cover header + payload, so that has to fit in 255 bytes
#define MERL_PROTOCOL_PAYLOAD_MAX_SIZE          (255 - MERL_PROTOCOL_HEADER_SIZE)
#define MERL_PROTOCOL_FRAME_MAX_SIZE            (MERL_PROTOCOL_HEADER_SIZE + MERL_PROTOCOL_PAYLOAD_MAX_SIZE + 1)
#define MERL_PROTOCOL_CRC8_POLYNOMIAL           0x4A
#define MERL_PROTOCOL_SYNC_BYTE                 '*'
#define MERL_PROTOCOL_VERSION                   'a'
#define MERL_PROTOCOL_DIRECTION_REQUEST         '>'
#define MERL_PROTOCOL_DIRECTION_ANSWER          '<'

// Longest user message (text with the trailing \r\n) that fits into one ERL packet.
// Must match ERL_PROTOCOL_PAYLOAD_MAX_SIZE of the emergency unit.
#define MERL_USER_MESSAGE_MAX_SIZE              128

// On the wire the command is 2 bytes; the values are plain enum constants so that
// the header builds without C23 typed enums.
typedef uint16_t merl_command_t;

enum {
    // Responses
    MERL_CMD_RESPONSE_OK = 0x0000,              // payload: 2 bytes of the command from the request
    MERL_CMD_RESPONSE_OK_BODY = 0x0001,         // payload: N bytes (based on the request command)
    MERL_CMD_RESPONSE_ERROR = 0x0100,           // payload: 2 bytes of the command from the request
    MERL_CMD_RESPONSE_UNKNOWN = 0x0101,         // payload: 2 bytes of the command from the request
    MERL_CMD_RESPONSE_UNIMPLEMENTED = 0x0102,   // payload: 2 bytes of the command from the request
    MERL_CMD_RESPONSE_OVERLOAD = 0x0110,        // payload: 2 bytes of the command from the request

    // Network commands
    MERL_CMD_NETWORK_GET = 0x1000,              // payload: 0 bytes; response: merl_network_params_t
    MERL_CMD_NETWORK_SET = 0x1001,              // payload: merl_network_params_t; response: 0x0000

    // Peer commands
    MERL_CMD_PEERS_GET = 0x2000,                // payload: 0 bytes; response: N * merl_peer_t
    MERL_CMD_PEERS_CLEAR = 0x2001,              // payload: 0 bytes; response: 0x0000
    MERL_CMD_RADIO_STATS_GET = 0x2002,          // payload: 0 bytes; response: merl_radio_stats_t
                                                // (MERL_FEATURE_LINK_STATS)
    MERL_CMD_LINK_STATS_GET = 0x2003,           // payload: 0 bytes; response: N * merl_link_stats_t, the
                                                // peers heard most recently first (MERL_FEATURE_LINK_STATS)

    // Position commands
    MERL_CMD_POSITION_GET = 0x3000,             // payload: 0 bytes; response: merl_position_t
    MERL_CMD_POSITION_SET = 0x3001,             // payload: merl_position_set_t; response: 0x0000

    // Compass commands (implemented compass module: QMC5883)
    MERL_CMD_COMPASS_GET = 0x3002,              // payload: 0 bytes; response: merl_compass_t
    MERL_CMD_COMPASS_SET = 0x3003,              // payload: merl_compass_t; response: 0x0000
    MERL_CMD_GPS_STATUS = 0x3004,               // payload: 0 bytes; response: merl_gps_status_t
                                                // (UNIMPLEMENTED without MERL_FEATURE_GPS)

    // Device commands
    MERL_CMD_DEVICE_VERSION = 0x4000,           // payload: 0 bytes; response: merl_device_version_t
    MERL_CMD_DEVICE_FEATURES = 0x4001,          // payload: 0 bytes; response: N * merl_feature_t (compiled-in features)

    // User message commands
    MERL_CMD_USER_MESSAGE_SEND = 0x5000,        // both ways, payload: see merl_user_message_encode(); response: 0x0000
    MERL_CMD_USER_MESSAGE_PASSTHROUGH = 0x5001, // device -> main: raw passthrough; response: 0x0000 from the main unit
};

// MERL_CMD_USER_MESSAGE_SEND payload: [address - 4 bytes BE][text ending with \r\n]
//   main -> device: address = ERL receiver (unicast or the subnet broadcast address)
//   device -> main: a user message received over ERL,
//                   address = receiver << 16 | sender (receiver shows whether it was a broadcast)
#define MERL_USER_MESSAGE_ADDRESS_SIZE          4
#define MERL_USER_MESSAGE_TEXT_MAX_SIZE         (MERL_USER_MESSAGE_MAX_SIZE - 2)   // without the \r\n

enum {
    MERL_OFFSET_SYNC = 0,
    MERL_OFFSET_VERSION = 1,
    MERL_OFFSET_DIRECTION = 2,
    MERL_OFFSET_COMMAND = 3,
    MERL_OFFSET_LENGTH = 5,
    MERL_OFFSET_PAYLOAD = MERL_PROTOCOL_HEADER_SIZE,
};

// A decoded frame. payload points into the buffer it was decoded from.
typedef struct {
    uint8_t direction;          // MERL_PROTOCOL_DIRECTION_REQUEST or _ANSWER
    merl_command_t command;
    uint8_t length;
    const uint8_t* payload;
} merl_frame_t;

// Byte-wise frame parser state
typedef struct {
    uint8_t buf[MERL_PROTOCOL_FRAME_MAX_SIZE];
    size_t pos;
} merl_parser_t;

// Payload of MERL_CMD_NETWORK_GET answer / MERL_CMD_NETWORK_SET request
typedef struct {
    uint16_t address;
    uint16_t subnet_mask;
} merl_network_params_t;
#define MERL_NETWORK_PARAMS_SIZE                4

// Payload of MERL_CMD_POSITION_GET answer.
// time: milliseconds since UTC midnight of the fix, 0 if the position was set manually.
// longitude/latitude: degrees * 1e6 (microdegrees); west/south are negative.
typedef struct {
    uint32_t time;
    int32_t longitude;
    int32_t latitude;
} merl_position_t;
#define MERL_POSITION_SIZE                      12

// Payload of MERL_CMD_POSITION_SET request, same encoding as merl_position_t
typedef struct {
    int32_t longitude;
    int32_t latitude;
} merl_position_set_t;
#define MERL_POSITION_SET_SIZE                  8

// Payload of MERL_CMD_COMPASS_GET answer / MERL_CMD_COMPASS_SET request: raw QMC5883
// magnetometer axes (signed ADC counts, not calibrated or scaled to gauss/degrees).
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} merl_compass_t;
#define MERL_COMPASS_SIZE                       6

// Payload of MERL_CMD_GPS_STATUS answer: the GPS receiver's view of the sky, fix or not
// (from the NMEA GGA / GSA / GSV sentences). All zero until the receiver has said anything.
typedef enum {
    MERL_GPS_FIX_NONE = 0,
    MERL_GPS_FIX_GPS = 1,           // satellites only
    MERL_GPS_FIX_DGPS = 2,          // differential
} merl_gps_fix_t;
typedef struct {
    uint8_t fix;                    // merl_gps_fix_t (GGA fix quality)
    uint8_t satellites_used;        // in the fix (GGA)
    uint8_t satellites_in_view;     // (GSV)
    uint16_t hdop_x100;             // horizontal dilution of precision * 100, 0 = unknown
} merl_gps_status_t;
#define MERL_GPS_STATUS_SIZE                    5   // [fix][used][in view][hdop x100 - 2 bytes BE]

// Payload of MERL_CMD_DEVICE_VERSION answer.
// build_date/build_time are the compiler's __DATE__ ("Mmm dd yyyy") and __TIME__ ("hh:mm:ss")
// from the build that produced this firmware, copied as-is (fixed width, not NUL-terminated
// on the wire: both formats are guaranteed fixed-length by the C standard). build_number is
// a plain counter, incremented once per build (see the emergency unit's build_info.h).
#define MERL_DEVICE_BUILD_DATE_SIZE              11
#define MERL_DEVICE_BUILD_TIME_SIZE              8
typedef struct {
    uint8_t major;
    uint8_t minor;
    uint32_t build_number;
    char build_date[MERL_DEVICE_BUILD_DATE_SIZE];
    char build_time[MERL_DEVICE_BUILD_TIME_SIZE];
} merl_device_version_t;
#define MERL_DEVICE_VERSION_SIZE                (1 + 1 + 4 + MERL_DEVICE_BUILD_DATE_SIZE + MERL_DEVICE_BUILD_TIME_SIZE)

// Feature ids reported by MERL_CMD_DEVICE_FEATURES: whether a feature is present is a
// compile-time decision (CONFIG_DEVICE_FEATURES at build time, see the emergency unit's
// CMakePresets.json), not something toggled at runtime. Appended only, never renumbered
// or reused, so both sides keep agreeing on old ids as new features are added.
typedef uint8_t merl_feature_t;
enum {
    MERL_FEATURE_GPS = 0,
    MERL_FEATURE_COMPASS = 1,
    MERL_FEATURE_LINK_STATS = 2,    // MERL_CMD_RADIO_STATS_GET, MERL_CMD_LINK_STATS_GET
};

// One entry of the MERL_CMD_PEERS_GET answer: the emergency unit's packed peer_t,
// little-endian: address u32, rssi i8, uptime u32, longitude i32, latitude i32, config u24,
// compass_x i16, compass_y i16, compass_z i16
typedef struct {
    uint32_t address;
    int8_t rssi;        // dBm of the last packet received from the peer, 0 for the device itself
    uint32_t uptime;    // seconds since the peer's boot
    int32_t longitude;  // degrees * 1e6, west negative
    int32_t latitude;   // degrees * 1e6, south negative
    uint32_t config;    // 24 bits, reserved for peer status/config flags
    int16_t compass_x;  // raw QMC5883 magnetometer axes (signed ADC counts); all 0 if never
    int16_t compass_y;  // reported: a peer's ride on its PACKET_TYPE_POSITION broadcasts (so only
    int16_t compass_z;  // with a known position, every 15 s), our own too (MERL_FEATURE_COMPASS)
} merl_peer_t;
#define MERL_PEER_SIZE                          26

// Payload of MERL_CMD_RADIO_STATS_GET answer: the emergency unit's radio since its boot, big-endian.
// Counters only grow (u32, wrap around after years), so a reader takes differences between two reads.
typedef struct {
    uint32_t uptime_ms;         // since the unit's boot: the time base of the counters
    uint32_t rx_ok;             // LoRa packets with a valid ERL frame (any receiver)
    uint32_t rx_crc_error;      // LoRa packets the transceiver received with a bad payload CRC
    uint32_t rx_bad_frame;      // received, but not an ERL frame: wrong length or CRC8
    uint32_t rx_not_for_us;     // valid ERL frames for another unit or another subnet
    uint32_t rx_queue_drop;     // received for us, dropped: the queue to the protocol side was full
    uint32_t tx_ok;             // packets sent
    uint32_t tx_error;          // transmit calls that failed
    uint32_t tx_airtime_ms;     // time on air of everything sent
    uint32_t lbt_busy;          // a transmission waited: the channel was busy (listen before talk)
    uint32_t tx_queue_drop;     // packets not sent: the transmit queue was full
    int8_t noise_dbm;           // channel RSSI of the last idle sample (no packet on the air)
    int8_t noise_min_dbm;       // lowest idle sample since boot, 0 = no sample yet
    int8_t noise_avg_dbm;       // moving average of the idle samples, 0 = no sample yet
} merl_radio_stats_t;
#define MERL_RADIO_STATS_SIZE                   (11 * 4 + 3)

// One entry of the MERL_CMD_LINK_STATS_GET answer: how a peer's packets arrive here, big-endian.
// The sums let a reader get exact means over any interval: difference of the sums / difference of rx_packets.
// Heartbeats (PACKET_TYPE_NETWORK, every 5 s) carry a sequence number (erl_network_info_t.config
// bits 0-15): gaps in it count heartbeats that never arrived, in this direction only.
typedef struct {
    uint16_t address;
    uint32_t rx_packets;        // ERL packets from the peer (all types)
    uint32_t hb_received;       // its heartbeats that arrived
    uint32_t hb_missed;         // its heartbeats lost (gaps in the sequence; reset when it reboots)
    int8_t rssi_dbm;            // last packet: RSSI
    int8_t snr_q4;              // last packet: SNR in 0.25 dB steps
    int32_t freq_error_hz;      // last packet: carrier frequency error against our crystal
    uint32_t age_ms;            // since the last packet
    int32_t rssi_sum;           // sum of the RSSI of all rx_packets, dBm
    int32_t snr_q4_sum;         // sum of their SNR, 0.25 dB steps
} merl_link_stats_t;
#define MERL_LINK_STATS_SIZE                    (2 + 4 + 4 + 4 + 1 + 1 + 4 + 4 + 4 + 4)

/**
 * @brief CRC8 of a frame: init 0, reflected, shifting right, MERL_PROTOCOL_CRC8_POLYNOMIAL.
 */
uint8_t merl_crc8(const uint8_t* data, size_t length);

/**
 * @brief Build a frame on the wire.
 *
 * @param out       Destination, at least MERL_PROTOCOL_HEADER_SIZE + length + 1 bytes.
 * @param out_size  Size of @p out.
 * @param direction MERL_PROTOCOL_DIRECTION_REQUEST or MERL_PROTOCOL_DIRECTION_ANSWER.
 * @param command   Request command or response code.
 * @param payload   Payload bytes, may be NULL if @p length is 0.
 * @param length    Payload size, at most MERL_PROTOCOL_PAYLOAD_MAX_SIZE.
 * @return Number of bytes written, or 0 if the payload is too long or @p out too small.
 */
size_t merl_frame_encode(uint8_t* out, size_t out_size, uint8_t direction, merl_command_t command,
                         const uint8_t* payload, uint8_t length);

/**
 * @brief Build an answer without a body: the payload is the command of the request.
 */
size_t merl_status_encode(uint8_t* out, size_t out_size, merl_command_t status, merl_command_t request_command);

void merl_parser_reset(merl_parser_t* parser);

/**
 * @brief true while a frame is partially received, for dropping it after MERL_PROTOCOL_RX_TIMEOUT_MS.
 */
bool merl_parser_busy(const merl_parser_t* parser);

/**
 * @brief Feed one received byte.
 *
 * Garbage between frames and frames with a bad CRC8 are skipped.
 *
 * @return true when the byte completes a valid frame, stored in @p out. out->payload
 *         points into the parser and stays valid until the next call.
 */
bool merl_parser_feed(merl_parser_t* parser, uint8_t byte, merl_frame_t* out);

void merl_network_params_encode(uint8_t out[MERL_NETWORK_PARAMS_SIZE], const merl_network_params_t* params);
void merl_network_params_decode(const uint8_t in[MERL_NETWORK_PARAMS_SIZE], merl_network_params_t* params);

void merl_position_encode(uint8_t out[MERL_POSITION_SIZE], const merl_position_t* position);
void merl_position_decode(const uint8_t in[MERL_POSITION_SIZE], merl_position_t* position);

void merl_position_set_encode(uint8_t out[MERL_POSITION_SET_SIZE], const merl_position_set_t* position);
void merl_position_set_decode(const uint8_t in[MERL_POSITION_SET_SIZE], merl_position_set_t* position);

void merl_compass_encode(uint8_t out[MERL_COMPASS_SIZE], const merl_compass_t* compass);
void merl_compass_decode(const uint8_t in[MERL_COMPASS_SIZE], merl_compass_t* compass);

void merl_gps_status_encode(uint8_t out[MERL_GPS_STATUS_SIZE], const merl_gps_status_t* status);
void merl_gps_status_decode(const uint8_t in[MERL_GPS_STATUS_SIZE], merl_gps_status_t* status);

void merl_radio_stats_encode(uint8_t out[MERL_RADIO_STATS_SIZE], const merl_radio_stats_t* stats);
void merl_radio_stats_decode(const uint8_t in[MERL_RADIO_STATS_SIZE], merl_radio_stats_t* stats);

void merl_link_stats_encode(uint8_t out[MERL_LINK_STATS_SIZE], const merl_link_stats_t* stats);
void merl_link_stats_decode(const uint8_t in[MERL_LINK_STATS_SIZE], merl_link_stats_t* stats);

void merl_peer_encode(uint8_t out[MERL_PEER_SIZE], const merl_peer_t* peer);
void merl_peer_decode(const uint8_t in[MERL_PEER_SIZE], merl_peer_t* peer);

void merl_device_version_encode(uint8_t out[MERL_DEVICE_VERSION_SIZE], const merl_device_version_t* version);
void merl_device_version_decode(const uint8_t in[MERL_DEVICE_VERSION_SIZE], merl_device_version_t* version);

/**
 * @brief Build a MERL_CMD_USER_MESSAGE_SEND payload: address, text, then \r\n.
 *
 * @param text        Message text without the trailing \r\n.
 * @param text_length At most MERL_USER_MESSAGE_TEXT_MAX_SIZE.
 * @return Payload size, or 0 if the text is too long or @p out too small.
 */
uint8_t merl_user_message_encode(uint8_t* out, size_t out_size, uint32_t address, const uint8_t* text,
                                 size_t text_length);

/**
 * @brief Split a MERL_CMD_USER_MESSAGE_SEND payload.
 *
 * @param text        Set to the message text inside @p payload, without the \r\n.
 * @param text_length Its length.
 * @return false if the payload is malformed: too short or long, or not ending with \r\n.
 */
bool merl_user_message_decode(const uint8_t* payload, uint8_t length, uint32_t* address, const uint8_t** text,
                              uint8_t* text_length);

/**
 * @brief Name of a command or response code ("PEERS_GET", "OK", ...), NULL if unknown.
 */
const char* merl_command_name(merl_command_t command);

/**
 * @brief Name of a MERL_CMD_DEVICE_FEATURES feature id ("GPS", ...), NULL for an id this
 *        library does not know yet (a newer firmware).
 */
const char* merl_feature_name(merl_feature_t feature);

static inline uint16_t merl_get_be16(const uint8_t* p) {
    return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t merl_get_be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void merl_put_be16(uint8_t* p, const uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static inline void merl_put_be32(uint8_t* p, const uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

#ifdef __cplusplus
}
#endif

#endif // MERL_H
