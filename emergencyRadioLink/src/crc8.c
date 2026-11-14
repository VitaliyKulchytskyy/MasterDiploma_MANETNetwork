#include "crc8.h"

static uint8_t g_lut[UINT8_MAX + 1];


void crc8_init_table(const uint8_t poly) {
    for (uint16_t idx = 0; idx <= UINT8_MAX; ++idx)
    {
        uint16_t crc = idx;
        for (uint16_t shift = 0; shift < 8; ++shift)
        {
            crc = (crc << 1) ^ ((crc & 0x80) ? poly : 0);
        }
        g_lut[idx] = crc & 0xff;
    }
}

uint8_t crc8_table_calc_next(const uint8_t data, const uint8_t prev_data) {
    return g_lut[prev_data ^ data];
}

uint8_t crc8_table_calc(const uint8_t* buf, uint8_t buflen) {
    uint8_t crc = 0;
    while (buflen--)
        crc = crc8_table_calc_next(*buf++, crc);

    return crc;
}

uint8_t crc8_calc(const uint8_t* buf, const uint8_t buflen, const uint8_t poly) {
    uint8_t crc = 0x00;
    uint16_t i, j;

    for (i = 0; i < buflen; i++) {
        crc ^= buf[i];
        for (j = 0; j < 8; j++) {
            if (crc & 0x01) {
                crc = (crc >> 1) ^ poly;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}