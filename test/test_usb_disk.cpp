#include "usb_disk_media.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <vector>

constexpr unsigned sector = 512, blocks = 32;
static std::array<uint8_t, sector * blocks> card;
static unsigned reads, writes;
static bool fail_read, fail_write;
DRESULT disk_read(BYTE drive, BYTE *buf, uint32_t lba, unsigned count)
{
    assert(drive == 3 && count && lba + count <= blocks);
    ++reads;
    if (fail_read) return RES_ERROR;
    memcpy(buf, &card[lba * sector], count * sector);
    return RES_OK;
}
DRESULT disk_write(BYTE drive, const BYTE *buf, uint32_t lba, unsigned count)
{
    assert(drive == 3 && count && lba + count <= blocks);
    ++writes;
    if (fail_write) return RES_ERROR;
    memcpy(&card[lba * sector], buf, count * sector);
    return RES_OK;
}
int main()
{
    UsbDiskMedia media;
    media.drive = 3;
    media.sector_size = sector;
    media.sectors = blocks;
    for (unsigned offset : {0u, 1u, 511u, 512u, 519u}) {
        for (unsigned size : {1u, 511u, 512u, 513u, 4096u}) {
            card.fill(0x5a);
            auto expected = card;
            std::vector<uint8_t> input(size), output(size);
            for (unsigned i = 0; i < size; ++i) input[i] = (13 * i + 1) & 255;
            std::copy(input.begin(), input.end(), expected.begin() + sector + offset);
            reads = writes = 0;
            assert(media.transfer(true, 1, offset, input.data(), size) == int(size));
            if (offset == 0 && size == 4096) assert(writes == 1 && reads == 0);
            assert(card == expected);
            reads = 0;
            assert(media.transfer(false, 1, offset, output.data(), size) == int(size));
            if (offset == 0 && size == 4096) assert(reads == 1);
            assert(output == input);
        }
    }
    uint8_t byte = 0;
    assert(media.transfer(true, blocks - 1, 511, &byte, 1) == 1);
    writes = reads = 0;
    assert(media.transfer(true, blocks - 1, 511, &byte, 2) == -1);
    assert(media.transfer(false, UINT32_MAX, UINT32_MAX, &byte, 1) == -1);
    assert(media.transfer(true, blocks, 0, nullptr, 0) == 0);
    assert(media.transfer(true, 0, 0, nullptr, 1) == -1);
    assert(writes == 0 && reads == 0);
    fail_read = true;
    assert(media.transfer(true, 0, 1, &byte, 1) == -1 && writes == 0);
    fail_read = false;
    fail_write = true;
    assert(media.transfer(true, 0, 1, &byte, 1) == -1);
}
