#include "protocol/merl_handler.h"

#include <stdio.h>
#include <string.h>

#include <pico/time.h>

#include "build_info.h"
#include "compass_handler.h"
#include "flash.h"
#include "gps_handler.h"
#include "network.h"
#include "uart.h"
#include "protocol/erl.h"
#include "protocol/erl_handler.h"

static_assert(MERL_USER_MESSAGE_MAX_SIZE == ERL_PROTOCOL_PAYLOAD_MAX_SIZE, "MERL and ERL message size differ");

#ifdef DEBUG_MODE
    #define MERL_LOG(...) printf(__VA_ARGS__)
    #define MERL_FRAME_PRINT(__frame, __tx) \
        do { \
            merl_handler_print(__frame, __tx, g_debug_buf, sizeof(g_debug_buf)); \
            printf("%s", g_debug_buf); \
        } while (0)
#else
    #define MERL_LOG(...)
    #define MERL_FRAME_PRINT(__frame, __tx)
#endif

#ifdef DEBUG_MODE
static char g_debug_buf[192];
#endif

static merl_parser_t g_parser;
static absolute_time_t g_rx_last;
static merl_send_user_message_fn g_send_user_message;

static void send_frame(const uint8_t direction, const merl_command_t command, const uint8_t* payload,
                       const uint8_t length) {
    uint8_t frame[MERL_PROTOCOL_FRAME_MAX_SIZE];
    const size_t size = merl_frame_encode(frame, sizeof(frame), direction, command, payload, length);
    if (size == 0)
        return;

    MERL_FRAME_PRINT(frame, true);
    uart_write(UART_MAIN, frame, size);
}

static void send_answer(const merl_command_t command, const uint8_t* payload, const uint8_t length) {
    send_frame(MERL_PROTOCOL_DIRECTION_ANSWER, command, payload, length);
}

static void send_status(const merl_command_t status, const merl_command_t request_command) {
    uint8_t frame[MERL_PROTOCOL_FRAME_MAX_SIZE];
    const size_t size = merl_status_encode(frame, sizeof(frame), status, request_command);

    MERL_FRAME_PRINT(frame, true);
    uart_write(UART_MAIN, frame, size);
}

static void handle_network_get(void) {
    const merl_network_params_t params = {
        .address = (uint16_t)g_device_network_info.address,
        .subnet_mask = (uint16_t)g_device_network_info.subnet_mask,
    };
    uint8_t body[MERL_NETWORK_PARAMS_SIZE];
    merl_network_params_encode(body, &params);
    send_answer(MERL_CMD_RESPONSE_OK_BODY, body, sizeof(body));
}

static void handle_network_set(const uint8_t* payload, const uint8_t length) {
    if (length != MERL_NETWORK_PARAMS_SIZE) {
        send_status(MERL_CMD_RESPONSE_ERROR, MERL_CMD_NETWORK_SET);
        return;
    }

    merl_network_params_t params;
    merl_network_params_decode(payload, &params);
    const network_t network = {.address = params.address, .subnet_mask = params.subnet_mask};

    if (!flash_network_save(&network)) {
        MERL_LOG("[MERL] Failed to save network params\r\n");
        send_status(MERL_CMD_RESPONSE_ERROR, MERL_CMD_NETWORK_SET);
        return;
    }

    erl_handler_network_init(&network);
    send_status(MERL_CMD_RESPONSE_OK, MERL_CMD_NETWORK_SET);
}

static void handle_peers_get(void) {
    uint8_t body[MERL_PROTOCOL_PAYLOAD_MAX_SIZE];
    size_t size = 0;

    for (size_t i = 0; i < NETWORK_MAX_PEERS; ++i) {
        if (g_peers_table[i].address == 0)
            continue;
        if (size + MERL_PEER_SIZE > sizeof(body))
            break;

        const peer_t* entry = &g_peers_table[i];
        const merl_peer_t peer = {
            .address = entry->address,
            .rssi = entry->rssi,
            .uptime = entry->uptime,
            .longitude = entry->longitude,
            .latitude = entry->latitude,
            .config = entry->config,
            .compass_x = entry->compass_x,
            .compass_y = entry->compass_y,
            .compass_z = entry->compass_z,
        };
        merl_peer_encode(&body[size], &peer);
        size += MERL_PEER_SIZE;
    }

    send_answer(MERL_CMD_RESPONSE_OK_BODY, body, (uint8_t)size);
}

static void handle_peers_clear(void) {
    memset(g_peers_table, 0, sizeof(g_peers_table));
    erl_handler_links_clear();
    send_status(MERL_CMD_RESPONSE_OK, MERL_CMD_PEERS_CLEAR);
}

static void handle_radio_stats_get(void) {
    const merl_radio_stats_t stats = {
        .uptime_ms = to_ms_since_boot(get_absolute_time()),
        .rx_ok = g_radio_stats.rx_ok,
        .rx_crc_error = g_radio_stats.rx_crc_error,
        .rx_bad_frame = g_radio_stats.rx_bad_frame,
        .rx_not_for_us = g_radio_stats.rx_not_for_us,
        .rx_queue_drop = g_radio_stats.rx_queue_drop,
        .tx_ok = g_radio_stats.tx_ok,
        .tx_error = g_radio_stats.tx_error,
        .tx_airtime_ms = g_radio_stats.tx_airtime_ms,
        .lbt_busy = g_radio_stats.lbt_busy,
        .tx_queue_drop = g_radio_stats.tx_queue_drop,
        .noise_dbm = g_radio_stats.noise_dbm,
        .noise_min_dbm = g_radio_stats.noise_min_dbm,
        .noise_avg_dbm = g_radio_stats.noise_avg_dbm,
    };
    uint8_t body[MERL_RADIO_STATS_SIZE];
    merl_radio_stats_encode(body, &stats);
    send_answer(MERL_CMD_RESPONSE_OK_BODY, body, sizeof(body));
}

// the peers heard most recently first, as many as fit into one answer
static void handle_link_stats_get(void) {
    uint8_t body[MERL_PROTOCOL_PAYLOAD_MAX_SIZE];
    size_t size = 0;
    bool taken[NETWORK_MAX_PEERS] = {false};
    const uint32_t now = to_ms_since_boot(get_absolute_time());

    while (size + MERL_LINK_STATS_SIZE <= sizeof(body)) {
        int best = -1;
        for (int i = 0; i < NETWORK_MAX_PEERS; ++i) {
            const peer_link_t* link = &g_peer_links[i];
            if (taken[i] || link->address == 0 || link->rx_packets == 0)
                continue;
            if (best < 0 || link->last_rx_ms > g_peer_links[best].last_rx_ms)
                best = i;
        }
        if (best < 0)
            break;
        taken[best] = true;
        const peer_link_t* link = &g_peer_links[best];
        const merl_link_stats_t stats = {
            .address = link->address,
            .rx_packets = link->rx_packets,
            .hb_received = link->hb_received,
            .hb_missed = link->hb_missed,
            .rssi_dbm = link->rssi,
            .snr_q4 = link->snr_q4,
            .freq_error_hz = link->freq_error_hz,
            .age_ms = now - link->last_rx_ms,
            .rssi_sum = link->rssi_sum,
            .snr_q4_sum = link->snr_q4_sum,
        };
        merl_link_stats_encode(&body[size], &stats);
        size += MERL_LINK_STATS_SIZE;
    }
    send_answer(MERL_CMD_RESPONSE_OK_BODY, body, (uint8_t)size);
}

static void handle_user_message_send(const uint8_t* payload, const uint8_t length) {
    const uint8_t address_size = sizeof(uint32_t);

    if (g_send_user_message == NULL) {
        send_status(MERL_CMD_RESPONSE_UNIMPLEMENTED, MERL_CMD_USER_MESSAGE_SEND);
        return;
    }

    // the message needs at least the \r\n and has to fit into an ERL packet
    if (length < address_size + 2 || length - address_size > MERL_USER_MESSAGE_MAX_SIZE) {
        send_status(MERL_CMD_RESPONSE_ERROR, MERL_CMD_USER_MESSAGE_SEND);
        return;
    }

    const uint8_t* message = &payload[address_size];
    const uint8_t message_length = length - address_size;
    const uint32_t address = merl_get_be32(payload);

    if (address > UINT16_MAX || message[message_length - 2] != '\r' || message[message_length - 1] != '\n') {
        send_status(MERL_CMD_RESPONSE_ERROR, MERL_CMD_USER_MESSAGE_SEND);
        return;
    }

    if (!g_send_user_message((uint16_t)address, message, message_length)) {
        send_status(MERL_CMD_RESPONSE_OVERLOAD, MERL_CMD_USER_MESSAGE_SEND);
        return;
    }

    send_status(MERL_CMD_RESPONSE_OK, MERL_CMD_USER_MESSAGE_SEND);
}

static void handle_position_get(void) {
    gps_position_t position;

    if (!gps_handler_get_position(&position)) {
        send_status(MERL_CMD_RESPONSE_ERROR, MERL_CMD_POSITION_GET);
        return;
    }

    const merl_position_t answer = {
        .time = position.time,
        .longitude = position.longitude,
        .latitude = position.latitude,
    };
    uint8_t body[MERL_POSITION_SIZE];
    merl_position_encode(body, &answer);
    send_answer(MERL_CMD_RESPONSE_OK_BODY, body, sizeof(body));
}

static void handle_position_set(const uint8_t* payload, const uint8_t length) {
    if (length != MERL_POSITION_SET_SIZE) {
        send_status(MERL_CMD_RESPONSE_ERROR, MERL_CMD_POSITION_SET);
        return;
    }

    merl_position_set_t position;
    merl_position_set_decode(payload, &position);

    if (!gps_handler_set_position(position.longitude, position.latitude)) {
        send_status(MERL_CMD_RESPONSE_UNIMPLEMENTED, MERL_CMD_POSITION_SET);
        return;
    }

    send_status(MERL_CMD_RESPONSE_OK, MERL_CMD_POSITION_SET);
}

static void handle_gps_status(void) {
    gps_status_t status;

    if (!gps_handler_get_status(&status)) {
        send_status(MERL_CMD_RESPONSE_UNIMPLEMENTED, MERL_CMD_GPS_STATUS);
        return;
    }

    const merl_gps_status_t answer = {
        .fix = status.fix,
        .satellites_used = status.satellites_used,
        .satellites_in_view = status.satellites_in_view,
        .hdop_x100 = status.hdop_x100,
    };
    uint8_t body[MERL_GPS_STATUS_SIZE];
    merl_gps_status_encode(body, &answer);
    send_answer(MERL_CMD_RESPONSE_OK_BODY, body, sizeof(body));
}

static void handle_compass_get(void) {
    compass_reading_t reading;

    if (!compass_handler_get(&reading)) {
        send_status(MERL_CMD_RESPONSE_ERROR, MERL_CMD_COMPASS_GET);
        return;
    }

    const merl_compass_t answer = {.x = reading.x, .y = reading.y, .z = reading.z};
    uint8_t body[MERL_COMPASS_SIZE];
    merl_compass_encode(body, &answer);
    send_answer(MERL_CMD_RESPONSE_OK_BODY, body, sizeof(body));
}

static void handle_compass_set(const uint8_t* payload, const uint8_t length) {
    if (length != MERL_COMPASS_SIZE) {
        send_status(MERL_CMD_RESPONSE_ERROR, MERL_CMD_COMPASS_SET);
        return;
    }

    merl_compass_t reading;
    merl_compass_decode(payload, &reading);

    if (!compass_handler_set(reading.x, reading.y, reading.z)) {
        send_status(MERL_CMD_RESPONSE_UNIMPLEMENTED, MERL_CMD_COMPASS_SET);
        return;
    }

    send_status(MERL_CMD_RESPONSE_OK, MERL_CMD_COMPASS_SET);
}

static void handle_device_version(void) {
    merl_device_version_t version = {
        .major = CONFIG_DEVICE_MAJOR_VERSION,
        .minor = CONFIG_DEVICE_MINOR_VERSION,
        .build_number = g_build_number,
    };
    // g_build_date/g_build_time are NUL-terminated ("Sep 27 2026\0", "14:23:01\0"); the wire
    // format drops the NUL since both __DATE__/__TIME__ formats are a fixed, known width.
    memcpy(version.build_date, g_build_date, MERL_DEVICE_BUILD_DATE_SIZE);
    memcpy(version.build_time, g_build_time, MERL_DEVICE_BUILD_TIME_SIZE);

    uint8_t body[MERL_DEVICE_VERSION_SIZE];
    merl_device_version_encode(body, &version);
    send_answer(MERL_CMD_RESPONSE_OK_BODY, body, sizeof(body));
}

static void handle_device_features(void) {
    uint8_t body[8];   // room for future features; grow if this firmware ever compiles in more
    size_t size = 0;

#ifdef FEATURE_GPS
    body[size++] = MERL_FEATURE_GPS;
#endif
#ifdef FEATURE_COMPASS
    body[size++] = MERL_FEATURE_COMPASS;
#endif
    body[size++] = MERL_FEATURE_LINK_STATS;     // always: the radio is always there

    send_answer(MERL_CMD_RESPONSE_OK_BODY, body, (uint8_t)size);
}

static void handle_request(const merl_command_t command, const uint8_t* payload, const uint8_t length) {
    switch (command) {
        case MERL_CMD_NETWORK_GET:
            handle_network_get();
            break;
        case MERL_CMD_NETWORK_SET:
            handle_network_set(payload, length);
            break;
        case MERL_CMD_PEERS_GET:
            handle_peers_get();
            break;
        case MERL_CMD_PEERS_CLEAR:
            handle_peers_clear();
            break;
        case MERL_CMD_RADIO_STATS_GET:
            handle_radio_stats_get();
            break;
        case MERL_CMD_LINK_STATS_GET:
            handle_link_stats_get();
            break;
        case MERL_CMD_USER_MESSAGE_SEND:
            handle_user_message_send(payload, length);
            break;
        case MERL_CMD_POSITION_GET:
            handle_position_get();
            break;
        case MERL_CMD_POSITION_SET:
            handle_position_set(payload, length);
            break;
        case MERL_CMD_GPS_STATUS:
            handle_gps_status();
            break;
        case MERL_CMD_COMPASS_GET:
            handle_compass_get();
            break;
        case MERL_CMD_COMPASS_SET:
            handle_compass_set(payload, length);
            break;
        case MERL_CMD_DEVICE_VERSION:
            handle_device_version();
            break;
        case MERL_CMD_DEVICE_FEATURES:
            handle_device_features();
            break;
        default:
            send_status(MERL_CMD_RESPONSE_UNKNOWN, command);
            break;
    }
}

static void handle_answer(const merl_command_t command) {
    if (command != MERL_CMD_RESPONSE_OK)
        MERL_LOG("[MERL] Main unit answered 0x%04X to our request\r\n", command);
}

static void feed(const uint8_t byte) {
    merl_frame_t frame;
    if (!merl_parser_feed(&g_parser, byte, &frame))
        return;

    MERL_FRAME_PRINT(frame.payload - MERL_OFFSET_PAYLOAD, false);

    if (frame.direction == MERL_PROTOCOL_DIRECTION_REQUEST) {
        handle_request(frame.command, frame.payload, frame.length);
    } else {
        handle_answer(frame.command);
    }
}

bool merl_handler_forward_user_message(const uint16_t sender, const uint16_t receiver, const uint8_t* message,
                                       uint8_t length) {
    if (message == NULL)
        return false;

    // the payload encoding appends the \r\n itself
    if (length >= 2 && message[length - 2] == '\r' && message[length - 1] == '\n')
        length -= 2;

    uint8_t payload[MERL_USER_MESSAGE_ADDRESS_SIZE + MERL_USER_MESSAGE_MAX_SIZE];
    const uint32_t address = (uint32_t)receiver << 16 | sender;
    const uint8_t size = merl_user_message_encode(payload, sizeof(payload), address, message, length);
    if (size == 0)
        return false;

    send_frame(MERL_PROTOCOL_DIRECTION_REQUEST, MERL_CMD_USER_MESSAGE_SEND, payload, size);
    return true;
}

bool merl_handler_passthrough_message(const uint8_t* message, const uint8_t length) {
    if (message == NULL || length > MERL_PROTOCOL_PAYLOAD_MAX_SIZE)
        return false;

    send_frame(MERL_PROTOCOL_DIRECTION_REQUEST, MERL_CMD_USER_MESSAGE_PASSTHROUGH, message, length);
    return true;
}

uint16_t merl_handler_print(const uint8_t* frame, const bool tx, char* out, const uint16_t out_size) {
    if (out_size == 0)
        return 0;

    const uint8_t length = frame[MERL_OFFSET_LENGTH];

    int written = snprintf(out, out_size, "(MERL %s)[%c]: command=0x%04X, len=%i, payload={", tx ? "TX" : "RX",
                           frame[MERL_OFFSET_DIRECTION], merl_get_be16(&frame[MERL_OFFSET_COMMAND]), length);
    uint16_t offset = written < 0 ? 0 : (written < out_size ? (uint16_t)written : (uint16_t)(out_size - 1));

    // keep room for "...}\r\n" and the terminating NUL
    bool truncated = false;
    for (uint8_t idx = 0; idx < length; ++idx) {
        if (offset + 3 + 7 > out_size) {
            truncated = true;
            break;
        }
        offset += snprintf(out + offset, out_size - offset, "%02X ", frame[MERL_OFFSET_PAYLOAD + idx]);
    }

    written = snprintf(out + offset, out_size - offset, "%s}\r\n", truncated ? "... " : "");
    if (written > 0)
        offset = written < out_size - offset ? (uint16_t)(offset + written) : (uint16_t)(out_size - 1);

    return offset;
}

void merl_handler_init(const merl_send_user_message_fn send_user_message) {
    g_send_user_message = send_user_message;
    merl_parser_reset(&g_parser);
    g_rx_last = get_absolute_time();
}

void merl_handler_service(void) {
    uint8_t chunk[32];
    size_t count = uart_read(UART_MAIN, chunk, sizeof(chunk));

    if (count == 0) {
        if (merl_parser_busy(&g_parser) &&
            absolute_time_diff_us(g_rx_last, get_absolute_time()) > MERL_PROTOCOL_RX_TIMEOUT_MS * 1000) {
            MERL_LOG("[MERL] Dropped partial request, timeout\r\n");
            merl_parser_reset(&g_parser);
        }
        return;
    }

    do {
        g_rx_last = get_absolute_time();
        for (size_t i = 0; i < count; ++i)
            feed(chunk[i]);
        count = uart_read(UART_MAIN, chunk, sizeof(chunk));
    } while (count > 0);
}
