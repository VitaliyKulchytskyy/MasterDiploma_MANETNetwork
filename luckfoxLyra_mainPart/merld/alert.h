#ifndef MERLD_ALERT_H
#define MERLD_ALERT_H

#include <stdbool.h>

// INT_ALERT_PIN: a GPIO line from this host to the emergency unit (RP2040 CONFIG_INT_ALERT_PIN,
// an input with pull-down there). High = alert: the unit lights its LED red while it reads it,
// and for CONFIG_ALERT_DURATION_MS after. Driven through the GPIO character device (v1 uAPI:
// the SDK toolchain's kernel headers predate v2); the line is found by its device tree name.

/**
 * Find the line named @p name on any /dev/gpiochip* and hold it as an output, low.
 * @return false if there is no such line or it cannot be requested (reported on stderr).
 */
bool alert_open(const char* name);

/** Drive the line: true = high (alert), false = low. No-op without alert_open(). */
void alert_set(bool on);

bool alert_available(void);

/** Low again and released (the device tree's pull-down keeps it low). */
void alert_close(void);

#endif // MERLD_ALERT_H
