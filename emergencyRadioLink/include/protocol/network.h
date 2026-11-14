#ifndef EMERGENCYRADIOLINK_NETWORK_H
#define EMERGENCYRADIOLINK_NETWORK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// Matches the number of host addresses in the default subnet mask (0xFFE0, 5 host bits):
// a peer's table entry is indexed directly by its host id (address & ~subnet_mask), see
// erl_handler_touch_peer(). A subnet with more host bits than this cannot hold every peer.
#define NETWORK_MAX_PEERS   32

typedef struct {
    uint32_t address;
    uint32_t subnet_mask;
} network_t;

typedef struct __attribute__((__packed__)) {
    uint32_t address;
    int8_t rssi;        // dBm of the last packet received from this peer, for a rough distance estimate
    uint32_t uptime;    // seconds since the peer's own boot, from its last PACKET_TYPE_NETWORK broadcast
    int32_t longitude;  // degrees * 1e6 (microdegrees), west negative
    int32_t latitude;   // degrees * 1e6 (microdegrees), south negative
    uint32_t config: 24; // reserved for future peer status/config flags
    int16_t compass_x;  // raw QMC5883 magnetometer axes (signed ADC counts), 0 for a peer
    int16_t compass_y;  // that never reported one; 0 is also a peer's own rssi, so it is not
    int16_t compass_z;  // by itself proof the reading is unset (see MERL_FEATURE_COMPASS)
} peer_t;

// How a peer's packets arrive here (MERL_CMD_LINK_STATS_GET), indexed like g_peers_table by host id.
// Updated on core 1 for every ERL packet from the peer.
typedef struct {
    uint16_t address;
    uint32_t rx_packets;        // packets from the peer, all types
    uint32_t hb_received;       // its heartbeats that arrived
    uint32_t hb_missed;         // gaps in their sequence numbers
    uint16_t hb_last_seq;
    uint32_t hb_last_uptime;    // the peer's uptime in its last heartbeat: going down = it rebooted
    bool hb_seen;
    int8_t rssi;                // last packet, dBm
    int8_t snr_q4;              // last packet, 0.25 dB steps
    int32_t freq_error_hz;      // last packet
    uint32_t last_rx_ms;        // ms since our boot
    int32_t rssi_sum;           // over rx_packets: exact means between two reads
    int32_t snr_q4_sum;
} peer_link_t;

// The radio since boot (MERL_CMD_RADIO_STATS_GET): counted on core 0 (radio), read on core 1 (MERL).
// 32-bit reads and writes are atomic on the RP2040, each counter has a single writer.
typedef struct {
    volatile uint32_t rx_ok;
    volatile uint32_t rx_crc_error;
    volatile uint32_t rx_bad_frame;
    volatile uint32_t rx_not_for_us;
    volatile uint32_t rx_queue_drop;
    volatile uint32_t tx_ok;
    volatile uint32_t tx_error;
    volatile uint32_t tx_airtime_ms;
    volatile uint32_t lbt_busy;
    volatile uint32_t tx_queue_drop;    // counted on core 1 (the queues' producer)
    volatile int8_t noise_dbm;
    volatile int8_t noise_min_dbm;
    volatile int8_t noise_avg_dbm;
} radio_stats_t;

#ifdef __cplusplus
}
#endif

#endif //EMERGENCYRADIOLINK_NETWORK_H
