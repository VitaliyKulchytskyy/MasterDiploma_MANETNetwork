#ifndef EMERGENCYRADIOLINK_QMC5883_H
#define EMERGENCYRADIOLINK_QMC5883_H

#include <stdbool.h>
#include <stdint.h>

#include <hardware/i2c.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Configuration for a QMC5883L magnetometer. */
typedef struct {
    i2c_inst_t* i2c;    /**< I2C block to use (i2c0 or i2c1). */
    uint32_t sda_pin;   /**< GPIO number of the SDA line. */
    uint32_t scl_pin;   /**< GPIO number of the SCL line. */
    uint32_t baudrate;  /**< I2C bus speed in Hz. 0 defaults to 100000 (standard mode). */
} qmc5883_config_t;

/**
 * @brief Initialize the I2C bus and put the QMC5883L into continuous measurement mode
 * (200 Hz output rate, +-8 gauss range, 512x oversampling ratio).
 *
 * @param config Configuration, must not be NULL.
 * @return true if the chip acknowledged both setup writes on the bus.
 */
bool qmc5883_init(const qmc5883_config_t* config);

/**
 * @brief Read the raw X/Y/Z magnetometer axes.
 *
 * Values are signed ADC counts, not calibrated or scaled to any physical unit (gauss,
 * heading degrees, etc.) - that conversion is left to whoever consumes the reading.
 *
 * @param[out] x,y,z Filled on success, untouched otherwise.
 * @return true on success, false on an I2C error (nothing acknowledged the address, etc.).
 */
bool qmc5883_read(int16_t* x, int16_t* y, int16_t* z);

#ifdef __cplusplus
}
#endif

#endif //EMERGENCYRADIOLINK_QMC5883_H
