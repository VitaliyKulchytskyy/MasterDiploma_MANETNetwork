#include "watchdog.h"

#include <hardware/watchdog.h>
#include <pico/platform.h>
#include <pico/time.h>

static uint32_t g_timeout_ms;
static volatile uint64_t g_core_alive_us[2];

void watchdog_init(const uint32_t timeout_ms) {
    g_timeout_ms = timeout_ms;

    const uint64_t now = to_us_since_boot(get_absolute_time());
    g_core_alive_us[0] = now;
    g_core_alive_us[1] = now;

    watchdog_enable(timeout_ms, true);
}

void watchdog_mark_alive(void) {
    g_core_alive_us[get_core_num()] = to_us_since_boot(get_absolute_time());
}

void watchdog_service(void) {
    const uint64_t now = to_us_since_boot(get_absolute_time());
    const uint64_t margin_us = (uint64_t)g_timeout_ms * 1000 / 2;

    if (now - g_core_alive_us[0] > margin_us || now - g_core_alive_us[1] > margin_us)
        return;

    watchdog_update();
}

bool watchdog_caused_last_reboot(void) {
    return watchdog_caused_reboot();
}
