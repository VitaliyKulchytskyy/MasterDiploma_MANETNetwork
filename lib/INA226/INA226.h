#ifndef INA226_H
#define INA226_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

//  REGISTERS
#define INA226_CONFIGURATION              0x00
#define INA226_SHUNT_VOLTAGE              0x01
#define INA226_BUS_VOLTAGE                0x02
#define INA226_POWER                      0x03
#define INA226_CURRENT                    0x04
#define INA226_CALIBRATION                0x05
#define INA226_MASK_ENABLE                0x06
#define INA226_ALERT_LIMIT                0x07
#define INA226_MANUFACTURER               0xFE
#define INA226_DIE_ID                     0xFF

//  CONFIGURATION MASKS
#define INA226_CONF_RESET_MASK            0x8000
#define INA226_CONF_AVERAGE_MASK          0x0E00
#define INA226_CONF_BUSVC_MASK            0x01C0
#define INA226_CONF_SHUNTVC_MASK          0x0038
#define INA226_CONF_MODE_MASK             0x0007

//  ALERT FLAGS
#define INA226_CONVERSION_READY_FLAG      0x0008
#define INA226_ALERT_LATCH_ENABLE_FLAG    0x0001
#define INA226_ALERT_POLARITY_FLAG        0x0002

//  ERROR CODES
#define INA226_ERR_NONE                   0
#define INA226_ERR_SHUNTVOLTAGE_HIGH     -1
#define INA226_ERR_MAXCURRENT_LOW        -2
#define INA226_ERR_SHUNT_LOW             -3
#define INA226_ERR_NORMALIZE_FAILED      -4

//  LIMITS
#define INA226_MINIMAL_SHUNT_OHM          1e-6f
#define INA226_MAX_SHUNT_VOLTAGE          0.08190f


/**
 * INA226 device handle.
 * All fields are considered private — use the API functions below.
 */
typedef struct
{
    uint8_t  address;
    int      last_error;

    float    current_LSB;
    float    maxCurrent;
    float    shunt;
    float    current_zero_offset;
    uint16_t bus_V_scaling_e4;
} INA226_t;


/**
 * Write a 16-bit value to a register.
 *
 * @param dev   Pointer to the device handle (contains the I2C address).
 * @param reg   Register address.
 * @param value Value to write.
 * @return      0 on success, non-zero on error.
 */
int INA226_writeRegister(INA226_t *dev, uint8_t reg, uint16_t value) __attribute__((weak));

/**
 * Read a 16-bit value from a register.
 *
 * @param dev   Pointer to the device handle (contains the I2C address).
 * @param reg   Register address.
 * @param out   Pointer to store the read value.
 * @return      0 on success, non-zero on error.
 */
int INA226_readRegister(INA226_t *dev, uint8_t reg, uint16_t *out) __attribute__((weak));

/** @brief Current time in milliseconds. Override for your platform.
 *  @return Milliseconds since boot. */
uint32_t INA226_millis(void) __attribute__((weak));

/** @brief Blocks for the given number of milliseconds. Override for your platform.
 *  @param ms Duration to wait. */
void INA226_delayMs(uint32_t ms) __attribute__((weak));

/** @brief Initialises the device handle.
 *  @param dev     Device handle.
 *  @param address I2C address of the device. */
void INA226_init(INA226_t *dev, uint8_t address);

/** @brief Checks if the device is reachable on the bus.
 *  @param dev Device handle.
 *  @return    True if the device ACKs. */
bool INA226_isConnected(INA226_t *dev);

/** @brief Returns the I2C address stored in the handle.
 *  @param dev Device handle.
 *  @return    I2C address. */
uint8_t INA226_getAddress(const INA226_t *dev);

/** @brief Reads the bus voltage.
 *  @param dev Device handle.
 *  @return    Bus voltage in volts. */
float INA226_getBusVoltage(INA226_t *dev);

/** @brief Reads the shunt voltage.
 *  @param dev Device handle.
 *  @return    Shunt voltage in volts. */
float INA226_getShuntVoltage(INA226_t *dev);

/** @brief Reads the measured current.
 *  @param dev Device handle.
 *  @return    Current in amperes. */
float INA226_getCurrent(INA226_t *dev);

/** @brief Reads the measured power.
 *  @param dev Device handle.
 *  @return    Power in watts. */
float INA226_getPower(INA226_t *dev);

/** @brief Checks if a conversion has completed.
 *  @param dev Device handle.
 *  @return    True if conversion is ready. */
bool INA226_isConversionReady(INA226_t *dev);

/** @brief Blocks until conversion is ready or timeout elapses.
 *  @param dev        Device handle.
 *  @param timeout_ms Timeout in milliseconds.
 *  @return           True if conversion completed, false on timeout. */
bool INA226_waitConversionReady(INA226_t *dev, uint32_t timeout_ms);

/** @brief Resets all registers to default values.
 *  @param dev Device handle.
 *  @return    True on success. */
bool INA226_reset(INA226_t *dev);

/** @brief Sets the number of averages.
 *  @param dev Device handle.
 *  @param avg Averaging count (0–7, see datasheet table 4).
 *  @return    True on success. */
bool INA226_setAverage(INA226_t *dev, uint8_t avg);

/** @brief Returns the current averaging setting.
 *  @param dev Device handle.
 *  @return    Averaging register value (0–7). */
uint8_t INA226_getAverage(INA226_t *dev);

/** @brief Sets the bus voltage conversion time.
 *  @param dev  Device handle.
 *  @param bvct Conversion time (0–7, see datasheet table 5).
 *  @return     True on success. */
bool INA226_setBusVoltageConversionTime(INA226_t *dev, uint8_t bvct);

/** @brief Returns the bus voltage conversion time setting.
 *  @param dev Device handle.
 *  @return    Conversion time register value (0–7). */
uint8_t INA226_getBusVoltageConversionTime(INA226_t *dev);

/** @brief Sets the shunt voltage conversion time.
 *  @param dev  Device handle.
 *  @param svct Conversion time (0–7, see datasheet table 5).
 *  @return     True on success. */
bool INA226_setShuntVoltageConversionTime(INA226_t *dev, uint8_t svct);

/** @brief Returns the shunt voltage conversion time setting.
 *  @param dev Device handle.
 *  @return    Conversion time register value (0–7). */
uint8_t INA226_getShuntVoltageConversionTime(INA226_t *dev);

/** @brief Calibrates using max current and shunt resistance.
 *  @param dev        Device handle.
 *  @param maxCurrent Maximum expected current in amperes.
 *  @param shunt      Shunt resistance in ohms.
 *  @param normalize  Round current_LSB to nearest 1/2/5 × 10^n.
 *  @return           INA226_ERR_NONE or a negative error code. */
int INA226_setMaxCurrentShunt(INA226_t *dev, float maxCurrent, float shunt, bool normalize);

/** @brief Calibrates with explicit LSB and offset values.
 *  @param dev                    Device handle.
 *  @param shunt                  Shunt resistance in ohms.
 *  @param current_LSB_mA         Current LSB in milliamperes per bit.
 *  @param current_zero_offset_mA Zero-current offset in milliamperes.
 *  @param bus_V_scaling_e4       Bus voltage scaling factor × 10000 (10000 = no scaling).
 *  @return                       INA226_ERR_NONE or a negative error code. */
int INA226_configure(INA226_t *dev, float shunt, float current_LSB_mA,
                     float current_zero_offset_mA, uint16_t bus_V_scaling_e4);

/** @brief Sets the operating mode.
 *  @param dev  Device handle.
 *  @param mode Mode value (0–7, see datasheet table 6).
 *  @return     True on success. */
bool    INA226_setMode(INA226_t *dev, uint8_t mode);

/** @brief Returns the current operating mode.
 *  @param dev Device handle.
 *  @return    Mode register value (0–7). */
uint8_t INA226_getMode(INA226_t *dev);

/** @brief Writes the Mask/Enable alert register directly.
 *  @param dev  Device handle.
 *  @param mask Raw 16-bit register value.
 *  @return     True on success. */
bool INA226_setAlertRegister(INA226_t *dev, uint16_t mask);

/** @brief Reads the Mask/Enable alert register.
 *  @param dev Device handle.
 *  @return    Raw 16-bit register value. */
uint16_t INA226_getAlertRegister(INA226_t *dev);

/** @brief Returns the lower 5 alert flag bits of the Mask/Enable register.
 *  @param dev Device handle.
 *  @return    Alert flags (bits 4:0). @deprecated Use INA226_getAlertRegister(). */
uint16_t INA226_getAlertFlag(INA226_t *dev);

/** @brief Enables or disables alert latch mode.
 *  @param dev   Device handle.
 *  @param latch True to latch the alert pin until cleared.
 *  @return      True on success. */
bool INA226_setAlertLatchEnable(INA226_t *dev, bool latch);

/** @brief Returns the alert latch enable setting.
 *  @param dev Device handle.
 *  @return    True if latch mode is enabled. */
bool INA226_getAlertLatchEnable(INA226_t *dev);

/** @brief Sets the alert pin polarity.
 *  @param dev      Device handle.
 *  @param inverted True for active-high, false for active-low.
 *  @return         True on success. */
bool INA226_setAlertPolarity(INA226_t *dev, bool inverted);

/** @brief Returns the alert pin polarity setting.
 *  @param dev Device handle.
 *  @return    True if active-high. */
bool INA226_getAlertPolarity(INA226_t *dev);

/** @brief Sets the alert threshold limit register.
 *  @param dev   Device handle.
 *  @param limit Raw 16-bit threshold value.
 *  @return      True on success. */
bool INA226_setAlertLimit(INA226_t *dev, uint16_t limit);

/** @brief Reads the alert threshold limit register.
 *  @param dev Device handle.
 *  @return    Raw 16-bit threshold value. */
uint16_t INA226_getAlertLimit(INA226_t *dev);

/** @brief Reads the manufacturer ID register.
 *  @param dev Device handle.
 *  @return    Manufacturer ID (expected 0x5449). */
uint16_t INA226_getManufacturerID(INA226_t *dev);

/** @brief Reads the die ID register.
 *  @param dev Device handle.
 *  @return    Die ID (expected 0x2260). */
uint16_t INA226_getDieID(INA226_t *dev);

/** @brief Returns and clears the last I2C error code.
 *  @param dev Device handle.
 *  @return    0 on no error, negative value on I2C failure. */
int INA226_getLastError(INA226_t *dev);


#ifdef __cplusplus
}
#endif

#endif  /* INA226_H */