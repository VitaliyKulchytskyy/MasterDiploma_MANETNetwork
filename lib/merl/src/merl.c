#include "merl.h"

#include <string.h>

uint8_t merl_crc8(const uint8_t* data, const size_t length) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc & 0x01) ? (uint8_t)((crc >> 1) ^ MERL_PROTOCOL_CRC8_POLYNOMIAL) : (uint8_t)(crc >> 1);
    }
    return crc;
}

size_t merl_frame_encode(uint8_t* out, const size_t out_size, const uint8_t direction, const merl_command_t command,
                         const uint8_t* payload, const uint8_t length) {
    const size_t size = MERL_OFFSET_PAYLOAD + length;
    if (length > MERL_PROTOCOL_PAYLOAD_MAX_SIZE || out_size < size + 1)
        return 0;

    out[MERL_OFFSET_SYNC] = MERL_PROTOCOL_SYNC_BYTE;
    out[MERL_OFFSET_VERSION] = MERL_PROTOCOL_VERSION;
    out[MERL_OFFSET_DIRECTION] = direction;
    merl_put_be16(&out[MERL_OFFSET_COMMAND], command);
    out[MERL_OFFSET_LENGTH] = length;
    if (length > 0)
        memcpy(&out[MERL_OFFSET_PAYLOAD], payload, length);

    out[size] = merl_crc8(out, size);
    return size + 1;
}

size_t merl_status_encode(uint8_t* out, const size_t out_size, const merl_command_t status,
                          const merl_command_t request_command) {
    uint8_t payload[2];
    merl_put_be16(payload, request_command);
    return merl_frame_encode(out, out_size, MERL_PROTOCOL_DIRECTION_ANSWER, status, payload, sizeof(payload));
}

void merl_parser_reset(merl_parser_t* parser) {
    parser->pos = 0;
}

bool merl_parser_busy(const merl_parser_t* parser) {
    return parser->pos > 0;
}

bool merl_parser_feed(merl_parser_t* parser, const uint8_t byte, merl_frame_t* out) {
    if (parser->pos == MERL_OFFSET_SYNC && byte != MERL_PROTOCOL_SYNC_BYTE)
        return false;

    if ((parser->pos == MERL_OFFSET_VERSION && byte != MERL_PROTOCOL_VERSION) ||
        (parser->pos == MERL_OFFSET_DIRECTION && byte != MERL_PROTOCOL_DIRECTION_REQUEST &&
         byte != MERL_PROTOCOL_DIRECTION_ANSWER) ||
        (parser->pos == MERL_OFFSET_LENGTH && byte > MERL_PROTOCOL_PAYLOAD_MAX_SIZE)) {
        // the offending byte may itself start the next frame
        parser->pos = 0;
        if (byte == MERL_PROTOCOL_SYNC_BYTE)
            parser->buf[parser->pos++] = byte;
        return false;
    }

    parser->buf[parser->pos++] = byte;
    if (parser->pos <= MERL_OFFSET_LENGTH)
        return false;

    const uint8_t length = parser->buf[MERL_OFFSET_LENGTH];
    const size_t frame_size = MERL_OFFSET_PAYLOAD + length + 1;
    if (parser->pos < frame_size)
        return false;

    parser->pos = 0;
    if (merl_crc8(parser->buf, frame_size - 1) != parser->buf[frame_size - 1])
        return false;

    out->direction = parser->buf[MERL_OFFSET_DIRECTION];
    out->command = merl_get_be16(&parser->buf[MERL_OFFSET_COMMAND]);
    out->length = length;
    out->payload = &parser->buf[MERL_OFFSET_PAYLOAD];
    return true;
}

void merl_network_params_encode(uint8_t out[MERL_NETWORK_PARAMS_SIZE], const merl_network_params_t* params) {
    merl_put_be16(&out[0], params->address);
    merl_put_be16(&out[2], params->subnet_mask);
}

void merl_network_params_decode(const uint8_t in[MERL_NETWORK_PARAMS_SIZE], merl_network_params_t* params) {
    params->address = merl_get_be16(&in[0]);
    params->subnet_mask = merl_get_be16(&in[2]);
}

void merl_position_encode(uint8_t out[MERL_POSITION_SIZE], const merl_position_t* position) {
    merl_put_be32(&out[0], position->time);
    merl_put_be32(&out[4], (uint32_t)position->longitude);
    merl_put_be32(&out[8], (uint32_t)position->latitude);
}

void merl_position_decode(const uint8_t in[MERL_POSITION_SIZE], merl_position_t* position) {
    position->time = merl_get_be32(&in[0]);
    position->longitude = (int32_t)merl_get_be32(&in[4]);
    position->latitude = (int32_t)merl_get_be32(&in[8]);
}

void merl_position_set_encode(uint8_t out[MERL_POSITION_SET_SIZE], const merl_position_set_t* position) {
    merl_put_be32(&out[0], (uint32_t)position->longitude);
    merl_put_be32(&out[4], (uint32_t)position->latitude);
}

void merl_position_set_decode(const uint8_t in[MERL_POSITION_SET_SIZE], merl_position_set_t* position) {
    position->longitude = (int32_t)merl_get_be32(&in[0]);
    position->latitude = (int32_t)merl_get_be32(&in[4]);
}

void merl_compass_encode(uint8_t out[MERL_COMPASS_SIZE], const merl_compass_t* compass) {
    merl_put_be16(&out[0], (uint16_t)compass->x);
    merl_put_be16(&out[2], (uint16_t)compass->y);
    merl_put_be16(&out[4], (uint16_t)compass->z);
}

void merl_compass_decode(const uint8_t in[MERL_COMPASS_SIZE], merl_compass_t* compass) {
    compass->x = (int16_t)merl_get_be16(&in[0]);
    compass->y = (int16_t)merl_get_be16(&in[2]);
    compass->z = (int16_t)merl_get_be16(&in[4]);
}

void merl_gps_status_encode(uint8_t out[MERL_GPS_STATUS_SIZE], const merl_gps_status_t* status) {
    out[0] = status->fix;
    out[1] = status->satellites_used;
    out[2] = status->satellites_in_view;
    merl_put_be16(&out[3], status->hdop_x100);
}

void merl_gps_status_decode(const uint8_t in[MERL_GPS_STATUS_SIZE], merl_gps_status_t* status) {
    status->fix = in[0];
    status->satellites_used = in[1];
    status->satellites_in_view = in[2];
    status->hdop_x100 = merl_get_be16(&in[3]);
}

void merl_radio_stats_encode(uint8_t out[MERL_RADIO_STATS_SIZE], const merl_radio_stats_t* stats) {
    const uint32_t counters[11] = {stats->uptime_ms, stats->rx_ok, stats->rx_crc_error, stats->rx_bad_frame,
                                   stats->rx_not_for_us, stats->rx_queue_drop, stats->tx_ok, stats->tx_error,
                                   stats->tx_airtime_ms, stats->lbt_busy, stats->tx_queue_drop};
    for (size_t i = 0; i < 11; ++i)
        merl_put_be32(&out[i * 4], counters[i]);
    out[44] = (uint8_t)stats->noise_dbm;
    out[45] = (uint8_t)stats->noise_min_dbm;
    out[46] = (uint8_t)stats->noise_avg_dbm;
}

void merl_radio_stats_decode(const uint8_t in[MERL_RADIO_STATS_SIZE], merl_radio_stats_t* stats) {
    uint32_t* const counters[11] = {&stats->uptime_ms, &stats->rx_ok, &stats->rx_crc_error, &stats->rx_bad_frame,
                                    &stats->rx_not_for_us, &stats->rx_queue_drop, &stats->tx_ok, &stats->tx_error,
                                    &stats->tx_airtime_ms, &stats->lbt_busy, &stats->tx_queue_drop};
    for (size_t i = 0; i < 11; ++i)
        *counters[i] = merl_get_be32(&in[i * 4]);
    stats->noise_dbm = (int8_t)in[44];
    stats->noise_min_dbm = (int8_t)in[45];
    stats->noise_avg_dbm = (int8_t)in[46];
}

void merl_link_stats_encode(uint8_t out[MERL_LINK_STATS_SIZE], const merl_link_stats_t* stats) {
    merl_put_be16(&out[0], stats->address);
    merl_put_be32(&out[2], stats->rx_packets);
    merl_put_be32(&out[6], stats->hb_received);
    merl_put_be32(&out[10], stats->hb_missed);
    out[14] = (uint8_t)stats->rssi_dbm;
    out[15] = (uint8_t)stats->snr_q4;
    merl_put_be32(&out[16], (uint32_t)stats->freq_error_hz);
    merl_put_be32(&out[20], stats->age_ms);
    merl_put_be32(&out[24], (uint32_t)stats->rssi_sum);
    merl_put_be32(&out[28], (uint32_t)stats->snr_q4_sum);
}

void merl_link_stats_decode(const uint8_t in[MERL_LINK_STATS_SIZE], merl_link_stats_t* stats) {
    stats->address = merl_get_be16(&in[0]);
    stats->rx_packets = merl_get_be32(&in[2]);
    stats->hb_received = merl_get_be32(&in[6]);
    stats->hb_missed = merl_get_be32(&in[10]);
    stats->rssi_dbm = (int8_t)in[14];
    stats->snr_q4 = (int8_t)in[15];
    stats->freq_error_hz = (int32_t)merl_get_be32(&in[16]);
    stats->age_ms = merl_get_be32(&in[20]);
    stats->rssi_sum = (int32_t)merl_get_be32(&in[24]);
    stats->snr_q4_sum = (int32_t)merl_get_be32(&in[28]);
}

static inline void put_le32(uint8_t* p, const uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static inline uint32_t get_le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void put_le16(uint8_t* p, const uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static inline uint16_t get_le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

void merl_device_version_encode(uint8_t out[MERL_DEVICE_VERSION_SIZE], const merl_device_version_t* version) {
    out[0] = version->major;
    out[1] = version->minor;
    merl_put_be32(&out[2], version->build_number);
    memcpy(&out[6], version->build_date, MERL_DEVICE_BUILD_DATE_SIZE);
    memcpy(&out[6 + MERL_DEVICE_BUILD_DATE_SIZE], version->build_time, MERL_DEVICE_BUILD_TIME_SIZE);
}

void merl_device_version_decode(const uint8_t in[MERL_DEVICE_VERSION_SIZE], merl_device_version_t* version) {
    version->major = in[0];
    version->minor = in[1];
    version->build_number = merl_get_be32(&in[2]);
    memcpy(version->build_date, &in[6], MERL_DEVICE_BUILD_DATE_SIZE);
    memcpy(version->build_time, &in[6 + MERL_DEVICE_BUILD_DATE_SIZE], MERL_DEVICE_BUILD_TIME_SIZE);
}

void merl_peer_encode(uint8_t out[MERL_PEER_SIZE], const merl_peer_t* peer) {
    put_le32(&out[0], peer->address);
    out[4] = (uint8_t)peer->rssi;
    put_le32(&out[5], peer->uptime);
    put_le32(&out[9], (uint32_t)peer->longitude);
    put_le32(&out[13], (uint32_t)peer->latitude);
    out[17] = (uint8_t)peer->config;
    out[18] = (uint8_t)(peer->config >> 8);
    out[19] = (uint8_t)(peer->config >> 16);
    put_le16(&out[20], (uint16_t)peer->compass_x);
    put_le16(&out[22], (uint16_t)peer->compass_y);
    put_le16(&out[24], (uint16_t)peer->compass_z);
}

void merl_peer_decode(const uint8_t in[MERL_PEER_SIZE], merl_peer_t* peer) {
    peer->address = get_le32(&in[0]);
    peer->rssi = (int8_t)in[4];
    peer->uptime = get_le32(&in[5]);
    peer->longitude = (int32_t)get_le32(&in[9]);
    peer->latitude = (int32_t)get_le32(&in[13]);
    peer->config = (uint32_t)in[17] | (uint32_t)in[18] << 8 | (uint32_t)in[19] << 16;
    peer->compass_x = (int16_t)get_le16(&in[20]);
    peer->compass_y = (int16_t)get_le16(&in[22]);
    peer->compass_z = (int16_t)get_le16(&in[24]);
}

uint8_t merl_user_message_encode(uint8_t* out, const size_t out_size, const uint32_t address, const uint8_t* text,
                                 const size_t text_length) {
    const size_t size = MERL_USER_MESSAGE_ADDRESS_SIZE + text_length + 2;
    if (text_length > MERL_USER_MESSAGE_TEXT_MAX_SIZE || size > out_size)
        return 0;

    merl_put_be32(out, address);
    if (text_length > 0)
        memcpy(&out[MERL_USER_MESSAGE_ADDRESS_SIZE], text, text_length);
    out[size - 2] = '\r';
    out[size - 1] = '\n';
    return (uint8_t)size;
}

bool merl_user_message_decode(const uint8_t* payload, const uint8_t length, uint32_t* address, const uint8_t** text,
                              uint8_t* text_length) {
    if (length < MERL_USER_MESSAGE_ADDRESS_SIZE + 2 ||
        length - MERL_USER_MESSAGE_ADDRESS_SIZE > MERL_USER_MESSAGE_MAX_SIZE ||
        payload[length - 2] != '\r' || payload[length - 1] != '\n')
        return false;

    *address = merl_get_be32(payload);
    *text = &payload[MERL_USER_MESSAGE_ADDRESS_SIZE];
    *text_length = (uint8_t)(length - MERL_USER_MESSAGE_ADDRESS_SIZE - 2);
    return true;
}

const char* merl_command_name(const merl_command_t command) {
    switch (command) {
        case MERL_CMD_RESPONSE_OK: return "OK";
        case MERL_CMD_RESPONSE_OK_BODY: return "OK_BODY";
        case MERL_CMD_RESPONSE_ERROR: return "ERROR";
        case MERL_CMD_RESPONSE_UNKNOWN: return "UNKNOWN";
        case MERL_CMD_RESPONSE_UNIMPLEMENTED: return "UNIMPLEMENTED";
        case MERL_CMD_RESPONSE_OVERLOAD: return "OVERLOAD";
        case MERL_CMD_NETWORK_GET: return "NETWORK_GET";
        case MERL_CMD_NETWORK_SET: return "NETWORK_SET";
        case MERL_CMD_PEERS_GET: return "PEERS_GET";
        case MERL_CMD_PEERS_CLEAR: return "PEERS_CLEAR";
        case MERL_CMD_RADIO_STATS_GET: return "RADIO_STATS_GET";
        case MERL_CMD_LINK_STATS_GET: return "LINK_STATS_GET";
        case MERL_CMD_POSITION_GET: return "POSITION_GET";
        case MERL_CMD_POSITION_SET: return "POSITION_SET";
        case MERL_CMD_COMPASS_GET: return "COMPASS_GET";
        case MERL_CMD_COMPASS_SET: return "COMPASS_SET";
        case MERL_CMD_GPS_STATUS: return "GPS_STATUS";
        case MERL_CMD_DEVICE_VERSION: return "DEVICE_VERSION";
        case MERL_CMD_DEVICE_FEATURES: return "DEVICE_FEATURES";
        case MERL_CMD_USER_MESSAGE_SEND: return "USER_MESSAGE_SEND";
        case MERL_CMD_USER_MESSAGE_PASSTHROUGH: return "USER_MESSAGE_PASSTHROUGH";
        default: return NULL;
    }
}

const char* merl_feature_name(const merl_feature_t feature) {
    switch (feature) {
        case MERL_FEATURE_GPS: return "GPS";
        case MERL_FEATURE_COMPASS: return "COMPASS";
        case MERL_FEATURE_LINK_STATS: return "LINK_STATS";
        default: return NULL;
    }
}
