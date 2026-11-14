// ina226d: battery monitor of the main unit.
//
// Reads the INA226 (lib/INA226) over Linux i2c-dev every interval and publishes the
// readings as JSON in the state file for the web UI. If the sensor stops answering,
// the state says so and the sensor is set up again every few seconds.
//
// Settings come from the environment (/etc/default/ina226d via systemd).

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "INA226.h"
#include "INA226_linux.h"

#define RETRY_MS    5000
#define DEAD_BUS_RETRY_MS 60000     // nothing drives the bus: look again rarely (each probe logs a kernel timeout)

static volatile sig_atomic_t g_stop;

static struct {
    const char* bus;
    uint8_t address_config; // INA226_ADDR, 0 = auto: search INA226_ADDR_FIRST..LAST
    uint8_t address;        // where the INA226 was found, 0 = not found
    bool bus_dead;          // the last search timed out: nothing connected / no pull-ups on the bus
    float shunt_ohm;
    float max_current_a;
    uint8_t average;        // INA226 averaging code 0..7 (1, 4, 16, 64, 128, 256, 512, 1024 samples)
    unsigned interval_ms;
    unsigned cells;         // Li-ion cells in series (2 = 2S): the web UI turns the voltage into a charge %
    unsigned capacity_mah;  // the pack's capacity: the web UI's runtime estimate is based on it
    const char* state_path;
    float avg_current_ma;   // current averaged over ~AVG_CURRENT_TAU_S, for the runtime estimate
    unsigned samples;       // readings since start
} g;

#define AVG_CURRENT_TAU_S 60.0f

#define INA226_ADDR_FIRST       0x40    // the chip's A0/A1 pins give 16 addresses
#define INA226_ADDR_LAST        0x4F
#define TI_MANUFACTURER_ID      0x5449  // "TI", in the INA226_MANUFACTURER register
#define INA226_DIE_ID_VALUE     0x2260  // in the INA226_DIE_ID register

static void log_msg(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static const char* env_or(const char* name, const char* fallback) {
    const char* value = getenv(name);
    return value && *value ? value : fallback;
}

static void write_state(const char* json) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g.state_path);
    FILE* f = fopen(tmp, "w");
    if (!f) {
        log_msg("cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    fputs(json, f);
    if (fclose(f) != 0 || rename(tmp, g.state_path) != 0)
        log_msg("cannot update %s: %s", g.state_path, strerror(errno));
}

// The settings, for the web UI's Info page: "bus":..,"address":..,"average_samples":..,"interval_ms":..
static int settings_json(char* out, size_t size) {
    static const unsigned samples[] = {1, 4, 16, 64, 128, 256, 512, 1024};
    char address[8], config[8];
    snprintf(address, sizeof(address), g.address ? "0x%02X" : "none", g.address);
    snprintf(config, sizeof(config), g.address_config ? "0x%02X" : "auto", g.address_config);
    return snprintf(out, size,
                    "\"bus\":\"%s\",\"address\":\"%s\",\"address_config\":\"%s\",\"shunt_ohm\":%g,\"max_current_a\":%g,"
                    "\"average_samples\":%u,\"interval_ms\":%u,\"cells\":%u,\"capacity_mah\":%u",
                    g.bus, address, config, g.shunt_ohm, g.max_current_a, samples[g.average & 7], g.interval_ms, g.cells,
                    g.capacity_mah);
}

static void publish_error(const char* error) {
    char settings[256], json[512];
    settings_json(settings, sizeof(settings));
    snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\",%s,\"updated\":%lld}\n", error, settings,
             (long long)time(NULL));
    write_state(json);
}

// An INA226 answers at the address: the ID registers read back TI's manufacturer and die ids
static bool is_ina226(INA226_t* ina, uint8_t address) {
    INA226_init(ina, address);
    return INA226_isConnected(ina) && INA226_getManufacturerID(ina) == TI_MANUFACTURER_ID &&
           INA226_getDieID(ina) == INA226_DIE_ID_VALUE;
}

// The INA226: at INA226_ADDR if one is set and answers, otherwise the first one found in the
// chip's address range (its A0/A1 pins pick one of 0x40..0x4F). Searched again on every
// reconnect, so moving the module or changing its address pins needs no configuration.
//
// A missing chip on a working bus NACKs at once (ENXIO). A transfer that times out instead
// (ETIMEDOUT, 0.2 s on the Lyra) means nothing drives the bus: no module connected, so no
// pull-ups either (the SoC's own are off, see dt/). Then stop searching: 16 probes would only
// add 16 kernel timeout messages per attempt.
static uint8_t find_ina226(INA226_t* ina) {
    g.bus_dead = false;
    if (g.address_config) {
        errno = 0;
        if (is_ina226(ina, g.address_config))
            return g.address_config;
        if (errno == ETIMEDOUT) {
            g.bus_dead = true;
            return 0;
        }
    }
    for (uint8_t a = INA226_ADDR_FIRST; a <= INA226_ADDR_LAST; ++a) {
        if (a == g.address_config)
            continue;
        errno = 0;
        if (is_ina226(ina, a)) {
            if (g.address_config)
                log_msg("no INA226 at the configured 0x%02X, using the one found at 0x%02X", g.address_config, a);
            return a;
        }
        if (errno == ETIMEDOUT) {
            g.bus_dead = true;
            return 0;
        }
    }
    return 0;
}

static bool setup(INA226_t* ina) {
    if (INA226_linuxOpen(g.bus) < 0) {
        log_msg("cannot open %s: %s", g.bus, strerror(errno));
        publish_error("cannot open the I2C bus");
        return false;
    }

    g.address = find_ina226(ina);
    if (!g.address) {
        // publish_error(g.bus_dead ? "I2C bus not responding: nothing connected (or no pull-ups)"
        //                         : "no INA226 found on the bus (searched 0x40-0x4F)");
        return false;
    }
    INA226_init(ina, g.address);

    INA226_setAverage(ina, g.average);
    const int err = INA226_setMaxCurrentShunt(ina, g.max_current_a, g.shunt_ohm, true);
    if (err != INA226_ERR_NONE || INA226_getLastError(ina) != 0) {
        log_msg("calibration failed: %d (max current %.3f A, shunt %.4f ohm)", err, g.max_current_a, g.shunt_ohm);
        publish_error("calibration failed, check INA226_SHUNT_OHM / INA226_MAX_CURRENT_A");
        return false;
    }

    log_msg("INA226 at %s 0x%02X: shunt %.4f ohm, max %.3f A, average code %u", g.bus, g.address, g.shunt_ohm,
            g.max_current_a, g.average);
    return true;
}

// false: the sensor did not answer
static bool measure(INA226_t* ina) {
    int err = 0;
    const float bus_v = INA226_getBusVoltage(ina);
    err |= INA226_getLastError(ina);
    const float shunt_v = INA226_getShuntVoltage(ina);
    err |= INA226_getLastError(ina);
    const float current_a = INA226_getCurrent(ina);
    err |= INA226_getLastError(ina);
    const float power_w = INA226_getPower(ina);
    err |= INA226_getLastError(ina);
    if (err)
        return false;

    // exponential average: the current jumps with radio TX and CPU load, the runtime estimate
    // should follow the typical draw. The first reading after setup comes before the chip's
    // first averaged conversion (~0): skip it; until a time constant's worth of samples is in,
    // weigh them equally (a plain mean) so the average is right from the start.
    const float current_ma = current_a * 1000.0f;
    const float dt_s = g.interval_ms / 1000.0f;
    ++g.samples;
    if (g.samples == 2) {
        g.avg_current_ma = current_ma;
    } else if (g.samples > 2) {
        const float mean_weight = 1.0f / (float)(g.samples - 1);
        const float ema_weight = dt_s / (AVG_CURRENT_TAU_S + dt_s);
        g.avg_current_ma += (current_ma - g.avg_current_ma) * (mean_weight > ema_weight ? mean_weight : ema_weight);
    } else {
        g.avg_current_ma = current_ma;      // shown as is until the second reading
    }

    char settings[256], json[512];
    settings_json(settings, sizeof(settings));
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"bus_v\":%.3f,\"shunt_mv\":%.3f,\"current_ma\":%.1f,\"avg_current_ma\":%.1f,"
             "\"power_mw\":%.1f,%s,\"updated\":%lld}\n",
             bus_v, shunt_v * 1000.0f, current_ma, g.avg_current_ma, power_w * 1000.0f, settings,
             (long long)time(NULL));
    write_state(json);
    return true;
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void sleep_ms(unsigned ms) {
    struct timespec ts = {.tv_sec = ms / 1000u, .tv_nsec = (long)(ms % 1000u) * 1000000L};
    nanosleep(&ts, NULL);   // a signal ends the wait early, which is what we want
}

int main(void) {
    g.bus = env_or("INA226_I2C", "/dev/i2c-1");
    // "auto" (or anything that is not an address in the INA226 range): search
    const unsigned long address = strtoul(env_or("INA226_ADDR", "auto"), NULL, 0);
    g.address_config = address >= INA226_ADDR_FIRST && address <= INA226_ADDR_LAST ? (uint8_t)address : 0;
    g.shunt_ohm = strtof(env_or("INA226_SHUNT_OHM", "0.1"), NULL);
    g.max_current_a = strtof(env_or("INA226_MAX_CURRENT_A", "0.8"), NULL);
    g.average = (uint8_t)strtoul(env_or("INA226_AVERAGE", "3"), NULL, 0);
    g.interval_ms = (unsigned)strtoul(env_or("INA226_INTERVAL_MS", "1000"), NULL, 10);
    g.cells = (unsigned)strtoul(env_or("INA226_CELLS", "2"), NULL, 10);
    g.capacity_mah = (unsigned)strtoul(env_or("INA226_CAPACITY_MAH", "2600"), NULL, 10);
    g.state_path = env_or("INA226_STATE", "/run/ina226/power.json");
    if (g.interval_ms < 100)
        g.interval_ms = 100;

    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    INA226_t ina;
    bool ready = false;
    while (!g_stop) {
        if (!ready) {
            ready = setup(&ina);
            if (!ready) {
                INA226_linuxClose();
                sleep_ms(g.bus_dead ? DEAD_BUS_RETRY_MS : RETRY_MS);
                continue;
            }
        }
        if (!measure(&ina)) {
            log_msg("INA226 stopped answering");
            publish_error("INA226 not answering");
            ready = false;
            INA226_linuxClose();
            continue;
        }
        sleep_ms(g.interval_ms);
    }

    INA226_linuxClose();
    return 0;
}
