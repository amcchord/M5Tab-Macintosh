// Classic HFS boot-volume handoff. System 7.6 and later can have the ROM
// skip its early scavenger and ask the loaded File Manager to check instead.
// This probe reads only the boot metadata and the blessed System's vers 1.
#ifdef DISK_IMAGE_HOST_TEST
#include "disk_image_test_stubs.h"
#else
#include "sysdeps.h"
#endif
#include "sys.h"

namespace {
uint16_t be16(const uint8_t *p) { return (uint16_t(p[0]) << 8) | p[1]; }
uint32_t be32(const uint8_t *p) { return (uint32_t(be16(p)) << 16) | be16(p + 2); }

struct Volume {
    void *fh;
    uint32_t start, size, allocation_start, block_size, blocks;

    bool read(uint32_t pos, void *out, size_t n) const {
        return pos <= size && n <= size - pos &&
               Sys_read(fh, out, start + pos, n) == n;
    }
};

// The first three HFS extents cover most system files. If a requested byte
// needs the overflow tree, leave the original mount procedure in control.
struct Fork {
    const Volume &volume;
    const uint8_t *extents;
    uint32_t size;

    bool read(uint32_t pos, uint8_t *out, size_t n) const {
        if (pos > size || n > size - pos) return false;
        for (unsigned i = 0; i < 3 && n; ++i) {
            uint32_t start = be16(extents + i * 4), blocks = be16(extents + i * 4 + 2);
            if (!blocks || start + blocks > volume.blocks) return false;
            uint32_t extent_size = blocks * volume.block_size;
            if (pos >= extent_size) {
                pos -= extent_size;
                continue;
            }
            size_t take = extent_size - pos;
            if (take > n) take = n;
            if (!volume.read(volume.allocation_start + start * volume.block_size + pos, out, take))
                return false;
            out += take;
            n -= take;
            pos = 0;
        }
        return !n;
    }
};

bool system_has_mount_check(const Fork &rsrc)
{
    uint8_t h[28];
    if (!rsrc.read(0, h, 16)) return false;
    uint32_t data = be32(h), map = be32(h + 4);
    uint32_t data_len = be32(h + 8), map_len = be32(h + 12);
    if (data < 16 || data > map || data_len > map - data ||
        map > rsrc.size || map_len > rsrc.size - map || map_len < 28 ||
        !rsrc.read(map, h, 28)) return false;
    uint32_t types = be16(h + 24);
    if (types < 28 || types > map_len - 2 || !rsrc.read(map + types, h, 2)) return false;
    uint32_t type_count = uint32_t(be16(h)) + 1;
    if (type_count > (map_len - types - 2) / 8 || type_count > 512) return false;
    for (uint32_t index = 0; index < type_count; ++index) {
        if (!rsrc.read(map + types + 2 + index * 8, h, 8)) return false;
        if (memcmp(h, "vers", 4)) continue;
        uint32_t refs = types + be16(h + 6), count = uint32_t(be16(h + 4)) + 1;
        if (refs < types + 2 + type_count * 8 || refs > map_len ||
            count > (map_len - refs) / 12 || count > 128) return false;
        for (uint32_t i = 0; i < count; ++i) {
            if (!rsrc.read(map + refs + i * 12, h, 12)) return false;
            if (be16(h) != 1) continue;
            uint32_t pos = be32(h + 4) & 0xffffff;
            if (pos > data_len || data_len - pos < 8 || !rsrc.read(data + pos, h, 8)) return false;
            uint32_t length = be32(h);
            if (length < 8 || length > data_len - pos - 4 || h[6] != 0x80) return false;
            // NumVersion uses BCD. These are the project's supported systems
            // which implement kHFSBootVolumeInconsistentBit (Apple hfs_format.h).
            return (h[4] == 7 && h[5] >= 0x60 && h[5] <= 0x61) ||
                   (h[4] == 8 && (h[5] == 0x00 || h[5] == 0x10));
        }
        return false;
    }
    return false;
}

bool check_blessed_system(const Fork &catalog, uint32_t folder, const uint8_t *name)
{
    uint8_t node[512];
    if (!catalog.read(0, node, sizeof(node)) || node[8] != 1 ||
        be16(node + 510) != 14 || be16(node + 32) != 512) return false;
    uint32_t first = be32(node + 24), nodes = be32(node + 36);
    if (!nodes || nodes > catalog.size / 512) return false;

    // Traditional HFS leaf nodes are 512 bytes and sorted by parent ID.
    // A bounded walk keeps an unfamiliar or damaged catalog off the boot path.
    uint32_t previous = 0;
    for (unsigned visits = 0; visits < 256 && first && first < nodes; ++visits) {
        if (!catalog.read(first * 512, node, sizeof(node)) ||
            node[8] != 0xff || node[9] != 1 || be32(node + 4) != previous) return false;
        uint16_t count = be16(node + 10);
        if (!count || count > 248) return false;
        for (unsigned i = 0; i < count; ++i) {
            unsigned begin = be16(node + 510 - i * 2), end = be16(node + 508 - i * 2);
            if (begin < 14 || end <= begin || end > 512 - 2 * (count + 1u) || end - begin < 8) return false;
            const uint8_t *key = node + begin;
            unsigned key_size = (unsigned(key[0]) + 2) & ~1u;
            if (key[6] > 31 || key[0] < 6 + key[6] || key_size + 1 > end - begin) return false;
            uint32_t parent = be32(key + 2);
            if (parent > folder) return false;
            if (parent != folder || key[6] != name[0] || memcmp(key + 7, name + 1, name[0])) continue;
            const uint8_t *record = key + key_size;
            if (end - begin < key_size + 102 || record[0] != 2 || memcmp(record + 4, "zsysMACS", 8))
                return false;
            return system_has_mount_check({catalog.volume, record + 86, be32(record + 36)});
        }
        previous = first;
        first = be32(node);
    }
    return false;
}
} // namespace

bool HfsCanDeferBootCheck(void *fh, loff_t start, uint32_t sectors)
{
    loff_t end = SysGetFileSize(fh);
    if (SysIsReadOnly(fh) || start < 0 || start > end || sectors < 5 ||
        sectors > uint32_t(end - start) / 512) return false;
    Volume volume = {fh, uint32_t(start), sectors * 512, 0, 0, 0};
    uint8_t mdb[162], other[162], boot[26];
    if (!volume.read(1024, mdb, sizeof(mdb)) || be16(mdb) != 0x4244 ||
        be16(mdb + 10) != 0 || be16(mdb + 124) == 0x482b) return false;

    volume.allocation_start = uint32_t(be16(mdb + 28)) * 512;
    volume.block_size = be32(mdb + 20);
    volume.blocks = be16(mdb + 18);
    uint32_t bitmap = uint32_t(be16(mdb + 14)) * 512;
    if (!volume.blocks || !volume.block_size || volume.block_size % 512 ||
        bitmap < 1536 || bitmap + (volume.blocks + 7) / 8 > volume.allocation_start ||
        uint64_t(volume.allocation_start) + uint64_t(volume.blocks) * volume.block_size > volume.size - 1024 ||
        be16(mdb + 34) > volume.blocks || !mdb[36] || mdb[36] > 27) return false;

    // The alternate MDB may be stale. Compare immutable volume identity and
    // geometry only; never copy its attributes or Finder information.
    if (!volume.read(volume.size - 1024, other, sizeof(other)) ||
        memcmp(other, mdb, 6) || be16(other + 10) != 0x0100 ||
        be16(other + 14) != be16(mdb + 14) || memcmp(other + 18, mdb + 18, 6) ||
        be16(other + 28) != be16(mdb + 28) || be16(other + 124) == 0x482b ||
        !volume.read(0, boot, sizeof(boot)) || be16(boot) != 0x4c4b ||
        !boot[10] || boot[10] > 15 || be32(mdb + 92) < 2) return false;

    return check_blessed_system({volume, mdb + 150, be32(mdb + 146)}, be32(mdb + 92), boot + 10);
}
