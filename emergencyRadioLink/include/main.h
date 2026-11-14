#ifndef LINK_ROUTER_MAIN_H
#define LINK_ROUTER_MAIN_H

#ifdef DEBUG_MODE
    static char l_debugBuf[128]{};
    #define D_printf(...) printf(__VA_ARGS__)
    #define D_packet_print(__packet) \
        do { \
            const uint16_t __len = erl_handler_print(&g_device_network_info, __packet, l_debugBuf, sizeof(l_debugBuf)); \
            D_printf(l_debugBuf, __len); \
        } while(0)
#else
    static char l_debugBuf[1]{};
    #define D_printf(...)
    #define D_packet_print(__packet)
#endif

#define RADIO_FRAME_DATA_SIZE (sizeof(erl_packet_t) + ERL_PROTOCOL_PAYLOAD_MAX_SIZE + sizeof(uint8_t))
#define WATCHDOG_TIMEOUT_MS    8000
#define HEARTBEAT_INTERVAL_MS  5000
#define POSITION_INTERVAL_MS   15000   // own position broadcast over ERL (compass rides along, see erl_compass_t)
#define COMPASS_POLL_INTERVAL_MS 200   // how often the QMC5883 is polled for a fresh reading
#define LOOP_DELAY_MS          10
#define PERIOD_JITTER_MS       500     // heartbeat / position periods vary by +- this, at random
#define TX_BACKOFF_MIN_MS      20      // channel busy: wait 20..99 ms before trying again
#define TX_BACKOFF_RANGE_MS    80
#define TX_GAP_RANGE_MS        50      // random 0..49 ms between consecutive frames
#define TX_QUEUE_DEPTH         8
#define RX_QUEUE_DEPTH         8
#define NOISE_SAMPLE_MS        250     // channel RSSI while nothing is on the air: the noise floor
#define NOISE_AVG_WEIGHT       0.05f   // moving average of the noise samples (~20 samples, 5 s)

#define RADIO_FREQUENCY_MHZ    433.0
#define RADIO_BANDWIDTH_KHZ    500.0
#define RADIO_SPREADING_FACTOR 11
#define RADIO_CODING_RATE      5
#define RADIO_OUTPUT_POWER_DBM 17
#define RADIO_PREAMBLE_LENGTH  8

#endif //LINK_ROUTER_MAIN_H