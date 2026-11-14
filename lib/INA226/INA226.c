#include "INA226.h"
#include <math.h>
#include <stdio.h>


// __attribute__((weak)) int INA226_writeRegister(INA226_t *dev, uint8_t reg, uint16_t value) {
//     (void)dev;
//     (void)reg;
//     (void)value;
//     return -1;
// }
//
// __attribute__((weak)) int INA226_readRegister(INA226_t *dev, uint8_t reg, uint16_t *out) {
//     (void)dev;
//     (void)reg;
//     *out = 0;
//     return -1;
// }
//
// __attribute__((weak)) uint32_t INA226_millis(void) {
//     return 0;
// }
//
// __attribute__((weak)) void INA226_delayMs(uint32_t ms) {
//     (void)ms;
// }


/** Read a register and store the last I²C error in dev->last_error. */
static uint16_t _read(INA226_t *dev, uint8_t reg) {
    uint16_t val = 0;
    dev->last_error = INA226_readRegister(dev, reg, &val);
    return val;
}

static int _write(INA226_t *dev, uint8_t reg, uint16_t value) {
    dev->last_error = INA226_writeRegister(dev, reg, value);
    return dev->last_error;
}

static inline float _min_f(float a, float b) { return (a < b) ? a : b; }


/* =========================================================================
 *  Constructor / init
 * ========================================================================= */

void INA226_init(INA226_t *dev, uint8_t address) {
    dev->address             = address;
    dev->last_error          = 0;
    dev->current_LSB         = 0.0f;
    dev->maxCurrent          = 0.0f;
    dev->shunt               = 0.0f;
    dev->current_zero_offset = 0.0f;
    dev->bus_V_scaling_e4    = 10000;
}


/* =========================================================================
 *  Connection
 * ========================================================================= */

bool INA226_isConnected(INA226_t *dev) {
    uint16_t dummy = 0;
    int rc = INA226_readRegister(dev, INA226_CONFIGURATION, &dummy);
    return (rc == 0);
}

uint8_t INA226_getAddress(const INA226_t *dev) {
    return dev->address;
}


/* =========================================================================
 *  Core measurements
 * ========================================================================= */

float INA226_getBusVoltage(INA226_t *dev) {
    uint16_t val = _read(dev, INA226_BUS_VOLTAGE);
    float voltage = (float)val * 1.25e-3f;
    if (dev->bus_V_scaling_e4 != 10000) {
        voltage *= (float)dev->bus_V_scaling_e4 * 1.0e-4f;
    }

    return voltage;
}

float INA226_getShuntVoltage(INA226_t *dev) {
    int16_t val = (int16_t)_read(dev, INA226_SHUNT_VOLTAGE);
    return (float)val * 2.5e-6f;
}

float INA226_getCurrent(INA226_t *dev) {
    int16_t val = (int16_t)_read(dev, INA226_CURRENT);
    return (float)val * dev->current_LSB - dev->current_zero_offset;
}

float INA226_getPower(INA226_t *dev) {
    uint16_t val = _read(dev, INA226_POWER);
    return (float)val * (dev->current_LSB * 25.0f);
}

bool INA226_isConversionReady(INA226_t *dev) {
    uint16_t mask = _read(dev, INA226_MASK_ENABLE);
    return (mask & INA226_CONVERSION_READY_FLAG) == INA226_CONVERSION_READY_FLAG;
}

bool INA226_waitConversionReady(INA226_t *dev, uint32_t timeout_ms) {
    uint32_t start = INA226_millis();
    while ((INA226_millis() - start) <= timeout_ms) {
        if (INA226_isConversionReady(dev))
            return true;
        INA226_delayMs(1);
    }

    return false;
}


/* =========================================================================
 *  Configuration
 * ========================================================================= */

bool INA226_reset(INA226_t *dev) {
    uint16_t mask = _read(dev, INA226_CONFIGURATION);
    mask |= INA226_CONF_RESET_MASK;
    int rc = _write(dev, INA226_CONFIGURATION, mask);
    if (rc != 0) return false;

    dev->current_LSB = 0.0f;
    dev->maxCurrent  = 0.0f;
    dev->shunt       = 0.0f;

    return true;
}

bool INA226_setAverage(INA226_t *dev, uint8_t avg) {
    if (avg > 7)
        return false;

    uint16_t mask = _read(dev, INA226_CONFIGURATION);
    mask &= ~INA226_CONF_AVERAGE_MASK;
    mask |= (uint16_t)(avg << 9);
    _write(dev, INA226_CONFIGURATION, mask);

    return true;
}

uint8_t INA226_getAverage(INA226_t *dev) {
    uint16_t mask = _read(dev, INA226_CONFIGURATION);
    mask &= INA226_CONF_AVERAGE_MASK;
    mask >>= 9;

    return (uint8_t)mask;
}

bool INA226_setBusVoltageConversionTime(INA226_t *dev, uint8_t bvct) {
    if (bvct > 7)
        return false;

    uint16_t mask = _read(dev, INA226_CONFIGURATION);
    mask &= ~INA226_CONF_BUSVC_MASK;
    mask |= (uint16_t)(bvct << 6);
    _write(dev, INA226_CONFIGURATION, mask);

    return true;
}

uint8_t INA226_getBusVoltageConversionTime(INA226_t *dev) {
    uint16_t mask = _read(dev, INA226_CONFIGURATION);
    mask &= INA226_CONF_BUSVC_MASK;
    mask >>= 6;

    return (uint8_t)mask;
}

bool INA226_setShuntVoltageConversionTime(INA226_t *dev, uint8_t svct) {
    if (svct > 7)
        return false;

    uint16_t mask = _read(dev, INA226_CONFIGURATION);
    mask &= ~INA226_CONF_SHUNTVC_MASK;
    mask |= (uint16_t)(svct << 3);
    _write(dev, INA226_CONFIGURATION, mask);

    return true;
}

uint8_t INA226_getShuntVoltageConversionTime(INA226_t *dev) {
    uint16_t mask = _read(dev, INA226_CONFIGURATION);
    mask &= INA226_CONF_SHUNTVC_MASK;
    mask >>= 3;

    return (uint8_t)mask;
}


/* =========================================================================
 *  Calibration
 * ========================================================================= */

int INA226_setMaxCurrentShunt(INA226_t *dev, float maxCurrent, float shunt, bool normalize)
{
    float shuntVoltage = maxCurrent * shunt;
    printf("DBG setMaxCurrentShunt: maxCurrent=%.6f shunt=%.6f shuntVoltage=%.6f\n",
           (double)maxCurrent, (double)shunt, (double)shuntVoltage);
    /*
     * Datasheet limit is 81.92 mV. We add a small epsilon to absorb
     * IEEE 754 single-precision rounding errors (e.g. 0.8f * 0.1f is
     * not exactly 0.08 in binary float).
     */
    if (shuntVoltage > 0.08192f + 1e-6f)      return INA226_ERR_SHUNTVOLTAGE_HIGH;
    if (maxCurrent < 0.001f)                  return INA226_ERR_MAXCURRENT_LOW;
    if (shunt < INA226_MINIMAL_SHUNT_OHM)     return INA226_ERR_SHUNT_LOW;

    dev->current_LSB = maxCurrent * 3.0517578125e-5f;  /* maxCurrent / 32768 */

    if (normalize)
    {
        /* Ensure LSB is not limited by the calibration register range. */
        float lsb_min = 1.5625e-7f / shunt;
        if (lsb_min > dev->current_LSB)
        {
            dev->current_LSB = lsb_min;
        }

        /* Round up to the nearest 1 / 2 / 5 × 10^n µA */
        uint16_t currentLSB_uA = (uint16_t)(dev->current_LSB * 1e6f);
        currentLSB_uA++;  /* implicit ceil */

        uint16_t factor  = 1;
        uint8_t  i       = 0;
        bool     result  = false;

        do {
            if      (1u * factor >= currentLSB_uA) { dev->current_LSB = 1u * factor * 1e-6f; result = true; }
            else if (2u * factor >= currentLSB_uA) { dev->current_LSB = 2u * factor * 1e-6f; result = true; }
            else if (5u * factor >= currentLSB_uA) { dev->current_LSB = 5u * factor * 1e-6f; result = true; }
            else { factor *= 10u; i++; }
        } while ((i < 4) && (!result));

        if (!result)
        {
            dev->current_LSB = 0.0f;
            return INA226_ERR_NORMALIZE_FAILED;
        }
    }

    /* Auto-scale calibration value into the 15-bit register. */
    uint32_t calib = (uint32_t)round(0.00512f / (dev->current_LSB * shunt));
    while (calib > 32767u)
    {
        dev->current_LSB *= 2.0f;
        calib >>= 1;
    }
    _write(dev, INA226_CALIBRATION, (uint16_t)calib);

    dev->maxCurrent = dev->current_LSB * 32768.0f;
    dev->shunt      = shunt;

    return INA226_ERR_NONE;
}

int INA226_configure(INA226_t *dev,
                     float    shunt,
                     float    current_LSB_mA,
                     float    current_zero_offset_mA,
                     uint16_t bus_V_scaling_e4)
{
    if (shunt < INA226_MINIMAL_SHUNT_OHM)
        return INA226_ERR_SHUNT_LOW;

    float maxCurrent = _min_f(INA226_MAX_SHUNT_VOLTAGE / shunt,
                               32768.0f * current_LSB_mA * 1e-3f);
    if (maxCurrent < 0.001f) return INA226_ERR_MAXCURRENT_LOW;

    dev->shunt               = shunt;
    dev->current_LSB         = current_LSB_mA * 1e-3f;
    dev->current_zero_offset = current_zero_offset_mA * 1e-3f;
    dev->bus_V_scaling_e4    = bus_V_scaling_e4;
    dev->maxCurrent          = maxCurrent;

    uint32_t calib = (uint32_t)round(0.00512f / (dev->current_LSB * dev->shunt));
    _write(dev, INA226_CALIBRATION, (uint16_t)calib);

    return INA226_ERR_NONE;
}


/* =========================================================================
 *  Operating mode
 * ========================================================================= */

bool INA226_setMode(INA226_t *dev, uint8_t mode) {
    if (mode > 7) return false;
    uint16_t config = _read(dev, INA226_CONFIGURATION);
    config &= ~INA226_CONF_MODE_MASK;
    config |= mode;
    _write(dev, INA226_CONFIGURATION, config);
    return true;
}

uint8_t INA226_getMode(INA226_t *dev) {
    uint16_t mode = _read(dev, INA226_CONFIGURATION);
    mode &= INA226_CONF_MODE_MASK;
    return (uint8_t)mode;
}


/* =========================================================================
 *  Alert
 * ========================================================================= */

bool INA226_setAlertRegister(INA226_t *dev, uint16_t mask) {
    return (_write(dev, INA226_MASK_ENABLE, mask) == 0);
}

uint16_t INA226_getAlertRegister(INA226_t *dev) {
    return _read(dev, INA226_MASK_ENABLE);
}

uint16_t INA226_getAlertFlag(INA226_t *dev) {
    return _read(dev, INA226_MASK_ENABLE) & 0x001Fu;
}

bool INA226_setAlertLatchEnable(INA226_t *dev, bool latch) {
    uint16_t mask = _read(dev, INA226_MASK_ENABLE);
    if (latch) mask |=  INA226_ALERT_LATCH_ENABLE_FLAG;
    else       mask &= ~INA226_ALERT_LATCH_ENABLE_FLAG;
    return (_write(dev, INA226_MASK_ENABLE, mask) == 0);
}

bool INA226_getAlertLatchEnable(INA226_t *dev) {
    return (_read(dev, INA226_MASK_ENABLE) & INA226_ALERT_LATCH_ENABLE_FLAG) != 0u;
}

bool INA226_setAlertPolarity(INA226_t *dev, bool inverted) {
    uint16_t mask = _read(dev, INA226_MASK_ENABLE);
    if (inverted) mask |=  INA226_ALERT_POLARITY_FLAG;
    else          mask &= ~INA226_ALERT_POLARITY_FLAG;
    return (_write(dev, INA226_MASK_ENABLE, mask) == 0);
}

bool INA226_getAlertPolarity(INA226_t *dev) {
    return (_read(dev, INA226_MASK_ENABLE) & INA226_ALERT_POLARITY_FLAG) != 0u;
}

bool INA226_setAlertLimit(INA226_t *dev, uint16_t limit) {
    return (_write(dev, INA226_ALERT_LIMIT, limit) == 0);
}

uint16_t INA226_getAlertLimit(INA226_t *dev) {
    return _read(dev, INA226_ALERT_LIMIT);
}

/* =========================================================================
 *  Meta information
 * ========================================================================= */

uint16_t INA226_getManufacturerID(INA226_t *dev) {
    return _read(dev, INA226_MANUFACTURER);
}

uint16_t INA226_getDieID(INA226_t *dev) {
    return _read(dev, INA226_DIE_ID);
}


/* =========================================================================
 *  Error handling
 * ========================================================================= */

int INA226_getLastError(INA226_t *dev) {
    int e = dev->last_error;
    dev->last_error = 0;
    return e;
}