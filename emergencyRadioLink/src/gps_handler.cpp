#include "gps_handler.h"

#include <cstring>

#ifdef CONFIG_DEBUG_GPS
#include <cstdio>
#define GPS_LOG(...) printf(__VA_ARGS__)
#else
#define GPS_LOG(...)
#endif


static gps_position_t g_position;
static bool g_have_position;

void gps_handler_init(void) {
    g_have_position = false;
    memset(&g_position, 0, sizeof(g_position));
}

bool gps_handler_get_position(gps_position_t* out) {
    if (!g_have_position || out == NULL)
        return false;

    *out = g_position;
    return true;
}

bool gps_handler_set_position(const int32_t longitude, const int32_t latitude) {
    g_position.time = 0;
    g_position.longitude = longitude;
    g_position.latitude = latitude;
    g_have_position = true;

    GPS_LOG("[GPS] position set manually: lon=%ld lat=%ld (microdegrees)\r\n",
            static_cast<long>(longitude), static_cast<long>(latitude));
    return true;
}

#ifdef FEATURE_GPS

#include <cmath>

#include "ublox_gps.h"
#include "uart.h"

static Ublox g_gps{};

void gps_handler_service(void) {
    uint8_t chunk[64];
    size_t count;

    while ((count = uart_read(UART_GPS, chunk, sizeof(chunk))) > 0) {
#ifdef CONFIG_DEBUG_GPS
        // echo the raw NMEA stream exactly as received from the module
        fwrite(chunk, 1, count, stdout);
#endif
        for (size_t i = 0; i < count; ++i) {
            if (!g_gps.encode(static_cast<char>(chunk[i])))
                continue;
            if (!g_gps.datetime.valid) {
                GPS_LOG("[GPS] sentence parsed, no active fix (fixtype=%d sats_in_use=%d)\r\n",
                        static_cast<int>(g_gps.fixtype), g_gps.sats_in_use);
                continue;
            }

            g_position.time = static_cast<uint32_t>(g_gps.datetime.hours) * 3600000u +
                              static_cast<uint32_t>(g_gps.datetime.minutes) * 60000u +
                              static_cast<uint32_t>(g_gps.datetime.seconds) * 1000u +
                              g_gps.datetime.millis;
            g_position.longitude = static_cast<int32_t>(std::lroundf(g_gps.longitude * 1000000.0f));
            g_position.latitude = static_cast<int32_t>(std::lroundf(g_gps.latitude * 1000000.0f));
            g_have_position = true;

            GPS_LOG("[GPS] fix: lon=%.6f lat=%.6f time=%02u:%02u:%02u.%03u fixtype=%d sats_in_use=%d\r\n",
                    g_gps.longitude, g_gps.latitude, g_gps.datetime.hours, g_gps.datetime.minutes,
                    g_gps.datetime.seconds, g_gps.datetime.millis, static_cast<int>(g_gps.fixtype),
                    g_gps.sats_in_use);
        }
    }
}

bool gps_handler_get_status(gps_status_t* out) {
    if (out == NULL)
        return false;

    out->fix = g_gps.fixtype > 0 ? static_cast<uint8_t>(g_gps.fixtype) : 0;
    out->satellites_used = g_gps.sats_in_use > 0 ? static_cast<uint8_t>(g_gps.sats_in_use) : 0;
    out->satellites_in_view = g_gps.sats_in_view > 0 ? static_cast<uint8_t>(g_gps.sats_in_view) : 0;
    const float hdop = g_gps.hdop * 100.0f;
    out->hdop_x100 = hdop > 0.0f && hdop < 65535.0f ? static_cast<uint16_t>(std::lroundf(hdop)) : 0;
    return true;
}

#else // !FEATURE_GPS

// No GPS hardware support compiled in (FEATURE_GPS missing from CONFIG_DEVICE_FEATURES in
// CMakePresets.json): nothing to poll, but gps_handler_get/set_position() above still work,
// so a position set manually over MERL is still cached and broadcast normally.
void gps_handler_service(void) {}

bool gps_handler_get_status(gps_status_t* out) {
    (void)out;
    return false;
}

#endif // FEATURE_GPS
