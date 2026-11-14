#include "flash.h"

#include <stddef.h>
#include <string.h>

#include <hardware/flash.h>
#include <pico/flash.h>

#define FLASH_PARAMS_MAGIC          0x50464C45u     // "ELFP"
#define FLASH_PARAMS_SECTOR_OFFSET  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)     // last sector of the flash
#define FLASH_PARAMS_SLOT_COUNT     (FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE)
#define FLASH_SAFE_TIMEOUT_MS       1000


typedef struct __attribute__((__packed__)) {
    uint32_t magic;
    uint32_t sequence;
    flash_params_t params;
    uint32_t crc;           // crc32 of everything above
} flash_record_t;

_Static_assert(sizeof(flash_record_t) <= FLASH_PAGE_SIZE, "record must fit into a flash page");

typedef struct {
    int newest;             // slot of the newest valid record, -1 if none
    int free;               // first erased slot, -1 if the log is full
} flash_scan_t;

typedef struct {
    const uint8_t* page;
    uint32_t slot_offset;
    bool erase;
} flash_write_request_t;

static uint32_t crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

static const uint8_t* slot_address(const int slot) {
    return (const uint8_t*)(XIP_BASE + FLASH_PARAMS_SECTOR_OFFSET + (uint32_t)slot * FLASH_PAGE_SIZE);
}

static bool slot_is_erased(const int slot) {
    const uint8_t* page = slot_address(slot);
    for (size_t i = 0; i < FLASH_PAGE_SIZE; ++i) {
        if (page[i] != 0xFF)
            return false;
    }
    return true;
}

static bool record_is_valid(const flash_record_t* record) {
    return record->magic == FLASH_PARAMS_MAGIC &&
           record->crc == crc32((const uint8_t*)record, offsetof(flash_record_t, crc));
}

static flash_scan_t scan_slots(void) {
    flash_scan_t scan = {.newest = -1, .free = -1};
    uint32_t newest_sequence = 0;

    for (int slot = 0; slot < FLASH_PARAMS_SLOT_COUNT; ++slot) {
        const flash_record_t* record = (const flash_record_t*)slot_address(slot);

        if (record_is_valid(record)) {
            // signed difference keeps the order correct even if the sequence wraps
            if (scan.newest < 0 || (int32_t)(record->sequence - newest_sequence) > 0) {
                scan.newest = slot;
                newest_sequence = record->sequence;
            }
        } else if (scan.free < 0 && slot_is_erased(slot)) {
            scan.free = slot;
        }
    }

    return scan;
}

static void write_callback(void* param) {
    const flash_write_request_t* request = param;

    if (request->erase)
        flash_range_erase(FLASH_PARAMS_SECTOR_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(request->slot_offset, request->page, FLASH_PAGE_SIZE);
}

bool flash_core_init(void) {
    return flash_safe_execute_core_init();
}

bool flash_params_load(flash_params_t* params) {
    if (params == NULL)
        return false;

    const flash_scan_t scan = scan_slots();
    if (scan.newest < 0)
        return false;

    memcpy(params, &((const flash_record_t*)slot_address(scan.newest))->params, sizeof(flash_params_t));
    return true;
}

bool flash_params_save(const flash_params_t* params) {
    if (params == NULL)
        return false;

    const flash_scan_t scan = scan_slots();
    uint32_t sequence = 1;

    if (scan.newest >= 0) {
        const flash_record_t* newest = (const flash_record_t*)slot_address(scan.newest);
        if (memcmp(&newest->params, params, sizeof(flash_params_t)) == 0)
            return true;    // nothing changed, spare the flash
        sequence = newest->sequence + 1;
    }

    const int slot = scan.free >= 0 ? scan.free : 0;

    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));

    flash_record_t record = {.magic = FLASH_PARAMS_MAGIC, .sequence = sequence, .params = *params};
    record.crc = crc32((const uint8_t*)&record, offsetof(flash_record_t, crc));
    memcpy(page, &record, sizeof(record));

    flash_write_request_t request = {
        .page = page,
        .slot_offset = FLASH_PARAMS_SECTOR_OFFSET + (uint32_t)slot * FLASH_PAGE_SIZE,
        .erase = scan.free < 0,
    };

    if (flash_safe_execute(write_callback, &request, FLASH_SAFE_TIMEOUT_MS) != PICO_OK)
        return false;

    const flash_record_t* written = (const flash_record_t*)slot_address(slot);
    return record_is_valid(written) && memcmp(&written->params, params, sizeof(flash_params_t)) == 0;
}

bool flash_network_load(network_t* network) {
    flash_params_t params;
    if (network == NULL || !flash_params_load(&params))
        return false;

    *network = params.network;
    return true;
}

bool flash_network_save(const network_t* network) {
    if (network == NULL)
        return false;

    flash_params_t params;
    memset(&params, 0, sizeof(params));
    flash_params_load(&params);

    params.network = *network;
    return flash_params_save(&params);
}
