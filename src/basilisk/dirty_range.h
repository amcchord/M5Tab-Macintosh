#pragma once
#include <stdint.h>

// Byte ranges may cross rows, packed pixels and any number of tiles. Padding
// bytes do not represent pixels. Use subtraction before addition to avoid wrap.
template<class Mark>
inline void markDirtyByteRange(uint32_t offset, uint32_t size,
                              uint32_t capacity, uint32_t stride,
                              uint32_t pixels_per_byte, uint32_t width,
                              uint32_t height, uint32_t tile_width,
                              uint32_t tile_height, Mark mark)
{
    if (!size || offset >= capacity || !stride) return;
    if (size > capacity - offset) size = capacity - offset;
    const uint32_t end = offset + size;
    const uint32_t columns = (width + tile_width - 1) / tile_width;
    while (offset < end) {
        const uint32_t y = offset / stride;
        if (y >= height) break;
        const uint32_t byte_x = offset % stride;
        const uint32_t remaining = end - offset;
        if (byte_x == 0 && remaining >= stride) {
            uint32_t rows = remaining / stride;
            if (rows > height - y) rows = height - y;
            const uint32_t tile_rows = tile_height - y % tile_height;
            if (rows > tile_rows) rows = tile_rows;
            for (uint32_t tx = 0; tx < columns; ++tx)
                mark((y / tile_height) * columns + tx);
            offset += rows * stride;
            continue;
        }
        const uint32_t count = remaining < stride - byte_x ? remaining : stride - byte_x;
        const uint32_t left = byte_x * pixels_per_byte;
        uint32_t right = (byte_x + count) * pixels_per_byte;
        if (right > width) right = width;
        if (left < right) {
            for (uint32_t tx = left / tile_width; tx <= (right - 1) / tile_width; ++tx)
                mark((y / tile_height) * columns + tx);
        }
        offset += count;
    }
}
