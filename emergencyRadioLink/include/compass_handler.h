#ifndef EMERGENCYRADIOLINK_COMPASS_HANDLER_H
#define EMERGENCYRADIOLINK_COMPASS_HANDLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Last known magnetometer reading, from the QMC5883L or compass_handler_set(). */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} compass_reading_t;

/**
 * @brief Initialize the compass. Call once before compass_handler_service().
 */
void compass_handler_init(void);

/**
 * @brief Poll the QMC5883L and refresh the cached reading.
 *
 * A failed I2C read leaves the cached reading (and whether one is available at all)
 * unchanged. Never blocks longer than one I2C transaction, call it periodically from one
 * core only, because it is the only one talking to the sensor over I2C.
 */
void compass_handler_service(void);

/**
 * @brief Get the last known reading (see compass_handler_service()).
 *
 * @param[out] out Filled with the last known reading on success, untouched otherwise.
 * @return true if a reading is available (the sensor answered at least once, or one was set
 *         manually).
 */
bool compass_handler_get(compass_reading_t* out);

/**
 * @brief Manually set the last known reading (MERL_CMD_COMPASS_SET), for example to test
 * without a sensor attached, or on a board with no compass wired at all.
 *
 * Works regardless of whether FEATURE_COMPASS is compiled in. If it is, the next successful
 * compass_handler_service() poll overwrites it; if not, this is the only way the device ever
 * reports a reading.
 *
 * @return Always true.
 */
bool compass_handler_set(int16_t x, int16_t y, int16_t z);

#ifdef __cplusplus
}
#endif

#endif //EMERGENCYRADIOLINK_COMPASS_HANDLER_H
