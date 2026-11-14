#ifndef EMERGENCYRADIOLINK_WATCHDOG_H
#define EMERGENCYRADIOLINK_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enable the hardware watchdog and reset both cores' liveness timers.
 *
 * Call once, from core 0, before multicore_launch_core1(). RP2040's watchdog counter maxes
 * out at 8388 ms, so @p timeout_ms must not exceed that.
 *
 * @param timeout_ms Reboot the chip if watchdog_service() has not fed it within this long.
 */
void watchdog_init(uint32_t timeout_ms);

/**
 * @brief Report that the calling core's main loop is still running.
 *
 * Call once per iteration of each core's loop (both core 0's and core1_entry()'s). A core
 * that stops calling this is what makes watchdog_service() stop feeding the watchdog.
 */
void watchdog_mark_alive(void);

/**
 * @brief Feed the hardware watchdog, but only if both cores reported alive recently.
 *
 * Call periodically, from either core (only one of them needs to). If one core has stopped
 * calling watchdog_mark_alive(), this stops feeding the watchdog and the whole chip reboots
 * once the configured timeout elapses.
 */
void watchdog_service(void);

/**
 * @brief Whether the previous boot ended in a watchdog reset, i.e. some loop hung.
 */
bool watchdog_caused_last_reboot(void);

#ifdef __cplusplus
}
#endif

#endif //EMERGENCYRADIOLINK_WATCHDOG_H
