// Host-side checks of the MERL codec. Build: see ../CMakeLists.txt (top-level only).
// Reference frames were produced by emergencyRadioLink/test/merl_link.py (Frame.encode).
#include <stdio.h>
#include <string.h>

#include "merl.h"

static int g_failures;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

static void check_bytes(const uint8_t* got, size_t got_size, const uint8_t* want, size_t want_size, const char* what) {
    if (got_size == want_size && memcmp(got, want, want_size) == 0)
        return;
    printf("FAIL %s:\n  got ", what);
    for (size_t i = 0; i < got_size; ++i)
        printf("%02x ", got[i]);
    printf("\n  want ");
    for (size_t i = 0; i < want_size; ++i)
        printf("%02x ", want[i]);
    printf("\n");
    ++g_failures;
}

static void test_encode(void) {
    uint8_t frame[MERL_PROTOCOL_FRAME_MAX_SIZE];

    // README: set network params 0xCF01 / 0xFFE0
    const merl_network_params_t params = {.address = 0xCF01, .subnet_mask = 0xFFE0};
    uint8_t body[MERL_NETWORK_PARAMS_SIZE];
    merl_network_params_encode(body, &params);
    size_t size = merl_frame_encode(frame, sizeof(frame), MERL_PROTOCOL_DIRECTION_REQUEST, MERL_CMD_NETWORK_SET,
                                    body, sizeof(body));
    const uint8_t network_set[] = {0x2a, 0x61, 0x3e, 0x10, 0x01, 0x04, 0xcf, 0x01, 0xff, 0xe0, 0x39};
    check_bytes(frame, size, network_set, sizeof(network_set), "NETWORK_SET request");

    // README: its answer
    size = merl_status_encode(frame, sizeof(frame), MERL_CMD_RESPONSE_OK, MERL_CMD_NETWORK_SET);
    const uint8_t network_set_ok[] = {0x2a, 0x61, 0x3c, 0x00, 0x00, 0x02, 0x10, 0x01, 0x32};
    check_bytes(frame, size, network_set_ok, sizeof(network_set_ok), "OK answer");

    // README: get peer table, empty payload
    size = merl_frame_encode(frame, sizeof(frame), MERL_PROTOCOL_DIRECTION_REQUEST, MERL_CMD_PEERS_GET, NULL, 0);
    const uint8_t peers_get[] = {0x2a, 0x61, 0x3e, 0x20, 0x00, 0x00, 0x69};
    check_bytes(frame, size, peers_get, sizeof(peers_get), "PEERS_GET request");

    CHECK(merl_frame_encode(frame, sizeof(frame), MERL_PROTOCOL_DIRECTION_REQUEST, MERL_CMD_PEERS_GET, NULL,
                            MERL_PROTOCOL_PAYLOAD_MAX_SIZE + 1) == 0);
    CHECK(merl_frame_encode(frame, 6, MERL_PROTOCOL_DIRECTION_REQUEST, MERL_CMD_PEERS_GET, NULL, 0) == 0);
}

static void test_parser(void) {
    merl_parser_t parser;
    merl_parser_reset(&parser);
    merl_frame_t frame;

    // garbage, a false start ("*x"), a frame with a bad crc, then a good frame
    const uint8_t stream[] = {
        0x00, 0x2a, 0x78,
        0x2a, 0x61, 0x3c, 0x00, 0x00, 0x02, 0x10, 0x01, 0x33,
        0x2a, 0x61, 0x3c, 0x00, 0x00, 0x02, 0x10, 0x01, 0x32,
    };
    int frames = 0;
    for (size_t i = 0; i < sizeof(stream); ++i) {
        if (merl_parser_feed(&parser, stream[i], &frame)) {
            ++frames;
            CHECK(frame.direction == MERL_PROTOCOL_DIRECTION_ANSWER);
            CHECK(frame.command == MERL_CMD_RESPONSE_OK);
            CHECK(frame.length == 2);
            CHECK(merl_get_be16(frame.payload) == MERL_CMD_NETWORK_SET);
        }
    }
    CHECK(frames == 1);
    CHECK(!merl_parser_busy(&parser));

    // a sync byte where the version is expected restarts the frame
    const uint8_t restart[] = {0x2a, 0x2a, 0x61, 0x3e, 0x20, 0x00, 0x00, 0x69};
    frames = 0;
    for (size_t i = 0; i < sizeof(restart); ++i)
        frames += merl_parser_feed(&parser, restart[i], &frame);
    CHECK(frames == 1);
    CHECK(frame.command == MERL_CMD_PEERS_GET && frame.length == 0);

    // partial frame
    merl_parser_feed(&parser, 0x2a, &frame);
    CHECK(merl_parser_busy(&parser));
    merl_parser_reset(&parser);
    CHECK(!merl_parser_busy(&parser));
}

static void test_payloads(void) {
    // peer_t is little-endian and packed: merl_link.py unpacks it as '<IbIii' + 3 config bytes + '<hhh'
    const merl_peer_t peer = {
        .address = 0xC70A, .rssi = -87, .uptime = 3600, .longitude = 30523456, .latitude = -50450100,
        .config = 0x123456, .compass_x = -1200, .compass_y = 300, .compass_z = 16000,
    };
    uint8_t entry[MERL_PEER_SIZE];
    merl_peer_encode(entry, &peer);
    const uint8_t want[] = {0x0a, 0xc7, 0x00, 0x00, 0xa9, 0x10, 0x0e, 0x00, 0x00, 0x40, 0xc0, 0xd1,
                            0x01, 0x4c, 0x31, 0xfe, 0xfc, 0x56, 0x34, 0x12, 0x50, 0xfb, 0x2c, 0x01, 0x80, 0x3e};
    check_bytes(entry, sizeof(entry), want, sizeof(want), "peer entry");

    merl_peer_t back;
    merl_peer_decode(entry, &back);
    CHECK(back.address == peer.address && back.rssi == peer.rssi && back.uptime == peer.uptime);
    CHECK(back.longitude == peer.longitude && back.latitude == peer.latitude && back.config == peer.config);
    CHECK(back.compass_x == peer.compass_x && back.compass_y == peer.compass_y && back.compass_z == peer.compass_z);

    const merl_position_t position = {.time = 45296000, .longitude = -73985000, .latitude = 40748000};
    uint8_t body[MERL_POSITION_SIZE];
    merl_position_encode(body, &position);
    merl_position_t position_back;
    merl_position_decode(body, &position_back);
    CHECK(body[4] == 0xfb);     // big-endian, negative longitude
    CHECK(position_back.time == position.time && position_back.longitude == position.longitude &&
          position_back.latitude == position.latitude);

    CHECK(strcmp(merl_command_name(MERL_CMD_USER_MESSAGE_PASSTHROUGH), "USER_MESSAGE_PASSTHROUGH") == 0);
    CHECK(merl_command_name(0x7777) == NULL);

    merl_device_version_t version = {.major = 1, .minor = 0, .build_number = 0x2a};
    memcpy(version.build_date, "Sep 27 2026", MERL_DEVICE_BUILD_DATE_SIZE);
    memcpy(version.build_time, "14:23:01", MERL_DEVICE_BUILD_TIME_SIZE);
    uint8_t version_body[MERL_DEVICE_VERSION_SIZE];
    merl_device_version_encode(version_body, &version);
    const uint8_t version_want[] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x2a,
                                    'S', 'e', 'p', ' ', '2', '7', ' ', '2', '0', '2', '6',
                                    '1', '4', ':', '2', '3', ':', '0', '1'};
    check_bytes(version_body, sizeof(version_body), version_want, sizeof(version_want), "device version");

    merl_device_version_t version_back;
    merl_device_version_decode(version_body, &version_back);
    CHECK(version_back.major == version.major && version_back.minor == version.minor &&
          version_back.build_number == version.build_number);
    CHECK(memcmp(version_back.build_date, version.build_date, MERL_DEVICE_BUILD_DATE_SIZE) == 0);
    CHECK(memcmp(version_back.build_time, version.build_time, MERL_DEVICE_BUILD_TIME_SIZE) == 0);

    CHECK(strcmp(merl_command_name(MERL_CMD_DEVICE_VERSION), "DEVICE_VERSION") == 0);
    CHECK(strcmp(merl_command_name(MERL_CMD_DEVICE_FEATURES), "DEVICE_FEATURES") == 0);
    CHECK(strcmp(merl_feature_name(MERL_FEATURE_GPS), "GPS") == 0);
    CHECK(strcmp(merl_feature_name(MERL_FEATURE_COMPASS), "COMPASS") == 0);
    CHECK(merl_feature_name(0xFF) == NULL);

    const merl_compass_t compass = {.x = -1200, .y = 300, .z = 16000};
    uint8_t compass_body[MERL_COMPASS_SIZE];
    merl_compass_encode(compass_body, &compass);
    const uint8_t compass_want[] = {0xfb, 0x50, 0x01, 0x2c, 0x3e, 0x80};   // big-endian, signed
    check_bytes(compass_body, sizeof(compass_body), compass_want, sizeof(compass_want), "compass");

    merl_compass_t compass_back;
    merl_compass_decode(compass_body, &compass_back);
    CHECK(compass_back.x == compass.x && compass_back.y == compass.y && compass_back.z == compass.z);

    const merl_gps_status_t gps = {.fix = MERL_GPS_FIX_GPS, .satellites_used = 7, .satellites_in_view = 12, .hdop_x100 = 135};
    uint8_t gps_body[MERL_GPS_STATUS_SIZE];
    merl_gps_status_encode(gps_body, &gps);
    const uint8_t gps_want[] = {0x01, 0x07, 0x0c, 0x00, 0x87};
    check_bytes(gps_body, sizeof(gps_body), gps_want, sizeof(gps_want), "gps status");
    merl_gps_status_t gps_back;
    merl_gps_status_decode(gps_body, &gps_back);
    CHECK(gps_back.fix == gps.fix && gps_back.satellites_used == 7 && gps_back.satellites_in_view == 12 &&
          gps_back.hdop_x100 == 135);
    CHECK(strcmp(merl_command_name(MERL_CMD_GPS_STATUS), "GPS_STATUS") == 0);

    CHECK(strcmp(merl_command_name(MERL_CMD_COMPASS_GET), "COMPASS_GET") == 0);
    CHECK(strcmp(merl_command_name(MERL_CMD_COMPASS_SET), "COMPASS_SET") == 0);

    const merl_radio_stats_t radio = {.uptime_ms = 123456, .rx_ok = 10, .rx_crc_error = 2, .rx_bad_frame = 1,
                                      .rx_not_for_us = 3, .rx_queue_drop = 0, .tx_ok = 20, .tx_error = 1,
                                      .tx_airtime_ms = 4567, .lbt_busy = 5, .tx_queue_drop = 6,
                                      .noise_dbm = -112, .noise_min_dbm = -118, .noise_avg_dbm = -114};
    uint8_t radio_body[MERL_RADIO_STATS_SIZE];
    merl_radio_stats_encode(radio_body, &radio);
    CHECK(radio_body[0] == 0x00 && radio_body[1] == 0x01 && radio_body[2] == 0xe2 && radio_body[3] == 0x40);
    CHECK(radio_body[44] == (uint8_t)-112);
    merl_radio_stats_t radio_back;
    merl_radio_stats_decode(radio_body, &radio_back);
    CHECK(radio_back.uptime_ms == 123456 && radio_back.rx_crc_error == 2 && radio_back.tx_airtime_ms == 4567 &&
          radio_back.tx_queue_drop == 6 && radio_back.noise_dbm == -112 && radio_back.noise_min_dbm == -118 &&
          radio_back.noise_avg_dbm == -114);

    const merl_link_stats_t link = {.address = 0xC702, .rx_packets = 77, .hb_received = 60, .hb_missed = 4,
                                    .rssi_dbm = -121, .snr_q4 = -58, .freq_error_hz = -1234, .age_ms = 2500,
                                    .rssi_sum = -9000, .snr_q4_sum = -3100};
    uint8_t link_body[MERL_LINK_STATS_SIZE];
    merl_link_stats_encode(link_body, &link);
    CHECK(link_body[0] == 0xc7 && link_body[1] == 0x02);
    merl_link_stats_t link_back;
    merl_link_stats_decode(link_body, &link_back);
    CHECK(link_back.address == 0xC702 && link_back.rx_packets == 77 && link_back.hb_missed == 4 &&
          link_back.rssi_dbm == -121 && link_back.snr_q4 == -58 && link_back.freq_error_hz == -1234 &&
          link_back.age_ms == 2500 && link_back.rssi_sum == -9000 && link_back.snr_q4_sum == -3100);
    CHECK(strcmp(merl_command_name(MERL_CMD_LINK_STATS_GET), "LINK_STATS_GET") == 0);
    CHECK(strcmp(merl_feature_name(MERL_FEATURE_LINK_STATS), "LINK_STATS") == 0);
}

static void test_user_message(void) {
    uint8_t payload[MERL_PROTOCOL_PAYLOAD_MAX_SIZE];

    // device -> main: receiver 0xC71F (broadcast) << 16 | sender 0xC705
    const uint8_t size = merl_user_message_encode(payload, sizeof(payload), 0xC71FC705u, (const uint8_t*)"Hi!", 3);
    const uint8_t want[] = {0xc7, 0x1f, 0xc7, 0x05, 'H', 'i', '!', '\r', '\n'};
    check_bytes(payload, size, want, sizeof(want), "user message payload");

    uint32_t address = 0;
    const uint8_t* text = NULL;
    uint8_t text_length = 0;
    CHECK(merl_user_message_decode(payload, size, &address, &text, &text_length));
    CHECK(address == 0xC71FC705u && text_length == 3 && memcmp(text, "Hi!", 3) == 0);

    // empty text is still a valid message
    CHECK(merl_user_message_encode(payload, sizeof(payload), 1, NULL, 0) == 6);
    CHECK(merl_user_message_decode(payload, 6, &address, &text, &text_length) && text_length == 0);

    // too long, too short, missing \r\n
    uint8_t text_max[MERL_USER_MESSAGE_TEXT_MAX_SIZE + 1];
    memset(text_max, 'x', sizeof(text_max));
    CHECK(merl_user_message_encode(payload, sizeof(payload), 1, text_max, MERL_USER_MESSAGE_TEXT_MAX_SIZE) ==
          MERL_USER_MESSAGE_ADDRESS_SIZE + MERL_USER_MESSAGE_MAX_SIZE);
    CHECK(merl_user_message_encode(payload, sizeof(payload), 1, text_max, sizeof(text_max)) == 0);
    CHECK(merl_user_message_encode(payload, 8, 1, (const uint8_t*)"Hi!", 3) == 0);
    CHECK(!merl_user_message_decode(want, 5, &address, &text, &text_length));
    const uint8_t no_crlf[] = {0, 0, 0, 1, 'H', 'i'};
    CHECK(!merl_user_message_decode(no_crlf, sizeof(no_crlf), &address, &text, &text_length));
}

int main(void) {
    test_encode();
    test_parser();
    test_payloads();
    test_user_message();
    printf(g_failures ? "%d check(s) failed\n" : "all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
