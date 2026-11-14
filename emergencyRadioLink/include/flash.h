#ifndef EMERGENCYRADIOLINK_FLASH_H
#define EMERGENCYRADIOLINK_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "network.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parameters kept in flash between power cycles.
 *
 * Add new fields at the end. Changing the layout invalidates the stored record, so the
 * device falls back to its defaults after a firmware update that does so.
 */
typedef struct {
    network_t network;      /**< Device address and subnet mask. */
} flash_params_t;

/**
 * @brief Prepare the calling core for flash writes issued from the other core.
 *
 * Must be called once on EACH core (before launching core 1 on core 0, and at the start
 * of the core 1 entry), otherwise flash_params_save() refuses to write.
 *
 * @return true on success.
 */
bool flash_core_init(void);

/**
 * @brief Load the most recently saved parameters.
 *
 * @param[out] params Filled with the stored parameters on success, untouched otherwise.
 * @return true if a valid record was found, false if nothing was saved yet or the data is corrupt.
 */
bool flash_params_load(flash_params_t* params);

/**
 * @brief Save the parameters to flash.
 *
 * Nothing is written if they are identical to the stored ones. Blocks for the duration of the
 * flash operation (a few ms, up to ~50 ms when the sector has to be erased), during which
 * interrupts are disabled on both cores. Records are appended to a log inside one sector,
 * which is erased only when full. If power is lost during that erase the stored parameters
 * are lost and the defaults are used at the next boot.
 *
 * @param params Parameters to save, must not be NULL.
 * @return true if the parameters are stored (and read back correctly), false otherwise.
 */
bool flash_params_save(const flash_params_t* params);

/**
 * @brief Load the saved network address and subnet mask.
 *
 * @param[out] network Filled on success, untouched otherwise.
 * @return true if a valid record was found.
 */
bool flash_network_load(network_t* network);

/**
 * @brief Save the network address and subnet mask, keeping the other saved parameters.
 *
 * @param network Network parameters to save, must not be NULL.
 * @return true on success, see flash_params_save().
 */
bool flash_network_save(const network_t* network);

#ifdef __cplusplus
}
#endif

#endif //EMERGENCYRADIOLINK_FLASH_H
