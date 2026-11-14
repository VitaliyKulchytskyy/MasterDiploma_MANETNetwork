#ifndef EMERGENCYRADIOLINK_HANDLER_H
#define EMERGENCYRADIOLINK_HANDLER_H

#include <stdbool.h>
#include <stdint.h>
#include "crc8.h"
#include "erl.h"
#include "network.h"

#ifdef __cplusplus
extern "C" {
#endif

extern peer_t g_peers_table[NETWORK_MAX_PEERS];
extern peer_link_t g_peer_links[NETWORK_MAX_PEERS];
extern radio_stats_t g_radio_stats;
extern network_t g_device_network_info;

void erl_handler_network_init(const network_t* network);

/**
 * @brief Record that a peer was heard from, creating its table entry if needed.
 *
 * The entry's table index is the peer's host id within our own subnet
 * (address & ~g_device_network_info.subnet_mask), so it is found or created without a
 * scan. A subnet with more host addresses than NETWORK_MAX_PEERS cannot hold every peer.
 *
 * Always updates rssi (it is only ever meaningful for the packet just received); fields
 * other than that and the address are left untouched on an existing entry, the caller sets
 * whichever ones it has fresh data for (see erl_handler_set_peer_position() and
 * erl_handler_set_peer_info()).
 *
 * @param address ERL address of the peer.
 * @param rssi    RSSI of the packet just received from this peer, in dBm.
 * @return Pointer to the peer's entry in g_peers_table, or NULL if its host id does not
 *         fit into the table.
 */
peer_t* erl_handler_touch_peer(uint16_t address, int8_t rssi);

/**
 * @brief Record a peer's last known GPS position, from a received PACKET_TYPE_POSITION packet.
 *
 * Creates the peer's table entry if it does not exist yet (see erl_handler_touch_peer()).
 *
 * @param address   ERL address of the peer.
 * @param rssi      RSSI of the packet just received from this peer, in dBm.
 * @param longitude Degrees * 1e6 (microdegrees), west negative.
 * @param latitude  Degrees * 1e6 (microdegrees), south negative.
 * @return true if the entry was updated, false if its host id does not fit into the table.
 */
bool erl_handler_set_peer_position(uint16_t address, int8_t rssi, int32_t longitude, int32_t latitude);

/**
 * @brief Record a peer's uptime and config flags, from a received PACKET_TYPE_NETWORK packet.
 *
 * Creates the peer's table entry if it does not exist yet (see erl_handler_touch_peer()).
 *
 * @param address ERL address of the peer.
 * @param rssi    RSSI of the packet just received from this peer, in dBm.
 * @param uptime  Seconds since the peer's own boot.
 * @param config  Reserved status/config flags, 24 bits.
 * @return true if the entry was updated, false if its host id does not fit into the table.
 */
bool erl_handler_set_peer_info(uint16_t address, int8_t rssi, uint32_t uptime, uint32_t config);

/**
 * @brief Record a peer's compass reading (raw QMC5883 axes).
 *
 * Creates the peer's table entry if it does not exist yet (see erl_handler_touch_peer()).
 * There is no ERL broadcast of a peer's heading, so in practice this is only ever called
 * for the device's own address, from compass_handler_get().
 *
 * @param address ERL address of the peer.
 * @param rssi    RSSI of the packet just received from this peer, in dBm.
 * @param x,y,z   Raw magnetometer axes (signed ADC counts).
 * @return true if the entry was updated, false if its host id does not fit into the table.
 */
bool erl_handler_set_peer_compass(uint16_t address, int8_t rssi, int16_t x, int16_t y, int16_t z);

/**
 * @brief Count a packet from a peer and keep its link quality (MERL_CMD_LINK_STATS_GET).
 *
 * @param rssi, snr_q4, freq_error_hz  Of the packet just received (SNR in 0.25 dB steps).
 * @param now_ms                       Milliseconds since our boot.
 */
void erl_handler_link_update(uint16_t address, int8_t rssi, int8_t snr_q4, int32_t freq_error_hz, uint32_t now_ms);

/**
 * @brief Count a peer's heartbeat: its sequence number (erl_network_info_t.config bits 0-15) shows lost ones.
 */
void erl_handler_link_heartbeat(uint16_t address, uint16_t seq, uint32_t uptime);

void erl_handler_links_clear(void);

uint16_t erl_handler_build_packet(const erl_packet_t* packet, uint8_t* out, uint16_t out_size);

uint16_t erl_handler_print(const network_t* network, const erl_packet_t* packet, char* out, uint16_t out_size);

#ifdef __cplusplus
}
#endif

#endif //EMERGENCYRADIOLINK_HANDLER_H
