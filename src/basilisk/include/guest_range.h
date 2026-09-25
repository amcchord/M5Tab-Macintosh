#pragma once
#include <stdint.h>

// Validate the whole contiguous range, without wrapping address + length.
inline bool GuestRangeContains(uint32_t address, uint32_t length,
                               uint32_t base, uint32_t size) {
    if (address < base || length > size) return false;
    return address - base <= size - length;
}
