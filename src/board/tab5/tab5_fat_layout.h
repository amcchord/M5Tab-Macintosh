// FAT BPB and MBR fields used for card inspection and post-format readback.
#pragma once
#include <cstdint>
#include <cstring>

namespace Tab5Fat {
inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
inline uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }

inline bool signature(const uint8_t *b) { return b[510] == 0x55 && b[511] == 0xaa; }

inline uint64_t volumeBytes(const uint8_t *b) {
    if (!memcmp(b + 3, "EXFAT   ", 8))
        return (le32(b + 72) | ((uint64_t)le32(b + 76) << 32)) * 512;
    return (uint64_t)(le16(b + 19) ? le16(b + 19) : le32(b + 32)) * 512;
}

inline const char *volume(const uint8_t *b, uint32_t extent, uint32_t &cluster) {
    cluster = 0;
    if (!signature(b)) return nullptr;
    if (!memcmp(b + 3, "EXFAT   ", 8)) {
        const uint64_t sectors = le32(b + 72) | ((uint64_t)le32(b + 76) << 32);
        if (b[108] != 9 || b[109] > 16 || !sectors || sectors > extent) return nullptr;
        cluster = 512U << b[109];
        return "exFAT";
    }
    const unsigned spc = b[13], reserved = le16(b + 14), fats = b[16];
    const uint32_t sectors = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    const uint32_t fat = le16(b + 22) ? le16(b + 22) : le32(b + 36);
    const uint64_t first = reserved + (uint64_t)fats * fat + (le16(b + 17) * 32U + 511) / 512;
    if (le16(b + 11) != 512 || !spc || spc > 128 || (spc & (spc - 1)) ||
        !reserved || (fats != 1 && fats != 2) || !fat || sectors > extent || sectors <= first)
        return nullptr;
    const uint32_t clusters = (sectors - first) / spc;
    if (!clusters) return nullptr;
    if (clusters >= 65525 && (le16(b + 17) || le16(b + 22) || le32(b + 44) < 2)) return nullptr;
    cluster = 512 * spc;
    return clusters < 4085 ? "FAT12" : clusters < 65525 ? "FAT16" : "FAT32";
}

// At least 70,000 prospective clusters leaves room for two FATs and the MBR.
// A 2 GiB card gets 16 KiB clusters; larger cards can use 32 KiB clusters.
inline unsigned allocationUnit(uint32_t sectors) {
    if (sectors < 64 * 2048) return 0;
    unsigned bytes = 32768;
    while (bytes > 512 && (uint64_t)(sectors - 2048) * 512 < uint64_t{70000} * bytes)
        bytes /= 2;
    return bytes;
}
}
