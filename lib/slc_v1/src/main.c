#include <stdio.h>
// #include "pico/stdlib.h"
#include <stdlib.h>
#include <string.h>

#include "slc.h"

// packet_t lora24 = {
//     .mac = 0x3E45FA,
//     .status = 0x00,
//     .payload_size = 6,
//     .payload = {'h', 'e', 'l', 'l', 'o', '\0'},
//     // .checksum = 0x00
// };

void print_packet(const packet_t *packet) {
    printf("mac: %u\nstatus: %x\npayload(%i):\n",
        packet->mac, packet->status, packet->payload_size);
    for (int i = 0; i < strlen(packet->payload); i++) {
        printf(" %i(%c)", packet->payload[i], (char)packet->payload[i]);
    }
    printf("\n");
}

void print_body(const body_t* body) {
    printf("signature: %i\nsize: %i\n\n", body->version_signature, body->body_size);
    size_t temp_size = 0;
    for (int i = 0; i < body->chained; i++) {
        packet_t* temp1 = (packet_t*)((uint8_t*)&body->packet + temp_size);
        temp_size = temp1->payload_size + ((uint8_t*)&temp1->payload_size - (uint8_t*)&body->packet[0]) + 1;
        packet_t* temp = (packet_t*)malloc(temp_size);
        memcpy(temp, (uint8_t*)temp1, temp_size);
        print_packet(temp);
        free(temp);
    }
}

int main() {
    // static uint8_t payload[] =  {0x3e, 0xe4, 0x45, 0xfa, 0x00, 0x06, 'h', 'e', 'l', 'l', 'o', '\0'};
    // static uint8_t payload1[] = {0x3e, 0xe4, 0x45, 0xfa, 0x00, 0x06, 'w', 'o', 'r', 'l', 'd', '\0'};
    static uint8_t body[] = {
        0x5F, 6, 1, 5,
        0x3e, 0xe4, 0x45, 0xfa, 0x00, 0x06, 'w', 'h', 'e', 'r', 'e', '\0',
        0x12, 0x86, 0xe5, 0x07, 0x00, 0x03, 'i', 's', '\0',
        0x12, 0x82, 0xe5, 0x47, 0x00, 0x05, 'r', 'i', 'd', 'e', '\0',
        0x12, 0x82, 0xe5, 0x47, 0x00, 0x04, 't', 'h', 'e', '\0',
        0x12, 0x82, 0xe5, 0x47, 0x00, 0x0a, 'l', 'i', 'g', 'h', 't', 'n', 'i', 'n', 'g', '\0',
    };

    print_body((body_t*)&body);
    return 0;
}