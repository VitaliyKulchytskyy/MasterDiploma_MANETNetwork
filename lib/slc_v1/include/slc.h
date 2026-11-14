#ifndef SLC_V1_SLC_H
#define SLC_V1_SLC_H

#include <stdbool.h>
#include <stdint.h>


typedef struct __attribute__((__packed__)) {
    uint32_t mac;
    uint8_t status;
    uint8_t payload_size;
    uint8_t payload[0];
    // uint8_t checksum;
} packet_t;

typedef struct __attribute__((__packed__)) {
    uint8_t version_signature;
    uint16_t body_size;
    uint8_t chained;
    packet_t packet[0];
    // uint8_t checksum; // version_signature + chained + crc of each chained packet
} body_t;

#endif //SLC_V1_SLC_H