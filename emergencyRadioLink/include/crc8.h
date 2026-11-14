#ifndef LINK_ROUTER_CRC8_H
#define LINK_ROUTER_CRC8_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize a global CRC8 table
 *
 * @param poly CRC8 polynomial value
 * @attention use CONFIG_CRC8_DISASSERT_TABLE to disable CRC8 initialization assertion
 */
void crc8_init_table(uint8_t poly);

/**
 * Calculate CRC8 for a value based on the previous value. For continuous calculation
 * when the buffer doesn't storage in an array. Use pre-generated values from the global CRC8 table
 *
 * @param data value
 * @param prev_data previous value
 * @return CRC8 check sum
 * @attention use CONFIG_CRC8_DISASSERT_TABLE to disable CRC8 initialization assertion
 */
uint8_t inline crc8_table_calc_next(uint8_t data, uint8_t prev_data);

/**
 * Calculate CRC8 for array, use pre-generated values from the global CRC8 table
 *
 * @param buf in array
 * @param buflen length of the array
 * @return CRC8 check sum
 * @attention use CONFIG_CRC8_DISASSERT_TABLE to disable CRC8 initialization assertion
 */
uint8_t crc8_table_calc(const uint8_t* buf, uint8_t buflen);

/**
 * Slow CRC8 calculation without the table
 *
 * @param buf in array
 * @param buflen length of the array
 * @param poly polynomial
 * @return CRC8 check sum
 */
uint8_t crc8_calc(const uint8_t* buf, uint8_t buflen, uint8_t poly);


#ifdef __cplusplus
}
#endif

#endif //LINK_ROUTER_CRC8_H