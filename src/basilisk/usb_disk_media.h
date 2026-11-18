// Byte ranges received by TinyUSB, backed by the mounted FatFs physical drive.
// One MSC task owns transfers and the scratch sector for partial requests.
#pragma once

#include <stdint.h>
#include <string.h>
#include "diskio.h"

struct UsbDiskMedia {
    BYTE drive = 0xff;
    uint16_t sector_size = 0;
    uint32_t sectors = 0;

    int32_t transfer(bool write, uint32_t lba, uint32_t offset, uint8_t *buffer,
                     uint32_t length)
    {
        if (drive == 0xff || !sector_size || sector_size > sizeof(scratch) ||
            length > INT32_MAX || (!buffer && length)) return -1;
        const uint64_t capacity = (uint64_t)sectors * sector_size;
        const uint64_t start = (uint64_t)lba * sector_size + offset;
        if (start > capacity || length > capacity - start) return -1;
        if (!length) return 0;

        lba = start / sector_size;
        offset = start % sector_size;
        uint32_t remaining = length;
        while (remaining) {
            if (offset == 0 && remaining >= sector_size) {
                const uint32_t count = remaining / sector_size;
                DRESULT result = write ? disk_write(drive, buffer, lba, count)
                                       : disk_read(drive, buffer, lba, count);
                if (result != RES_OK) return -1;
                const uint32_t bytes = count * sector_size;
                remaining -= bytes;
                buffer += bytes;
                lba += count;
            } else {
                uint32_t bytes = sector_size - offset;
                if (bytes > remaining) bytes = remaining;
                if (disk_read(drive, scratch, lba, 1) != RES_OK) return -1;
                if (write) {
                    memcpy(scratch + offset, buffer, bytes);
                    if (disk_write(drive, scratch, lba, 1) != RES_OK) return -1;
                } else {
                    memcpy(buffer, scratch + offset, bytes);
                }
                remaining -= bytes;
                buffer += bytes;
                ++lba;
                offset = 0;
            }
        }
        return length;
    }

private:
    alignas(4) BYTE scratch[FF_MAX_SS];
};
