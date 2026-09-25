#pragma once
#include "cpu_emulation.h"
#include "guest_range.h"

inline bool AudioGuestRAMRange(uint32_t address, uint32_t length) {
    return RAMBaseHost && GuestRangeContains(address, length, RAMBaseMac, RAMSize);
}

inline const uint8_t *AudioGuestSamples(uint32_t address, uint32_t length) {
    if (AudioGuestRAMRange(address, length)) return RAMBaseHost + (address - RAMBaseMac);
    if (ROMBaseHost && GuestRangeContains(address, length, ROMBaseMac, ROMSize))
        return ROMBaseHost + (address - ROMBaseMac);
    return nullptr;
}
