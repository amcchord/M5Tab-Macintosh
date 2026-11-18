#pragma once
#include <stdint.h>
using BYTE = uint8_t;
enum DRESULT { RES_OK, RES_ERROR };
constexpr unsigned FF_MAX_SS = 4096;
DRESULT disk_read(BYTE drive, BYTE *buffer, uint32_t lba, unsigned count);
DRESULT disk_write(BYTE drive, const BYTE *buffer, uint32_t lba, unsigned count);
