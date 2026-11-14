#ifndef EMERGENCYRADIOLINK_GPS_HANDLER_H
#define EMERGENCYRADIOLINK_GPS_HANDLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Last known position, either from a GPS fix or from gps_handler_set_position().
 */
typedef struct {
    uint32_t time;       /**< Milliseconds since UTC midnight of the fix. 0 for a manually set position. */
    int32_t longitude;   /**< Degrees * 1e6 (microdegrees). West is negative. */
    int32_t latitude;    /**< Degrees * 1e6 (microdegrees). South is negative. */
} gps_position_t;

/**
 * @brief Reset the GPS parser state. Call once before gps_handler_service().
 */
void gps_handler_init(void);

/**
 * @brief Feed the bytes received so far on UART_GPS into the NMEA parser.
 *
 * Updates the cached position whenever a sentence completes while the last RMC sentence
 * reported an active fix. Never blocks, call it periodically from one core only, because
 * it is the only reader of UART_GPS.
 */
void gps_handler_service(void);

/**
 * @brief Get the last known position.
 *
 * @param[out] out Filled with the last known position on success, untouched otherwise.
 * @return true if a position is available (a GPS fix was seen, or one was set manually).
 */
bool gps_handler_get_position(gps_position_t* out);

/**
 * @brief Manually set the last known position (MERL_CMD_POSITION_SET), for example
 * to configure a fixed installation that has no GPS fix, or no GPS module at all.
 *
 * Works regardless of whether FEATURE_GPS is compiled in. If it is, the next GPS fix overwrites
 * it; if not, this is the only way the device ever reports a position.
 *
 * @param longitude Degrees * 1e6 (microdegrees). West is negative.
 * @param latitude  Degrees * 1e6 (microdegrees). South is negative.
 * @return Always true.
 */
bool gps_handler_set_position(int32_t longitude, int32_t latitude);

/**
 * @brief The receiver's view of the sky (MERL_CMD_GPS_STATUS), whether it has a fix or not.
 */
typedef struct {
    uint8_t fix;                /**< GGA fix quality: 0 none, 1 GPS, 2 differential. */
    uint8_t satellites_used;    /**< Satellites in the fix (GGA). */
    uint8_t satellites_in_view; /**< Satellites the receiver sees (GSV). */
    uint16_t hdop_x100;         /**< Horizontal dilution of precision * 100, 0 = unknown. */
} gps_status_t;

/**
 * @brief Get the receiver's status, as parsed so far (all zero before its first sentence).
 *
 * @param[out] out Filled on success, untouched otherwise.
 * @return false without FEATURE_GPS: there is no receiver.
 */
bool gps_handler_get_status(gps_status_t* out);

#ifdef __cplusplus
}
#endif

#endif //EMERGENCYRADIOLINK_GPS_HANDLER_H
