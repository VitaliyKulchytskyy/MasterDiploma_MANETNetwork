#include "protocol/erl_handler.h"

#include <stdio.h>
#include <string.h>

peer_t g_peers_table[NETWORK_MAX_PEERS];
peer_link_t g_peer_links[NETWORK_MAX_PEERS];
radio_stats_t g_radio_stats;
network_t g_device_network_info;

// a jump this large is not loss: the peer rebooted or its counter was reset
#define HB_SEQ_MAX_GAP  1000

static peer_link_t* link_of(const uint16_t address) {
    const uint32_t host_id = (uint32_t)address & ~g_device_network_info.subnet_mask;
    if (host_id >= NETWORK_MAX_PEERS)
        return NULL;
    peer_link_t* link = &g_peer_links[host_id];
    if (link->address != address) {
        memset(link, 0, sizeof(*link));
        link->address = address;
    }
    return link;
}

void erl_handler_link_update(const uint16_t address, const int8_t rssi, const int8_t snr_q4,
                             const int32_t freq_error_hz, const uint32_t now_ms) {
    peer_link_t* link = link_of(address);
    if (link == NULL)
        return;
    ++link->rx_packets;
    link->rssi = rssi;
    link->snr_q4 = snr_q4;
    link->freq_error_hz = freq_error_hz;
    link->last_rx_ms = now_ms;
    link->rssi_sum += rssi;
    link->snr_q4_sum += snr_q4;
}

void erl_handler_link_heartbeat(const uint16_t address, const uint16_t seq, const uint32_t uptime) {
    peer_link_t* link = link_of(address);
    if (link == NULL)
        return;
    const uint16_t gap = (uint16_t)(seq - link->hb_last_seq);
    if (link->hb_seen && uptime >= link->hb_last_uptime && gap > 0 && gap < HB_SEQ_MAX_GAP)
        link->hb_missed += gap - 1u;
    ++link->hb_received;
    link->hb_seen = true;
    link->hb_last_seq = seq;
    link->hb_last_uptime = uptime;
}

void erl_handler_links_clear(void) {
    memset(g_peer_links, 0, sizeof(g_peer_links));
}

void erl_handler_network_init(const network_t* network) {
    g_device_network_info.address = network->address;
    g_device_network_info.subnet_mask = network->subnet_mask;
}

peer_t* erl_handler_touch_peer(const uint16_t address, const int8_t rssi) {
    const uint32_t host_id = (uint32_t)address & ~g_device_network_info.subnet_mask;
    if (host_id >= NETWORK_MAX_PEERS)
        return NULL;

    peer_t* peer = &g_peers_table[host_id];
    if (peer->address != address) {
        memset(peer, 0, sizeof(*peer));
        peer->address = address;
    }

    peer->rssi = rssi;
    return peer;
}

bool erl_handler_set_peer_position(const uint16_t address, const int8_t rssi,
                                   const int32_t longitude, const int32_t latitude) {
    peer_t* peer = erl_handler_touch_peer(address, rssi);
    if (peer == NULL)
        return false;

    peer->longitude = longitude;
    peer->latitude = latitude;
    return true;
}

bool erl_handler_set_peer_info(const uint16_t address, const int8_t rssi,
                               const uint32_t uptime, const uint32_t config) {
    peer_t* peer = erl_handler_touch_peer(address, rssi);
    if (peer == NULL)
        return false;

    peer->uptime = uptime;
    peer->config = config;
    return true;
}

bool erl_handler_set_peer_compass(const uint16_t address, const int8_t rssi,
                                  const int16_t x, const int16_t y, const int16_t z) {
    peer_t* peer = erl_handler_touch_peer(address, rssi);
    if (peer == NULL)
        return false;

    peer->compass_x = x;
    peer->compass_y = y;
    peer->compass_z = z;
    return true;
}

uint16_t erl_handler_build_packet(const erl_packet_t* packet, uint8_t* out, const uint16_t out_size) {
    const uint16_t packet_size = sizeof(erl_packet_t) + packet->length + sizeof(uint8_t);

    if (packet->length > ERL_PROTOCOL_PAYLOAD_MAX_SIZE || packet_size > out_size) {
        return 0;
    }

    erl_packet_t* out_packet = (erl_packet_t*)out;
    out_packet->sync = ERL_PROTOCOL_SYNC_BYTE;
    out_packet->version = ERL_PROTOCOL_VERSION;
    out_packet->address = (uint16_t)g_device_network_info.address;
    out_packet->receiver = packet->receiver;
    out_packet->type = packet->type;
    out_packet->length = packet->length;
    memcpy(out_packet->payload, packet->payload, packet->length);

    out[packet_size - 1] = crc8_calc(out, packet_size - 1, ERL_PROTOCOL_CRC8_POLYNOMIAL);

    return packet_size;
}

uint16_t erl_handler_print(const network_t* network, const erl_packet_t* packet, char* out, const uint16_t out_size) {
    uint16_t offset = 0;
    if (network->address == packet->address) {
        offset = sprintf(out, "(TX)[0x%X -> 0x%X]: type=0x%X, len=%i, payload={", packet->address, packet->receiver, packet->type, packet->length);
    } else {
        offset = sprintf(out, "(RX)[0x%X <- 0x%X]: type=0x%X, len=%i, payload={", packet->address, packet->receiver, packet->type, packet->length);
    }

    for (uint8_t idx = 0; idx < packet->length && offset < out_size; ++idx) {
        offset += snprintf(out + offset, out_size - offset, "%02X ", packet->payload[idx]);
    }

    if (offset < out_size) {
        offset += sprintf(out + offset, "}\r\n");
    }

    return offset;
}
