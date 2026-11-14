#include "qmc5883.h"

#include <hardware/gpio.h>

// Register map and init sequence per the QST QMC5883L datasheet, cross-checked against
// mprograms/QMC5883LCompass (a widely used reference implementation of this exact chip).
#define QMC5883_I2C_ADDRESS      0x0D
#define QMC5883_REG_DATA_X_LSB   0x00   // 6 bytes from here: X_L, X_M, Y_L, Y_M, Z_L, Z_M
#define QMC5883_REG_CONTROL_1    0x09
#define QMC5883_REG_SET_RESET    0x0B

// Control register 1 (mode | odr | rng | osr): continuous mode, 200 Hz, +-8 gauss, 512x oversampling
#define QMC5883_MODE_CONTINUOUS  0x01
#define QMC5883_ODR_200HZ        0x0C
#define QMC5883_RNG_8G           0x10
#define QMC5883_OSR_512          0x00

static i2c_inst_t* g_i2c;

static bool write_reg(const uint8_t reg, const uint8_t value) {
    const uint8_t buf[2] = {reg, value};
    return i2c_write_blocking(g_i2c, QMC5883_I2C_ADDRESS, buf, sizeof(buf), false) == (int)sizeof(buf);
}

bool qmc5883_init(const qmc5883_config_t* config) {
    if (config == NULL || config->i2c == NULL)
        return false;

    g_i2c = config->i2c;
    i2c_init(g_i2c, config->baudrate > 0 ? config->baudrate : 100000);
    gpio_set_function(config->sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(config->scl_pin, GPIO_FUNC_I2C);
    // most breakout boards already have pull-ups; these are a harmless fallback if not
    gpio_pull_up(config->sda_pin);
    gpio_pull_up(config->scl_pin);

    // the SET/RESET period register must be written before enabling continuous mode,
    // otherwise readings drift per the datasheet
    return write_reg(QMC5883_REG_SET_RESET, 0x01) &&
           write_reg(QMC5883_REG_CONTROL_1,
                     QMC5883_MODE_CONTINUOUS | QMC5883_ODR_200HZ | QMC5883_RNG_8G | QMC5883_OSR_512);
}

bool qmc5883_read(int16_t* x, int16_t* y, int16_t* z) {
    if (x == NULL || y == NULL || z == NULL)
        return false;

    const uint8_t reg = QMC5883_REG_DATA_X_LSB;
    if (i2c_write_blocking(g_i2c, QMC5883_I2C_ADDRESS, &reg, 1, true) != 1)
        return false;

    uint8_t data[6];
    if (i2c_read_blocking(g_i2c, QMC5883_I2C_ADDRESS, data, sizeof(data), false) != (int)sizeof(data))
        return false;

    // little-endian on the wire (LSB register first), signed 16-bit per axis
    *x = (int16_t)((uint16_t)data[1] << 8 | data[0]);
    *y = (int16_t)((uint16_t)data[3] << 8 | data[2]);
    *z = (int16_t)((uint16_t)data[5] << 8 | data[4]);
    return true;
}
