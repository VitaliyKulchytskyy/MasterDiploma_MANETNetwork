// merld: MERL master on the main unit (Luckfox Lyra).
//
// Owns the UART to the emergency unit (RP2040) and is the only process talking to it:
// - sends one request at a time (the emergency unit answers every request, in order),
// - polls network params, position and the peer table every poll interval,
// - reads the unit's firmware version and compiled-in features once per link-up,
// - reads the compass every second when the firmware has one (heading for the web UI),
// - stands in the user's phone GPS / compass (sent by the app) for a unit built without them,
// - keeps the chat: user messages the emergency unit forwards from ERL (MERL_CMD_USER_MESSAGE_SEND)
//   and the ones sent from here, in SQLite (chat.c),
// - publishes its view as JSON in the state file for the web UI,
// - takes commands from merlctl over a Unix socket (one line in, one JSON line out).
//
// Settings come from the environment (/etc/default/merld via systemd), options override them.

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "merl.h"

#include "buf.h"
#include "alert.h"
#include "chat.h"

#define ANSWER_TIMEOUT_MS       500     // the emergency unit answers within a few ms; allow for its main loop
#define LINK_DOWN_AFTER         3       // timeouts in a row before the link is reported down
#define TTY_RETRY_MS            5000
#define QUEUE_SIZE              16
#define MAX_CLIENTS             8
#define CLIENT_LINE_MAX         512
#define CHAT_LIST_DEFAULT       50      // messages per "chat list"
#define CHAT_LIST_MAX           200
#define MERL_PEER_SIZE_V1       20      // peer entry of firmware before the compass fields
#define MAX_PEERS               (MERL_PROTOCOL_PAYLOAD_MAX_SIZE / MERL_PEER_SIZE_V1)
#define COMPASS_POLL_MS         1000    // a heading shown live wants more than the 5 s poll
#define LINK_STATS_POLL_MS      1000    // radio and per-peer link statistics (MERL_FEATURE_LINK_STATS):
                                        // link experiments sample them every second
#define PHONE_HEADING_MAX_AGE_S 15      // the app sends it every second while it is open
#define PHONE_FORWARD_EVERY_S   5       // the phone's position / heading to the unit, at most this often
#define PHONE_FORWARD_TURN_DEG  10.0    // ... or sooner, for a heading turned by this much

// What merld knows about a piece of the unit's firmware info
enum info {
    INFO_UNKNOWN,       // not read yet (or the link went down since: it may have been reflashed)
    INFO_KNOWN,
    INFO_UNSUPPORTED,   // older firmware without the command
};

struct job {
    uint8_t frame[MERL_PROTOCOL_FRAME_MAX_SIZE];
    size_t size;
    merl_command_t command;
    int client;                 // index into g.clients waiting for the answer, -1 for an internal poll
    int64_t message_id;         // chat row of an outgoing message, -1 otherwise
};

struct client {
    int fd;
    char line[CLIENT_LINE_MAX];
    size_t length;
    bool waiting;               // a queued job will answer it
};

static struct {
    // settings
    const char* tty_path;
    unsigned baudrate;
    const char* socket_path;
    const char* state_path;
    const char* chat_path;
    unsigned poll_ms;
    bool verbose;

    // link
    int tty_fd;
    int64_t tty_retry_ms;
    merl_parser_t parser;
    int64_t rx_last_ms;

    // request queue, the head is in flight
    struct job queue[QUEUE_SIZE];
    size_t queue_head;
    size_t queue_count;
    bool in_flight;
    int64_t deadline_ms;
    int64_t next_poll_ms;

    // control socket
    int listen_fd;
    struct client clients[MAX_CLIENTS];

    // what we know about the emergency unit
    bool link_up;
    unsigned timeouts_in_row;
    unsigned long timeouts;
    time_t last_answer;
    bool have_network;
    merl_network_params_t network;
    bool have_position;             // the position was asked for at least once
    bool position_fix;              // false: the unit has no position (answered ERROR)
    merl_position_t position;
    bool have_peers;
    merl_peer_t peers[MAX_PEERS];
    size_t peers_count;
    // firmware: only changes with a reflash, so read again only after the link was down
    enum info version_info;
    merl_device_version_t version;
    enum info features_info;
    merl_feature_t features[MERL_PROTOCOL_PAYLOAD_MAX_SIZE];
    size_t features_count;
    uint32_t unit_uptime;           // the unit's own peer entry: going down means it rebooted
    // GPS receiver status (MERL_FEATURE_GPS): satellites, fix; read with the 5 s poll
    enum info gps_info;             // INFO_UNSUPPORTED: firmware before MERL_CMD_GPS_STATUS
    merl_gps_status_t gps;
    time_t gps_time;
    // compass (MERL_FEATURE_COMPASS): raw QMC5883 axes and the heading derived from them
    bool have_compass;              // a reading came (the unit answers ERROR until it has one)
    merl_compass_t compass;
    time_t compass_time;
    int64_t next_compass_ms;
    // radio and link statistics (MERL_FEATURE_LINK_STATS): counters since the unit's boot
    bool have_radio_stats;
    merl_radio_stats_t radio_stats;
    bool have_links;
    merl_link_stats_t links[MAX_PEERS];
    size_t links_count;
    time_t link_stats_time;
    int64_t next_link_stats_ms;
    // heading = atan2(y - offset_y, x - offset_x) + declination + rotation, see heading_deg()
    double compass_offset_x;        // hard-iron offsets: (max + min) / 2 of each axis turned round
    double compass_offset_y;
    double compass_declination;     // local magnetic declination, degrees, east positive
    double compass_rotation;        // how the module is mounted relative to the unit's front, degrees
    // INT_ALERT_PIN to the unit: high for alert_pulse_ms on every received SOS (alert.c)
    const char* alert_line;         // its device tree name; "none": not used
    unsigned alert_pulse_ms;
    int64_t alert_until_ms;         // 0: low
    time_t alert_last;              // last raised
    unsigned long alerts;
    // the phone's GPS / compass (merlctl phone ..., from the app), used only for a feature the
    // unit's firmware lacks
    bool have_phone_position;
    merl_position_t phone_position; // time unused (0)
    double phone_accuracy_m;
    long phone_fix_age_s;           // how old the phone's fix was when it came
    char phone_gps_state[16];       // the app's own: no-permission, location-off, waiting, ok
    time_t phone_gps_state_time;
    int phone_satellites_used;      // the phone's GNSS receiver, -1: not sent (older app)
    int phone_satellites_in_view;
    time_t phone_position_time;
    double phone_heading;           // degrees from true north
    time_t phone_heading_time;
    // what was last handed on to the unit (POSITION_SET / COMPASS_SET), so its peers get it too
    time_t phone_position_forwarded;
    bool position_from_phone;       // the unit's set position (time 0) may be replaced by the phone's:
                                    // not one the user set in this run (after a restart: unknown, so yes)
    time_t phone_heading_forwarded;
    double phone_heading_sent;
} g = {
    .position_from_phone = true,
    .tty_fd = -1,
    .listen_fd = -1,
};

static volatile sig_atomic_t g_stop;

static void log_msg(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---- JSON views of the state ------------------------------------------------------------------

static void json_coordinate(struct buf* b, const char* name, int32_t microdegrees) {
    buf_printf(b, "\"%s\":%s%d.%06d", name, microdegrees < 0 ? "-" : "",
               abs(microdegrees / 1000000), abs(microdegrees % 1000000));
}

static void json_network(struct buf* b) {
    if (!g.have_network) {
        buf_printf(b, "null");
        return;
    }
    buf_printf(b, "{\"address\":\"0x%04X\",\"subnet_mask\":\"0x%04X\"}", g.network.address, g.network.subnet_mask);
}

// The unit's firmware is known to be built without a feature (so the phone may stand in)
static bool lacks_feature(merl_feature_t feature);

// Whether the phone's GPS stands in for the unit's: a unit built without GPS, or one with GPS
// but no fix yet (answers ERROR, or still holds the phone's position we set: time 0). A GPS fix
// overwrites a set position in the firmware, so the unit's own GPS takes over once it has one;
// a position the user set (merlctl position set) is left alone.
static bool phone_gps_wanted(void) {
    if (lacks_feature(MERL_FEATURE_GPS))
        return true;
    if (g.features_info != INFO_KNOWN || !g.have_position)
        return false;
    return !g.position_fix || (g.position.time == 0 && g.position_from_phone);
}

static void json_position(struct buf* b) {
    // the phone's position, if the app sends one, for a unit without GPS or a GPS fix
    if (phone_gps_wanted()) {
        if (!g.have_phone_position) {
            buf_printf(b, "null");
            return;
        }
        buf_printf(b, "{\"source\":\"phone\",\"accuracy_m\":%.0f,\"age_s\":%lld,", g.phone_accuracy_m,
                   (long long)(time(NULL) - g.phone_position_time + g.phone_fix_age_s));
        json_coordinate(b, "longitude", g.phone_position.longitude);
        buf_printf(b, ",");
        json_coordinate(b, "latitude", g.phone_position.latitude);
        buf_printf(b, "}");
        return;
    }
    if (!g.have_position || !g.position_fix) {
        buf_printf(b, "null");
        return;
    }
    buf_printf(b, "{\"source\":\"unit\",\"time_ms\":%u,\"manual\":%s,", g.position.time,
               g.position.time == 0 ? "true" : "false");
    json_coordinate(b, "longitude", g.position.longitude);
    buf_printf(b, ",");
    json_coordinate(b, "latitude", g.position.latitude);
    buf_printf(b, "}");
}

static double raw_heading_deg(double x, double y);
static const char* cardinal(double heading);

static void json_peers(struct buf* b) {
    buf_printf(b, "[");
    for (size_t i = 0; i < g.peers_count; ++i) {
        const merl_peer_t* p = &g.peers[i];
        buf_printf(b, "%s{\"address\":\"0x%04X\",\"rssi\":%d,\"uptime\":%u,", i ? "," : "", p->address, p->rssi,
                   p->uptime);
        if (p->longitude == 0 && p->latitude == 0) {
            buf_printf(b, "\"longitude\":null,\"latitude\":null");
        } else {
            json_coordinate(b, "longitude", p->longitude);
            buf_printf(b, ",");
            json_coordinate(b, "latitude", p->latitude);
        }
        buf_printf(b, ",\"config\":%u,\"compass\":", p->config);
        // raw QMC5883 axes the peer sends over ERL with its position (appended to
        // PACKET_TYPE_POSITION, every 15 s, only while it knows its position), all 0 = none yet.
        // Only the declination applies: the offsets / mounting in /etc/default/merld belong to
        // our own module (our own entry is json_compass(), calibrated)
        if (p->compass_x == 0 && p->compass_y == 0 && p->compass_z == 0) {
            buf_printf(b, "null");
        } else {
            const double h = raw_heading_deg(p->compass_x, p->compass_y);
            buf_printf(b, "{\"x\":%d,\"y\":%d,\"z\":%d,\"heading\":%.1f,\"cardinal\":\"%s\",\"calibrated\":false}",
                       p->compass_x, p->compass_y, p->compass_z, h, cardinal(h));
        }
        buf_printf(b, "}");
    }
    buf_printf(b, "]");
}

#define ISO_DATE_SIZE 32      // "yyyy-mm-dd" fits, and the compiler can see any int does

// __DATE__ text ("Sep 27 2026", "Sep  7 2026") -> "2026-09-27", false if it is not one
static bool iso_date(const char* date, char out[ISO_DATE_SIZE]) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* m = strstr(months, (char[4]){date[0], date[1], date[2], '\0'});
    int day, year;
    if (!m || (m - months) % 3 != 0 || sscanf(date + 3, "%d %d", &day, &year) != 2 || day < 1 || day > 31 ||
        year < 0 || year > 9999)
        return false;
    snprintf(out, ISO_DATE_SIZE, "%04d-%02d-%02d", year, (int)(m - months) / 3 + 1, day);
    return true;
}

// Fixed-width text from the wire as a C string: non-printable bytes become '?'
static void wire_text(const char* in, size_t size, char* out) {
    for (size_t i = 0; i < size; ++i)
        out[i] = in[i] >= 0x20 && in[i] < 0x7f ? in[i] : '?';
    out[size] = '\0';
}

// null: not read yet; {"supported":false}: older firmware without MERL_CMD_DEVICE_VERSION
static void json_firmware(struct buf* b) {
    if (g.version_info != INFO_KNOWN) {
        buf_printf(b, g.version_info == INFO_UNSUPPORTED ? "{\"supported\":false}" : "null");
        return;
    }
    char date[MERL_DEVICE_BUILD_DATE_SIZE + 1], time_[MERL_DEVICE_BUILD_TIME_SIZE + 1], iso[ISO_DATE_SIZE];
    wire_text(g.version.build_date, MERL_DEVICE_BUILD_DATE_SIZE, date);
    wire_text(g.version.build_time, MERL_DEVICE_BUILD_TIME_SIZE, time_);
    buf_printf(b, "{\"supported\":true,\"version\":\"%u.%u\",\"major\":%u,\"minor\":%u,\"build\":%u,\"build_date\":",
               g.version.major, g.version.minor, g.version.major, g.version.minor, g.version.build_number);
    buf_json_string(b, (const uint8_t*)date, strlen(date));
    buf_printf(b, ",\"build_time\":");
    buf_json_string(b, (const uint8_t*)time_, strlen(time_));
    if (iso_date(date, iso))
        buf_printf(b, ",\"built\":\"%s %s\"", iso, time_);
    buf_printf(b, "}");
}

// Feature ids this merld does not know yet (newer firmware) are listed with a null name
static void json_features(struct buf* b) {
    if (g.features_info != INFO_KNOWN) {
        buf_printf(b, g.features_info == INFO_UNSUPPORTED ? "{\"supported\":false}" : "null");
        return;
    }
    buf_printf(b, "{\"supported\":true,\"list\":[");
    for (size_t i = 0; i < g.features_count; ++i) {
        const char* name = merl_feature_name(g.features[i]);
        buf_printf(b, "%s{\"id\":%u,\"name\":", i ? "," : "", g.features[i]);
        if (name)
            buf_printf(b, "\"%s\"}", name);
        else
            buf_printf(b, "null}");
    }
    buf_printf(b, "]}");
}

// Link lost: the unit may come back with another firmware
static void forget_firmware(void) {
    g.version_info = INFO_UNKNOWN;
    g.features_info = INFO_UNKNOWN;
    g.gps_info = INFO_UNKNOWN;
    g.have_compass = false;
    g.have_radio_stats = false;
    g.have_links = false;
}

// The firmware has a compass (MERL_CMD_DEVICE_FEATURES lists it)
static bool has_feature(merl_feature_t feature) {
    if (g.features_info != INFO_KNOWN)
        return false;
    for (size_t i = 0; i < g.features_count; ++i)
        if (g.features[i] == feature)
            return true;
    return false;
}

// Known to be missing: the feature list was read and does not have it. Not for firmware
// without the list (older ones had a GPS) or before the list is read.
static bool lacks_feature(merl_feature_t feature) {
    return g.features_info == INFO_KNOWN && !has_feature(feature);
}

// The GPS receiver: the phone's while its position stands in for the unit's (and the app sent
// its satellites), else the unit's; null without one (or not read yet), {"supported":false}
// for a firmware before MERL_CMD_GPS_STATUS
// The unit's radio since its boot (MERL_CMD_RADIO_STATS_GET), null without MERL_FEATURE_LINK_STATS
static void json_radio(struct buf* b) {
    if (!g.have_radio_stats) {
        buf_printf(b, "null");
        return;
    }
    const merl_radio_stats_t* r = &g.radio_stats;
    buf_printf(b, "{\"uptime_ms\":%u,\"rx_ok\":%u,\"rx_crc_error\":%u,\"rx_bad_frame\":%u,\"rx_not_for_us\":%u,"
               "\"rx_queue_drop\":%u,\"tx_ok\":%u,\"tx_error\":%u,\"tx_airtime_ms\":%u,\"lbt_busy\":%u,"
               "\"tx_queue_drop\":%u,\"noise_dbm\":", r->uptime_ms, r->rx_ok, r->rx_crc_error, r->rx_bad_frame,
               r->rx_not_for_us, r->rx_queue_drop, r->tx_ok, r->tx_error, r->tx_airtime_ms, r->lbt_busy,
               r->tx_queue_drop);
    // 0 = no idle sample yet
    if (r->noise_dbm)
        buf_printf(b, "%d,\"noise_min_dbm\":%d,\"noise_avg_dbm\":%d", r->noise_dbm, r->noise_min_dbm, r->noise_avg_dbm);
    else
        buf_printf(b, "null,\"noise_min_dbm\":null,\"noise_avg_dbm\":null");
    buf_printf(b, ",\"updated\":%lld}", (long long)g.link_stats_time);
}

// How each peer's packets arrive (MERL_CMD_LINK_STATS_GET): SNR in dB, the sums for exact means
static void json_links(struct buf* b) {
    if (!g.have_links) {
        buf_printf(b, "null");
        return;
    }
    buf_printf(b, "[");
    for (size_t i = 0; i < g.links_count; ++i) {
        const merl_link_stats_t* l = &g.links[i];
        buf_printf(b, "%s{\"address\":\"0x%04X\",\"rx_packets\":%u,\"hb_received\":%u,\"hb_missed\":%u,"
                   "\"rssi_dbm\":%d,\"snr_db\":%.2f,\"freq_error_hz\":%d,\"age_ms\":%u,\"rssi_sum\":%d,"
                   "\"snr_sum_db\":%.2f}", i ? "," : "", l->address, l->rx_packets, l->hb_received, l->hb_missed,
                   l->rssi_dbm, l->snr_q4 / 4.0, l->freq_error_hz, l->age_ms, l->rssi_sum, l->snr_q4_sum / 4.0);
    }
    buf_printf(b, "]");
}

static void json_gps(struct buf* b) {
    if (phone_gps_wanted() && g.have_phone_position && g.phone_satellites_in_view >= 0) {
        // no satellites in the fix: the phone's position came from Wi-Fi / cell towers
        buf_printf(b, "{\"supported\":true,\"source\":\"phone\",\"fix\":\"%s\",\"satellites_used\":%d,"
                      "\"satellites_in_view\":%d,\"hdop\":null,\"updated\":%lld}",
                   g.phone_satellites_used >= 3 ? "gps" : "none", g.phone_satellites_used,
                   g.phone_satellites_in_view, (long long)g.phone_position_time);
        return;
    }
    if (!has_feature(MERL_FEATURE_GPS) || g.gps_info == INFO_UNKNOWN) {
        buf_printf(b, "null");
        return;
    }
    if (g.gps_info == INFO_UNSUPPORTED) {
        buf_printf(b, "{\"supported\":false}");
        return;
    }
    static const char* const fixes[] = {"none", "gps", "dgps"};
    buf_printf(b, "{\"supported\":true,\"source\":\"unit\",\"fix\":\"%s\",\"satellites_used\":%u,\"satellites_in_view\":%u,\"hdop\":",
               g.gps.fix < 3 ? fixes[g.gps.fix] : "other", g.gps.satellites_used, g.gps.satellites_in_view);
    if (g.gps.hdop_x100)
        buf_printf(b, "%.2f", g.gps.hdop_x100 / 100.0);
    else
        buf_printf(b, "null");
    buf_printf(b, ",\"updated\":%lld}", (long long)g.gps_time);
}

static bool has_compass(void) {
    return has_feature(MERL_FEATURE_COMPASS);
}

// Heading in degrees 0..360 (0 = north, 90 = east) from the raw QMC5883 axes: the horizontal
// field direction, corrected by the hard-iron offsets, the local declination (magnetic -> true
// north) and the module's mounting rotation. The module is assumed level.
static double heading_deg(const merl_compass_t* c) {
    double h = atan2((double)c->y - g.compass_offset_y, (double)c->x - g.compass_offset_x) * 180.0 / M_PI;
    h += g.compass_declination + g.compass_rotation;
    h = fmod(h, 360.0);
    return h < 0 ? h + 360.0 : h;
}

// A peer's heading from its raw axes: no offsets or mounting correction known, only the local
// declination (magnetic -> true north)
static double raw_heading_deg(double x, double y) {
    double h = atan2(y, x) * 180.0 / M_PI + g.compass_declination;
    h = fmod(h, 360.0);
    return h < 0 ? h + 360.0 : h;
}

static const char* cardinal(double heading) {
    static const char* const names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return names[(int)((heading + 22.5) / 45.0) % 8];
}

// null: no compass in this firmware (or no reading yet)
static void json_compass(struct buf* b) {
    // a unit without a compass: the phone's heading while the app keeps sending it
    if (lacks_feature(MERL_FEATURE_COMPASS)) {
        if (!g.phone_heading_time || time(NULL) - g.phone_heading_time > PHONE_HEADING_MAX_AGE_S) {
            buf_printf(b, "null");
            return;
        }
        buf_printf(b, "{\"source\":\"phone\",\"heading\":%.1f,\"cardinal\":\"%s\",\"updated\":%lld}",
                   g.phone_heading, cardinal(g.phone_heading), (long long)g.phone_heading_time);
        return;
    }
    if (!g.have_compass) {
        buf_printf(b, "null");
        return;
    }
    const double h = heading_deg(&g.compass);
    buf_printf(b, "{\"source\":\"unit\",\"x\":%d,\"y\":%d,\"z\":%d,\"heading\":%.1f,\"cardinal\":\"%s\",\"updated\":%lld}",
               g.compass.x, g.compass.y, g.compass.z, h, cardinal(h), (long long)g.compass_time);
}

static void json_state(struct buf* b) {
    const char* link = g.tty_fd < 0 ? "no-tty" : (g.last_answer == 0 && g.timeouts_in_row < LINK_DOWN_AFTER)
                                                     ? "unknown"
                                                     : (g.link_up ? "up" : "down");
    buf_printf(b, "{\"tty\":");
    buf_json_string(b, (const uint8_t*)g.tty_path, strlen(g.tty_path));
    buf_printf(b, ",\"link\":\"%s\",\"last_answer\":", link);
    if (g.last_answer)
        buf_printf(b, "%lld", (long long)g.last_answer);
    else
        buf_printf(b, "null");
    buf_printf(b, ",\"timeouts\":%lu,\"network\":", g.timeouts);
    json_network(b);
    buf_printf(b, ",\"position\":");
    json_position(b);
    buf_printf(b, ",\"peers\":");
    if (g.have_peers)
        json_peers(b);
    else
        buf_printf(b, "null");
    buf_printf(b, ",\"firmware\":");
    json_firmware(b);
    buf_printf(b, ",\"features\":");
    json_features(b);
    buf_printf(b, ",\"compass\":");
    json_compass(b);
    buf_printf(b, ",\"gps\":");
    json_gps(b);
    buf_printf(b, ",\"radio\":");
    json_radio(b);
    buf_printf(b, ",\"links\":");
    json_links(b);
    buf_printf(b, ",\"alert\":{\"line\":");
    buf_json_string(b, (const uint8_t*)g.alert_line, strlen(g.alert_line));
    buf_printf(b, ",\"available\":%s,\"active\":%s,\"count\":%lu,\"last\":", alert_available() ? "true" : "false",
               g.alert_until_ms ? "true" : "false", g.alerts);
    if (g.alert_last)
        buf_printf(b, "%lld}", (long long)g.alert_last);
    else
        buf_printf(b, "null}");
    // which of the phone's sensors merld wants now (the app turns them on and off by this)
    buf_printf(b, ",\"phone\":{\"gps\":%s,\"compass\":%s,\"gps_state\":", phone_gps_wanted() ? "true" : "false",
               lacks_feature(MERL_FEATURE_COMPASS) ? "true" : "false");
    // the app's last word on its location (every 10 s while it is open): older is stale
    if (g.phone_gps_state[0] && time(NULL) - g.phone_gps_state_time <= 30)
        buf_printf(b, "\"%s\"}", g.phone_gps_state);
    else
        buf_printf(b, "null}");
    buf_printf(b, ",\"messages\":%ld,\"chat_last_id\":%lld,\"updated\":%lld}", chat_count(),
               (long long)chat_last_id(), (long long)time(NULL));
}

static void write_file_atomic(const char* path, const char* data, size_t length) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) {
        log_msg("cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    fwrite(data, 1, length, f);
    if (fclose(f) != 0 || rename(tmp, path) != 0)
        log_msg("cannot update %s: %s", path, strerror(errno));
}

static void publish_state(void) {
    struct buf b = {0};
    json_state(&b);
    buf_printf(&b, "\n");
    if (b.data)
        write_file_atomic(g.state_path, b.data, b.length);
    buf_free(&b);
}

// ---- UART ---------------------------------------------------------------------------------

static speed_t baud_constant(unsigned baudrate) {
    switch (baudrate) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default: return 0;
    }
}

static void tty_close(const char* why) {
    if (g.tty_fd < 0)
        return;
    log_msg("closing %s: %s", g.tty_path, why);
    close(g.tty_fd);
    g.tty_fd = -1;
    g.link_up = false;
    forget_firmware();
    g.tty_retry_ms = now_ms() + TTY_RETRY_MS;
    publish_state();
}

static void tty_open(void) {
    const int fd = open(g.tty_path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        g.tty_retry_ms = now_ms() + TTY_RETRY_MS;
        return;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        log_msg("%s is not a tty: %s", g.tty_path, strerror(errno));
        close(fd);
        g.tty_retry_ms = now_ms() + TTY_RETRY_MS;
        return;
    }
    cfmakeraw(&tio);
    cfsetispeed(&tio, baud_constant(g.baudrate));
    cfsetospeed(&tio, baud_constant(g.baudrate));
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(CRTSCTS | CSTOPB | PARENB);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &tio);
    tcflush(fd, TCIOFLUSH);

    g.tty_fd = fd;
    merl_parser_reset(&g.parser);
    log_msg("opened %s at %u baud", g.tty_path, g.baudrate);
    publish_state();
}

static void dump_frame(const char* what, const uint8_t* frame, size_t size) {
    if (!g.verbose)
        return;
    char hex[MERL_PROTOCOL_FRAME_MAX_SIZE * 3 + 1];
    size_t n = 0;
    for (size_t i = 0; i < size; ++i)
        n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%02x ", frame[i]);
    log_msg("%s %s", what, hex);
}

static bool tty_write(const uint8_t* data, size_t size) {
    if (g.tty_fd < 0)
        return false;
    dump_frame("tx", data, size);
    while (size > 0) {
        const ssize_t n = write(g.tty_fd, data, size);
        if (n > 0) {
            data += n;
            size -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EAGAIN) {
            struct pollfd pfd = {.fd = g.tty_fd, .events = POLLOUT};
            if (poll(&pfd, 1, 100) > 0)
                continue;
        }
        tty_close(n < 0 ? strerror(errno) : "write failed");
        return false;
    }
    return true;
}

static void send_status(merl_command_t status, merl_command_t request_command) {
    uint8_t frame[MERL_PROTOCOL_FRAME_MAX_SIZE];
    const size_t size = merl_status_encode(frame, sizeof(frame), status, request_command);
    tty_write(frame, size);
}

// ---- control clients ----------------------------------------------------------------------

static void client_close(int idx) {
    if (g.clients[idx].fd >= 0)
        close(g.clients[idx].fd);
    g.clients[idx] = (struct client){.fd = -1};

    // a queued job for it just has nobody to answer
    for (size_t i = 0; i < g.queue_count; ++i) {
        struct job* job = &g.queue[(g.queue_head + i) % QUEUE_SIZE];
        if (job->client == idx)
            job->client = -1;
    }
}

static void client_reply(int idx, struct buf* b) {
    buf_printf(b, "\n");
    if (b->data && g.clients[idx].fd >= 0)
        send(g.clients[idx].fd, b->data, b->length, MSG_NOSIGNAL);
    client_close(idx);
}

static void client_error(int idx, const char* fmt, ...) {
    char text[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    struct buf b = {0};
    buf_printf(&b, "{\"ok\":false,\"error\":");
    buf_json_string(&b, (const uint8_t*)text, strlen(text));
    buf_printf(&b, "}");
    client_reply(idx, &b);
    buf_free(&b);
}

// ---- chat ---------------------------------------------------------------------------------

// Subnet broadcast: all host bits set. Without the network params only our own address is known.
static bool is_broadcast(uint16_t receiver) {
    if (!g.have_network)
        return false;
    const uint16_t host_mask = (uint16_t)~g.network.subnet_mask;
    return host_mask != 0 && (receiver & host_mask) == host_mask;
}

// An SOS: the text starts with "sos" in any case, after leading blanks (the web UI's and the
// app's rule too)
static bool is_sos(const uint8_t* text, size_t length) {
    size_t i = 0;
    while (i < length && (text[i] == ' ' || text[i] == '\t'))
        ++i;
    static const char word[] = "sos";
    for (size_t k = 0; k < sizeof(word) - 1; ++k, ++i)
        if (i >= length || tolower(text[i]) != word[k])
            return false;
    return true;
}

// INT_ALERT_PIN high for alert_pulse_ms (a new SOS while it is high starts the pulse again); the
// unit keeps its alert for its CONFIG_ALERT_DURATION_MS after the pin goes low
static void alert_raise(const char* why) {
    if (!alert_available())
        return;
    alert_set(true);
    g.alert_until_ms = now_ms() + g.alert_pulse_ms;
    g.alert_last = time(NULL);
    ++g.alerts;
    log_msg("alert: INT_ALERT_PIN high for %u ms (%s)", g.alert_pulse_ms, why);
    publish_state();
}

static void alert_service(int64_t now) {
    if (g.alert_until_ms && now >= g.alert_until_ms) {
        alert_set(false);
        g.alert_until_ms = 0;
        publish_state();
    }
}

// A user message the emergency unit received over ERL: address = receiver << 16 | sender
static void chat_received(const merl_frame_t* frame) {
    uint32_t address;
    const uint8_t* text;
    uint8_t length;
    if (!merl_user_message_decode(frame->payload, frame->length, &address, &text, &length)) {
        log_msg("malformed user message (%u bytes)", frame->length);
        send_status(MERL_CMD_RESPONSE_ERROR, frame->command);
        return;
    }
    const uint16_t sender = (uint16_t)address, receiver = (uint16_t)(address >> 16);
    const bool broadcast = is_broadcast(receiver);
    send_status(chat_add("in", sender, receiver, broadcast, text, length, "received") >= 0
                    ? MERL_CMD_RESPONSE_OK : MERL_CMD_RESPONSE_OVERLOAD,
                frame->command);
    log_msg("chat in 0x%04X -> 0x%04X%s: %.*s", sender, receiver, broadcast ? " (broadcast)" : "", length,
            (const char*)text);
    if (is_sos(text, length))
        alert_raise("SOS received");
    publish_state();
}

// ---- request queue ------------------------------------------------------------------------

static bool enqueue(merl_command_t command, const uint8_t* payload, uint8_t length, int client) {
    if (g.queue_count == QUEUE_SIZE)
        return false;
    struct job* job = &g.queue[(g.queue_head + g.queue_count) % QUEUE_SIZE];
    job->size = merl_frame_encode(job->frame, sizeof(job->frame), MERL_PROTOCOL_DIRECTION_REQUEST, command, payload,
                                  length);
    if (job->size == 0)
        return false;
    job->command = command;
    job->client = client;
    job->message_id = -1;
    ++g.queue_count;
    if (client >= 0)
        g.clients[client].waiting = true;
    return true;
}

static bool internal_job_queued(merl_command_t command) {
    for (size_t i = 0; i < g.queue_count; ++i) {
        const struct job* job = &g.queue[(g.queue_head + i) % QUEUE_SIZE];
        if (job->client < 0 && job->command == command)
            return true;
    }
    return false;
}

static void poll_unit(merl_command_t command) {
    if (!internal_job_queued(command))
        enqueue(command, NULL, 0, -1);
}

static void send_next(void) {
    if (g.in_flight || g.queue_count == 0 || g.tty_fd < 0)
        return;
    struct job* job = &g.queue[g.queue_head];
    if (!tty_write(job->frame, job->size))
        return;
    g.in_flight = true;
    g.deadline_ms = now_ms() + ANSWER_TIMEOUT_MS;
}

static bool answer_ok(const merl_frame_t* answer) {
    return answer->command == MERL_CMD_RESPONSE_OK || answer->command == MERL_CMD_RESPONSE_OK_BODY;
}

// Update what we know from an answer, whoever asked
static void apply_answer(const struct job* job, const merl_frame_t* answer) {
    const bool body = answer->command == MERL_CMD_RESPONSE_OK_BODY;

    switch (job->command) {
        case MERL_CMD_NETWORK_GET:
            if (body && answer->length == MERL_NETWORK_PARAMS_SIZE) {
                merl_network_params_decode(answer->payload, &g.network);
                g.have_network = true;
            }
            break;
        case MERL_CMD_POSITION_GET:
            if (body && answer->length == MERL_POSITION_SIZE) {
                merl_position_decode(answer->payload, &g.position);
                g.have_position = g.position_fix = true;
            } else if (answer->command == MERL_CMD_RESPONSE_ERROR) {
                g.have_position = true;     // no fix and no manual position yet
                g.position_fix = false;
            }
            break;
        case MERL_CMD_GPS_STATUS:
            if (body && answer->length == MERL_GPS_STATUS_SIZE) {
                merl_gps_status_decode(answer->payload, &g.gps);
                g.gps_info = INFO_KNOWN;
                g.gps_time = time(NULL);
            } else if (answer->command == MERL_CMD_RESPONSE_UNKNOWN ||
                       answer->command == MERL_CMD_RESPONSE_UNIMPLEMENTED) {
                g.gps_info = INFO_UNSUPPORTED;
            }
            break;
        case MERL_CMD_COMPASS_GET:
            if (body && answer->length == MERL_COMPASS_SIZE) {
                merl_compass_decode(answer->payload, &g.compass);
                g.have_compass = true;
                g.compass_time = time(NULL);
            } else {
                g.have_compass = false;     // ERROR: no reading yet
            }
            break;
        case MERL_CMD_COMPASS_SET:
            if (answer_ok(answer))
                poll_unit(MERL_CMD_COMPASS_GET);
            break;
        case MERL_CMD_RADIO_STATS_GET:
            if (body && answer->length == MERL_RADIO_STATS_SIZE) {
                merl_radio_stats_decode(answer->payload, &g.radio_stats);
                g.have_radio_stats = true;
                g.link_stats_time = time(NULL);
            }
            break;
        case MERL_CMD_LINK_STATS_GET:
            if (body && answer->length % MERL_LINK_STATS_SIZE == 0) {
                g.links_count = answer->length / MERL_LINK_STATS_SIZE;
                if (g.links_count > MAX_PEERS)
                    g.links_count = MAX_PEERS;
                for (size_t i = 0; i < g.links_count; ++i)
                    merl_link_stats_decode(&answer->payload[i * MERL_LINK_STATS_SIZE], &g.links[i]);
                g.have_links = true;
                g.link_stats_time = time(NULL);
            }
            break;
        case MERL_CMD_PEERS_GET: {
            // 26-byte entries (with the compass axes); firmware before them sent 20-byte ones.
            // No length up to 255 is a multiple of both, so the size tells the format.
            const size_t size = answer->length % MERL_PEER_SIZE == 0     ? MERL_PEER_SIZE
                                : answer->length % MERL_PEER_SIZE_V1 == 0 ? MERL_PEER_SIZE_V1
                                                                          : 0;
            if (body && size) {
                g.peers_count = answer->length / size;
                for (size_t i = 0; i < g.peers_count; ++i) {
                    uint8_t entry[MERL_PEER_SIZE] = {0};    // a 20-byte entry: no compass (0 = none)
                    memcpy(entry, &answer->payload[i * size], size);
                    merl_peer_decode(entry, &g.peers[i]);
                    // a reboot (e.g. a reflash) quicker than LINK_DOWN_AFTER timeouts: read the
                    // firmware info again
                    if (g.have_network && g.peers[i].address == g.network.address) {
                        if (g.peers[i].uptime < g.unit_uptime)
                            forget_firmware();
                        g.unit_uptime = g.peers[i].uptime;
                    }
                }
                g.have_peers = true;
            }
            break;
        }
        case MERL_CMD_DEVICE_VERSION:
            if (body && answer->length == MERL_DEVICE_VERSION_SIZE) {
                merl_device_version_decode(answer->payload, &g.version);
                g.version_info = INFO_KNOWN;
            } else if (answer->command == MERL_CMD_RESPONSE_UNKNOWN ||
                       answer->command == MERL_CMD_RESPONSE_UNIMPLEMENTED) {
                g.version_info = INFO_UNSUPPORTED;      // older firmware: do not ask on every poll
            }
            break;
        case MERL_CMD_DEVICE_FEATURES:
            // no features compiled in: an empty list
            if (answer_ok(answer)) {
                g.features_count = answer->length;
                memcpy(g.features, answer->payload, answer->length);
                g.features_info = INFO_KNOWN;
            } else if (answer->command == MERL_CMD_RESPONSE_UNKNOWN ||
                       answer->command == MERL_CMD_RESPONSE_UNIMPLEMENTED) {
                g.features_info = INFO_UNSUPPORTED;
            }
            break;
        // after a change, read it back
        case MERL_CMD_NETWORK_SET:
            if (answer_ok(answer))
                poll_unit(MERL_CMD_NETWORK_GET);
            break;
        case MERL_CMD_POSITION_SET:
            if (answer_ok(answer))
                poll_unit(MERL_CMD_POSITION_GET);
            break;
        case MERL_CMD_PEERS_CLEAR:
            if (answer_ok(answer))
                poll_unit(MERL_CMD_PEERS_GET);
            break;
        default:
            break;
    }
}

static void reply_answer(int idx, const struct job* job, const merl_frame_t* answer) {
    struct buf b = {0};
    const char* name = merl_command_name(answer->command);
    buf_printf(&b, "{\"ok\":%s,\"request\":\"%s\",\"answer\":\"", answer_ok(answer) ? "true" : "false",
               merl_command_name(job->command) ? merl_command_name(job->command) : "RAW");
    if (name)
        buf_printf(&b, "%s", name);
    else
        buf_printf(&b, "0x%04X", answer->command);
    if (job->message_id >= 0)
        buf_printf(&b, "\",\"message_id\":%lld,\"payload\":\"", (long long)job->message_id);
    else
        buf_printf(&b, "\",\"payload\":\"");
    for (size_t i = 0; i < answer->length; ++i)
        buf_printf(&b, "%02x", answer->payload[i]);
    buf_printf(&b, "\"");

    if (answer->command == MERL_CMD_RESPONSE_OK_BODY) {
        switch (job->command) {
            case MERL_CMD_NETWORK_GET:
                buf_printf(&b, ",\"network\":");
                json_network(&b);
                break;
            case MERL_CMD_POSITION_GET:
                buf_printf(&b, ",\"position\":");
                json_position(&b);
                break;
            case MERL_CMD_PEERS_GET:
                buf_printf(&b, ",\"peers\":");
                json_peers(&b);
                break;
            case MERL_CMD_COMPASS_GET:
                buf_printf(&b, ",\"compass\":");
                json_compass(&b);
                break;
            case MERL_CMD_GPS_STATUS:
                buf_printf(&b, ",\"gps\":");
                json_gps(&b);
                break;
            case MERL_CMD_RADIO_STATS_GET:
                buf_printf(&b, ",\"radio\":");
                json_radio(&b);
                break;
            case MERL_CMD_LINK_STATS_GET:
                buf_printf(&b, ",\"links\":");
                json_links(&b);
                break;
            default:
                break;
        }
    }
    if (answer_ok(answer) && job->command == MERL_CMD_DEVICE_VERSION) {
        buf_printf(&b, ",\"firmware\":");
        json_firmware(&b);
    } else if (answer_ok(answer) && job->command == MERL_CMD_DEVICE_FEATURES) {
        buf_printf(&b, ",\"features\":");
        json_features(&b);
    }
    buf_printf(&b, "}");
    client_reply(idx, &b);
    buf_free(&b);
}

// answer == NULL: timed out
static void complete_job(const merl_frame_t* answer) {
    struct job job = g.queue[g.queue_head];
    g.queue_head = (g.queue_head + 1) % QUEUE_SIZE;
    --g.queue_count;
    g.in_flight = false;

    if (answer) {
        g.link_up = true;
        g.timeouts_in_row = 0;
        g.last_answer = time(NULL);
        apply_answer(&job, answer);
        if (job.message_id >= 0) {
            const char* name = merl_command_name(answer->command);
            char status[64];
            snprintf(status, sizeof(status), "failed: %s", name ? name : "unknown answer");
            chat_set_status(job.message_id, answer_ok(answer) ? "sent" : status);
        }
        if (job.client >= 0)
            reply_answer(job.client, &job, answer);
    } else {
        ++g.timeouts;
        if (++g.timeouts_in_row >= LINK_DOWN_AFTER && g.link_up) {
            g.link_up = false;
            forget_firmware();
            log_msg("no answers from the emergency unit, link down");
        }
        chat_set_status(job.message_id, "failed: no answer");
        if (job.client >= 0)
            client_error(job.client, "no answer from the emergency unit");
    }
    publish_state();
}

// ---- frames from the emergency unit -------------------------------------------------------

static void handle_frame(const merl_frame_t* frame) {
    dump_frame("rx", frame->payload - MERL_OFFSET_PAYLOAD, MERL_OFFSET_PAYLOAD + frame->length + 1u);

    if (frame->direction == MERL_PROTOCOL_DIRECTION_REQUEST) {
        // requests the emergency unit sends on its own
        if (frame->command == MERL_CMD_USER_MESSAGE_SEND) {
            chat_received(frame);           // a chat message it received over ERL
        } else if (frame->command == MERL_CMD_USER_MESSAGE_PASSTHROUGH) {
            // reserved for another purpose, not part of the chat
            send_status(MERL_CMD_RESPONSE_OK, frame->command);
            log_msg("passthrough request (%u bytes), not handled", frame->length);
        } else {
            send_status(MERL_CMD_RESPONSE_UNKNOWN, frame->command);
        }
        return;
    }

    if (!g.in_flight) {
        log_msg("unexpected answer 0x%04X, nothing was asked", frame->command);
        return;
    }
    complete_job(frame);
}

static void tty_readable(void) {
    uint8_t chunk[256];
    for (;;) {
        const ssize_t n = read(g.tty_fd, chunk, sizeof(chunk));
        if (n > 0) {
            g.rx_last_ms = now_ms();
            for (ssize_t i = 0; i < n; ++i) {
                merl_frame_t frame;
                if (merl_parser_feed(&g.parser, chunk[i], &frame))
                    handle_frame(&frame);
                if (g.tty_fd < 0)
                    return;
            }
            continue;
        }
        // raw mode with VMIN = 0: read() returns 0, not EAGAIN, once the input is drained.
        // A vanished device shows up as an error or as POLLHUP, handled by the caller.
        if (n == 0 || errno == EAGAIN || errno == EINTR)
            return;
        tty_close(strerror(errno));
        return;
    }
}

// ---- control commands ---------------------------------------------------------------------

static bool parse_u32(const char* text, unsigned long max, unsigned long* out) {
    if (!text || !*text)
        return false;
    char* end;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 0);
    if (errno || *end || value > max)
        return false;
    *out = value;
    return true;
}

static bool parse_degrees(const char* text, double limit, int32_t* out) {
    if (!text || !*text)
        return false;
    char* end;
    errno = 0;
    const double value = strtod(text, &end);
    if (errno || *end || value < -limit || value > limit)
        return false;
    *out = (int32_t)(value * 1e6 + (value < 0 ? -0.5 : 0.5));
    return true;
}

static int hex_decode(const char* text, uint8_t* out, size_t max) {
    const size_t length = strlen(text);
    if (length % 2 || length / 2 > max)
        return -1;
    for (size_t i = 0; i < length / 2; ++i) {
        unsigned value;
        if (sscanf(text + 2 * i, "%2x", &value) != 1)
            return -1;
        out[i] = (uint8_t)value;
    }
    return (int)(length / 2);
}

static void queue_for_client(int idx, merl_command_t command, const uint8_t* payload, uint8_t length) {
    if (g.tty_fd < 0) {
        client_error(idx, "no link: cannot open %s", g.tty_path);
        return;
    }
    if (!enqueue(command, payload, length, idx))
        client_error(idx, "busy, try again");
}

static const char* const USAGE =
    "status | network get | network set <address> <mask> | peers get | peers clear | "
    "position get | position set <longitude> <latitude> | compass get | compass set <x> <y> <z> | "
    "phone position <longitude> <latitude> [accuracy-m] | phone compass <heading> | "
    "version | features | radio stats | links get | chat list [after-id] [limit] | "
    "chat send <address|broadcast> <text> | raw <command> [hex payload]";

// Queue a chat message to an ERL address or the subnet broadcast; stored as pending until answered
static void chat_send(int idx, const char* to, const char* text) {
    unsigned long address;
    if (to && strcmp(to, "broadcast") == 0) {
        if (!g.have_network) {
            client_error(idx, "the emergency unit's network params are not known yet");
            return;
        }
        address = g.network.address | (uint16_t)~g.network.subnet_mask;
    } else if (!parse_u32(to, UINT16_MAX, &address)) {
        client_error(idx, "usage: chat send <address|broadcast> <text>, e.g. chat send broadcast Hi!");
        return;
    }
    const size_t length = text ? strlen(text) : 0;
    if (length == 0) {
        client_error(idx, "empty message");
        return;
    }
    if (length > MERL_USER_MESSAGE_TEXT_MAX_SIZE) {
        client_error(idx, "message too long: %zu bytes, at most %d", length, MERL_USER_MESSAGE_TEXT_MAX_SIZE);
        return;
    }
    if (g.tty_fd < 0) {
        client_error(idx, "no link: cannot open %s", g.tty_path);
        return;
    }

    uint8_t body[MERL_USER_MESSAGE_ADDRESS_SIZE + MERL_USER_MESSAGE_MAX_SIZE];
    const uint8_t size = merl_user_message_encode(body, sizeof(body), (uint32_t)address, (const uint8_t*)text, length);
    const uint16_t sender = g.have_network ? g.network.address : 0;
    const int64_t id = chat_add("out", sender, (uint16_t)address, is_broadcast((uint16_t)address),
                                (const uint8_t*)text, length, "pending");
    if (!enqueue(MERL_CMD_USER_MESSAGE_SEND, body, size, idx)) {
        chat_set_status(id, "failed: busy");
        client_error(idx, "busy, try again");
        return;
    }
    g.queue[(g.queue_head + g.queue_count - 1) % QUEUE_SIZE].message_id = id;
    publish_state();
}

static void chat_list(int idx, const char* after_text, const char* limit_text) {
    unsigned long after = 0, limit = CHAT_LIST_DEFAULT;
    if ((after_text && !parse_u32(after_text, UINT32_MAX, &after)) ||
        (limit_text && (!parse_u32(limit_text, CHAT_LIST_MAX, &limit) || limit == 0))) {
        client_error(idx, "usage: chat list [after-id] [limit <= %d]", CHAT_LIST_MAX);
        return;
    }
    struct buf b = {0};
    buf_printf(&b, "{\"ok\":true,\"last_id\":%lld,\"messages\":", (long long)chat_last_id());
    chat_json(&b, (int64_t)after, (int)limit);
    buf_printf(&b, "}");
    client_reply(idx, &b);
    buf_free(&b);
}

// A unit built without GPS / compass still takes a position / reading over MERL and broadcasts
// it with its own (PACKET_TYPE_POSITION): hand the phone's on to it, so the unit's peers see
// this node on their map too. Rate-limited: the app sends its heading every second.
static void forward_phone_position(void) {
    const time_t now = time(NULL);
    if (!phone_gps_wanted() || internal_job_queued(MERL_CMD_POSITION_SET) ||
        now - g.phone_position_forwarded < PHONE_FORWARD_EVERY_S)
        return;
    const merl_position_set_t position = {
        .longitude = g.phone_position.longitude,
        .latitude = g.phone_position.latitude,
    };
    uint8_t body[MERL_POSITION_SET_SIZE];
    merl_position_set_encode(body, &position);
    if (enqueue(MERL_CMD_POSITION_SET, body, sizeof(body), -1)) {
        g.phone_position_forwarded = now;
        g.position_from_phone = true;
    }
}

static void forward_phone_heading(void) {
    const time_t now = time(NULL);
    double turn = fabs(g.phone_heading - g.phone_heading_sent);
    if (turn > 180)
        turn = 360 - turn;
    if (!lacks_feature(MERL_FEATURE_COMPASS) || internal_job_queued(MERL_CMD_COMPASS_SET) ||
        (now - g.phone_heading_forwarded < PHONE_FORWARD_EVERY_S && turn < PHONE_FORWARD_TURN_DEG))
        return;
    // raw axes that a peer turns back into this heading: it takes atan2(y, x) + its own
    // declination (raw_heading_deg()), about ours, so leave our declination out
    const double a = (g.phone_heading - g.compass_declination) * M_PI / 180.0;
    const merl_compass_t reading = {
        .x = (int16_t)lround(1000.0 * cos(a)),
        .y = (int16_t)lround(1000.0 * sin(a)),
        .z = 0,
    };
    uint8_t body[MERL_COMPASS_SIZE];
    merl_compass_encode(body, &reading);
    if (enqueue(MERL_CMD_COMPASS_SET, body, sizeof(body), -1)) {
        g.phone_heading_forwarded = now;
        g.phone_heading_sent = g.phone_heading;
    }
}

// The phone's GPS / compass from the app (cgi-bin/phone). Kept always, used only while the
// unit's firmware lacks the feature (and then handed on to the unit); the answer says whether
// it is used right now.
static void phone_command(int idx, const char* what, char* args) {
    // "phone gps <state>": why the app has no position (a word, not numbers)
    if (strcmp(what, "gps") == 0) {
        const size_t len = args ? strspn(args, "abcdefghijklmnopqrstuvwxyz-") : 0;
        if (len == 0 || len >= sizeof(g.phone_gps_state) || args[len] != '\0') {
            client_error(idx, "usage: phone gps no-permission|location-off|waiting|ok");
            return;
        }
        memcpy(g.phone_gps_state, args, len + 1);
        g.phone_gps_state_time = time(NULL);
        struct buf b = {0};
        buf_printf(&b, "{\"ok\":true}");
        client_reply(idx, &b);
        buf_free(&b);
        return;
    }
    double v[6] = {0};
    int n = 0;
    for (char* p = args; p && *p && n < 6; ++n) {
        char* end;
        v[n] = strtod(p, &end);
        if (end == p)
            break;
        p = end;
    }
    struct buf b = {0};
    if (strcmp(what, "position") == 0 && n >= 2 && fabs(v[0]) <= 180 && fabs(v[1]) <= 90) {
        g.phone_position.longitude = (int32_t)lround(v[0] * 1e6);
        g.phone_position.latitude = (int32_t)lround(v[1] * 1e6);
        g.phone_accuracy_m = n >= 3 ? v[2] : 0;
        const bool sats = n >= 5 && v[3] >= 0 && v[4] >= v[3] && v[4] < 1000;
        g.phone_satellites_used = sats ? (int)v[3] : -1;
        g.phone_satellites_in_view = sats ? (int)v[4] : -1;
        g.phone_fix_age_s = n >= 6 && v[5] >= 0 ? (long)v[5] : 0;
        g.phone_position_time = time(NULL);
        g.have_phone_position = true;
        forward_phone_position();
        buf_printf(&b, "{\"ok\":true,\"used\":%s}", phone_gps_wanted() ? "true" : "false");
    } else if (strcmp(what, "compass") == 0 && n >= 1 && isfinite(v[0])) {
        g.phone_heading = fmod(fmod(v[0], 360.0) + 360.0, 360.0);
        g.phone_heading_time = time(NULL);
        forward_phone_heading();
        buf_printf(&b, "{\"ok\":true,\"used\":%s}", lacks_feature(MERL_FEATURE_COMPASS) ? "true" : "false");
    } else {
        buf_free(&b);
        client_error(idx, "usage: phone position <longitude> <latitude> [accuracy-m [satellites-used satellites-in-view [fix-age-s]]] | phone compass <heading>");
        return;
    }
    client_reply(idx, &b);
    buf_free(&b);
    publish_state();
}

static void handle_command(int idx, char* line) {
    char* rest = line;
    char* words[4] = {0};
    size_t count = 0;
    // a message text keeps its spaces: "chat send <to> <text>" and "send <to> <text>"; so do the
    // three axes of "compass set <x> <y> <z>"
    while (count < 4 && rest && *rest) {
        while (*rest == ' ')
            ++rest;
        if (!*rest)
            break;
        const bool text_next = (count == 2 && strcmp(words[0], "send") == 0) ||
                               (count == 3 && strcmp(words[0], "chat") == 0 && strcmp(words[1], "send") == 0) ||
                               (count == 2 && strcmp(words[0], "compass") == 0 && strcmp(words[1], "set") == 0) ||
                               (count == 2 && strcmp(words[0], "phone") == 0);
        if (text_next) {
            words[count++] = rest;
            break;
        }
        words[count++] = rest;
        rest = strchr(rest, ' ');
        if (rest)
            *rest++ = '\0';
    }
    const char* cmd = words[0] ? words[0] : "";
    const char* sub = words[1] ? words[1] : "";

    if (strcmp(cmd, "status") == 0) {
        struct buf b = {0};
        buf_printf(&b, "{\"ok\":true,\"state\":");
        json_state(&b);
        buf_printf(&b, "}");
        client_reply(idx, &b);
        buf_free(&b);
    } else if (strcmp(cmd, "chat") == 0 && strcmp(sub, "clear") == 0) {
        const long deleted = chat_clear();
        if (deleted < 0) {
            client_error(idx, "could not clear the chat history");
            return;
        }
        struct buf b = {0};
        buf_printf(&b, "{\"ok\":true,\"deleted\":%ld,\"last_id\":%lld}", deleted, (long long)chat_last_id());
        client_reply(idx, &b);
        buf_free(&b);
        publish_state();
    } else if (strcmp(cmd, "chat") == 0 && strcmp(sub, "list") == 0) {
        chat_list(idx, words[2], words[3]);
    } else if (strcmp(cmd, "chat") == 0 && strcmp(sub, "send") == 0) {
        chat_send(idx, words[2], words[3]);
    } else if (strcmp(cmd, "messages") == 0) {
        chat_list(idx, NULL, NULL);
    } else if (strcmp(cmd, "network") == 0 && strcmp(sub, "get") == 0) {
        queue_for_client(idx, MERL_CMD_NETWORK_GET, NULL, 0);
    } else if (strcmp(cmd, "network") == 0 && strcmp(sub, "set") == 0) {
        unsigned long address, mask;
        if (!parse_u32(words[2], UINT16_MAX, &address) || !parse_u32(words[3], UINT16_MAX, &mask)) {
            client_error(idx, "usage: network set <address> <mask>, e.g. network set 0xC701 0xFFE0");
            return;
        }
        const merl_network_params_t params = {.address = (uint16_t)address, .subnet_mask = (uint16_t)mask};
        uint8_t body[MERL_NETWORK_PARAMS_SIZE];
        merl_network_params_encode(body, &params);
        queue_for_client(idx, MERL_CMD_NETWORK_SET, body, sizeof(body));
    } else if (strcmp(cmd, "peers") == 0 && strcmp(sub, "get") == 0) {
        queue_for_client(idx, MERL_CMD_PEERS_GET, NULL, 0);
    } else if (strcmp(cmd, "peers") == 0 && strcmp(sub, "clear") == 0) {
        queue_for_client(idx, MERL_CMD_PEERS_CLEAR, NULL, 0);
    } else if (strcmp(cmd, "position") == 0 && strcmp(sub, "get") == 0) {
        queue_for_client(idx, MERL_CMD_POSITION_GET, NULL, 0);
    } else if (strcmp(cmd, "position") == 0 && strcmp(sub, "set") == 0) {
        merl_position_set_t position;
        if (!parse_degrees(words[2], 180, &position.longitude) || !parse_degrees(words[3], 90, &position.latitude)) {
            client_error(idx, "usage: position set <longitude> <latitude>, in degrees, e.g. 30.523456 50.450100");
            return;
        }
        uint8_t body[MERL_POSITION_SET_SIZE];
        merl_position_set_encode(body, &position);
        g.position_from_phone = false;      // the user's own: the phone does not replace it
        queue_for_client(idx, MERL_CMD_POSITION_SET, body, sizeof(body));
    } else if (strcmp(cmd, "alert") == 0 && strcmp(sub, "test") == 0) {
        // the wiring to the unit: its LED should go red
        if (!alert_available()) {
            client_error(idx, "no %s line (see merld's log)", g.alert_line);
            return;
        }
        alert_raise("merlctl alert test");
        struct buf b = {0};
        buf_printf(&b, "{\"ok\":true,\"pulse_ms\":%u}", g.alert_pulse_ms);
        client_reply(idx, &b);
        buf_free(&b);
    } else if (strcmp(cmd, "gps") == 0 && strcmp(sub, "status") == 0) {
        queue_for_client(idx, MERL_CMD_GPS_STATUS, NULL, 0);
    } else if (strcmp(cmd, "radio") == 0 && strcmp(sub, "stats") == 0) {
        queue_for_client(idx, MERL_CMD_RADIO_STATS_GET, NULL, 0);
    } else if (strcmp(cmd, "links") == 0 && strcmp(sub, "get") == 0) {
        queue_for_client(idx, MERL_CMD_LINK_STATS_GET, NULL, 0);
    } else if (strcmp(cmd, "compass") == 0 && strcmp(sub, "get") == 0) {
        queue_for_client(idx, MERL_CMD_COMPASS_GET, NULL, 0);
    } else if (strcmp(cmd, "compass") == 0 && strcmp(sub, "set") == 0) {
        // raw axes (tests: "compass set 100 100 0" points north-east with no offsets)
        long axis[3];
        char* rest_axes = words[2];
        bool ok = rest_axes != NULL;
        for (int i = 0; ok && i < 3; ++i) {
            char* end;
            axis[i] = strtol(rest_axes, &end, 0);
            ok = end != rest_axes && axis[i] >= INT16_MIN && axis[i] <= INT16_MAX;
            rest_axes = end;
        }
        if (!ok) {
            client_error(idx, "usage: compass set <x> <y> <z>, raw signed 16-bit axes");
            return;
        }
        const merl_compass_t c = {.x = (int16_t)axis[0], .y = (int16_t)axis[1], .z = (int16_t)axis[2]};
        uint8_t body[MERL_COMPASS_SIZE];
        merl_compass_encode(body, &c);
        queue_for_client(idx, MERL_CMD_COMPASS_SET, body, sizeof(body));
    } else if (strcmp(cmd, "phone") == 0) {
        phone_command(idx, sub, words[2]);
    } else if (strcmp(cmd, "version") == 0) {
        queue_for_client(idx, MERL_CMD_DEVICE_VERSION, NULL, 0);
    } else if (strcmp(cmd, "features") == 0) {
        queue_for_client(idx, MERL_CMD_DEVICE_FEATURES, NULL, 0);
    } else if (strcmp(cmd, "send") == 0) {
        chat_send(idx, words[1], words[2]);
    } else if (strcmp(cmd, "raw") == 0) {
        unsigned long command;
        uint8_t body[MERL_PROTOCOL_PAYLOAD_MAX_SIZE];
        const int length = words[2] ? hex_decode(words[2], body, sizeof(body)) : 0;
        if (!parse_u32(sub, UINT16_MAX, &command) || length < 0) {
            client_error(idx, "usage: raw <command> [hex payload], e.g. raw 0x1000");
            return;
        }
        queue_for_client(idx, (merl_command_t)command, body, (uint8_t)length);
    } else {
        client_error(idx, "unknown command; commands: %s", USAGE);
    }
}

static void client_readable(int idx) {
    struct client* c = &g.clients[idx];
    const ssize_t n = recv(c->fd, c->line + c->length, sizeof(c->line) - 1 - c->length, 0);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EINTR))
            client_close(idx);
        return;
    }
    c->length += (size_t)n;
    c->line[c->length] = '\0';

    char* newline = strchr(c->line, '\n');
    if (!newline) {
        if (c->length == sizeof(c->line) - 1)
            client_error(idx, "command too long");
        return;
    }
    *newline = '\0';
    if (newline > c->line && newline[-1] == '\r')
        newline[-1] = '\0';
    handle_command(idx, c->line);
}

static void accept_client(void) {
    const int fd = accept4(g.listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (g.clients[i].fd < 0) {
            g.clients[i] = (struct client){.fd = fd};
            return;
        }
    }
    const char* busy = "{\"ok\":false,\"error\":\"too many clients\"}\n";
    send(fd, busy, strlen(busy), MSG_NOSIGNAL);
    close(fd);
}

static int listen_socket(const char* path) {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(addr.sun_path, path);
    unlink(path);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// ---- main ---------------------------------------------------------------------------------

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static const char* env_or(const char* name, const char* fallback) {
    const char* value = getenv(name);
    return value && *value ? value : fallback;
}

static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [-d tty] [-b baudrate] [-s socket] [-o state.json] [-c chat.db] [-p poll-ms] [-v]\n"
            "       %s -I [-c chat.db]   create/check the chat database and exit\n"
            "defaults from MERL_TTY, MERL_BAUDRATE, MERL_SOCKET, MERL_STATE, MERL_CHAT, MERL_POLL_MS\n",
            argv0,
            argv0);
}

int main(int argc, char** argv) {
    g.tty_path = env_or("MERL_TTY", "/dev/ttyS3");
    g.baudrate = (unsigned)strtoul(env_or("MERL_BAUDRATE", "115200"), NULL, 10);
    g.socket_path = env_or("MERL_SOCKET", "/run/merl/merld.sock");
    g.state_path = env_or("MERL_STATE", "/run/merl/state.json");
    g.chat_path = env_or("MERL_CHAT", "/var/lib/merl/chat.db");
    g.poll_ms = (unsigned)strtoul(env_or("MERL_POLL_MS", "5000"), NULL, 10);
    g.compass_offset_x = strtod(env_or("MERL_COMPASS_OFFSET_X", "0"), NULL);
    g.compass_offset_y = strtod(env_or("MERL_COMPASS_OFFSET_Y", "0"), NULL);
    g.compass_declination = strtod(env_or("MERL_COMPASS_DECLINATION", "0"), NULL);
    g.compass_rotation = strtod(env_or("MERL_COMPASS_ROTATION", "0"), NULL);
    g.alert_line = env_or("MERL_ALERT_LINE", "INT_ALERT_PIN");
    g.alert_pulse_ms = (unsigned)strtoul(env_or("MERL_ALERT_PULSE_MS", "1000"), NULL, 10);

    bool init_db = false;
    int opt;
    while ((opt = getopt(argc, argv, "d:b:s:o:c:p:vIh")) != -1) {
        switch (opt) {
            case 'd': g.tty_path = optarg; break;
            case 'b': g.baudrate = (unsigned)strtoul(optarg, NULL, 10); break;
            case 's': g.socket_path = optarg; break;
            case 'o': g.state_path = optarg; break;
            case 'c': g.chat_path = optarg; break;
            case 'p': g.poll_ms = (unsigned)strtoul(optarg, NULL, 10); break;
            case 'v': g.verbose = true; break;
            case 'I': init_db = true; break;
            default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    // Deploy / backup / restore of the chat history (just db-*): the schema lives only here
    if (init_db) {
        const bool ok = chat_open(g.chat_path) && chat_check();
        if (ok)
            printf("%s: ok, %ld messages, last id %lld\n", g.chat_path, chat_count(), (long long)chat_last_id());
        chat_close();
        return ok ? 0 : 1;
    }

    if (baud_constant(g.baudrate) == 0) {
        log_msg("unsupported baudrate %u", g.baudrate);
        return 2;
    }
    if (g.poll_ms < 200)
        g.poll_ms = 200;

    for (int i = 0; i < MAX_CLIENTS; ++i)
        g.clients[i].fd = -1;

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    g.listen_fd = listen_socket(g.socket_path);
    if (g.listen_fd < 0) {
        log_msg("cannot listen on %s: %s", g.socket_path, strerror(errno));
        return 1;
    }
    chmod(g.socket_path, 0660);

    if (!chat_open(g.chat_path))
        log_msg("chat history unavailable (%s), messages are not stored", g.chat_path);
    log_msg("merld: tty %s, socket %s, state %s, poll %u ms", g.tty_path, g.socket_path, g.state_path, g.poll_ms);
    if (strcmp(g.alert_line, "none") != 0)
        alert_open(g.alert_line);
    tty_open();
    publish_state();
    g.next_poll_ms = now_ms();

    while (!g_stop) {
        const int64_t now = now_ms();

        if (g.tty_fd < 0 && now >= g.tty_retry_ms)
            tty_open();

        if (g.in_flight && now >= g.deadline_ms)
            complete_job(NULL);

        // drop a half-received frame if the rest never comes
        if (g.tty_fd >= 0 && merl_parser_busy(&g.parser) && now - g.rx_last_ms > MERL_PROTOCOL_RX_TIMEOUT_MS)
            merl_parser_reset(&g.parser);

        if (g.tty_fd >= 0 && now >= g.next_poll_ms) {
            poll_unit(MERL_CMD_NETWORK_GET);
            poll_unit(MERL_CMD_POSITION_GET);
            poll_unit(MERL_CMD_PEERS_GET);
            if (g.version_info == INFO_UNKNOWN)
                poll_unit(MERL_CMD_DEVICE_VERSION);
            if (g.features_info == INFO_UNKNOWN)
                poll_unit(MERL_CMD_DEVICE_FEATURES);
            if (has_feature(MERL_FEATURE_GPS) && g.gps_info != INFO_UNSUPPORTED)
                poll_unit(MERL_CMD_GPS_STATUS);
            g.next_poll_ms = now + g.poll_ms;
        }
        if (g.tty_fd >= 0 && has_feature(MERL_FEATURE_LINK_STATS) && now >= g.next_link_stats_ms) {
            poll_unit(MERL_CMD_RADIO_STATS_GET);
            poll_unit(MERL_CMD_LINK_STATS_GET);
            g.next_link_stats_ms = now + LINK_STATS_POLL_MS;
        }
        if (g.tty_fd >= 0 && has_compass() && now >= g.next_compass_ms) {
            poll_unit(MERL_CMD_COMPASS_GET);
            g.next_compass_ms = now + COMPASS_POLL_MS;
        }
        alert_service(now);

        // clients whose job can no longer run
        if (g.tty_fd < 0) {
            while (g.queue_count > 0) {
                const int client = g.queue[g.queue_head].client;
                chat_set_status(g.queue[g.queue_head].message_id, "failed: no link");
                g.queue_head = (g.queue_head + 1) % QUEUE_SIZE;
                --g.queue_count;
                if (client >= 0)
                    client_error(client, "no link: %s closed", g.tty_path);
            }
            g.in_flight = false;
        }

        send_next();

        struct pollfd fds[2 + MAX_CLIENTS];
        int client_of[2 + MAX_CLIENTS];
        nfds_t nfds = 0;
        fds[nfds] = (struct pollfd){.fd = g.listen_fd, .events = POLLIN};
        client_of[nfds++] = -1;
        if (g.tty_fd >= 0) {
            fds[nfds] = (struct pollfd){.fd = g.tty_fd, .events = POLLIN};
            client_of[nfds++] = -2;
        }
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (g.clients[i].fd >= 0 && !g.clients[i].waiting) {
                fds[nfds] = (struct pollfd){.fd = g.clients[i].fd, .events = POLLIN};
                client_of[nfds++] = i;
            }
        }

        int64_t wake = now + 1000;
        if (g.in_flight && g.deadline_ms < wake)
            wake = g.deadline_ms;
        if (g.tty_fd >= 0 && g.next_poll_ms < wake)
            wake = g.next_poll_ms;
        if (g.tty_fd >= 0 && has_compass() && g.next_compass_ms < wake)
            wake = g.next_compass_ms;
        if (g.tty_fd < 0 && g.tty_retry_ms < wake)
            wake = g.tty_retry_ms;
        if (g.alert_until_ms && g.alert_until_ms < wake)
            wake = g.alert_until_ms;
        if (merl_parser_busy(&g.parser) && g.rx_last_ms + MERL_PROTOCOL_RX_TIMEOUT_MS + 1 < wake)
            wake = g.rx_last_ms + MERL_PROTOCOL_RX_TIMEOUT_MS + 1;
        const int timeout = wake > now ? (int)(wake - now) : 0;

        if (poll(fds, nfds, timeout) < 0) {
            if (errno == EINTR)
                continue;
            log_msg("poll: %s", strerror(errno));
            break;
        }

        for (nfds_t i = 0; i < nfds; ++i) {
            if (!fds[i].revents)
                continue;
            if (client_of[i] == -1)
                accept_client();
            else if (client_of[i] == -2 && g.tty_fd >= 0) {
                if (fds[i].revents & POLLIN)
                    tty_readable();
                if (g.tty_fd >= 0 && (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)))
                    tty_close("device gone");
            }
            else if (client_of[i] >= 0 && g.clients[client_of[i]].fd == fds[i].fd)
                client_readable(client_of[i]);
        }
    }

    log_msg("merld: stopping");
    alert_close();
    chat_close();
    unlink(g.socket_path);
    return 0;
}
