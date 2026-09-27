#pragma once
#include <stdint.h>

// RGB565 is native endian throughout. Write in portrait row order so every
// destination cache line is touched contiguously, without a DMA scratch buffer.
inline bool writePanelTile(uint16_t *fb, int pw, int ph, bool flip,
                           int x, int y, int w, int h, const uint16_t *pixels)
{
    if (!fb || !pixels || w <= 0 || h <= 0 || x < 0 || y < 0 ||
        x > ph - w || y > pw - h) return false;
    for (int column = 0; column < w; ++column) {
        uint16_t *dst = fb + (flip ? ph - 1 - x - column : x + column) * pw
                            + (flip ? y : pw - y - h);
        for (int row = 0; row < h; ++row)
            dst[row] = pixels[(flip ? row : h - 1 - row) * w + column];
    }
    return true;
}

// Render w/2 x h/2 indexed source pixels (1, 2, 4 or 8 bits, big-endian bit
// order within a byte), doubled in both directions, straight into the
// portrait panel at landscape (x, y, w, h). `src` addresses the first source
// row and `first_pixel` is the tile's first source column. `pairs` maps an
// index to its RGB565 color replicated in both halfwords, so each source
// pixel becomes one aligned 32-bit store in each of its two portrait rows.
// The geometry matches writePanelTile for an equivalent pixel-doubled tile.
inline bool writePanelIndexedTile2x(uint16_t *fb, int pw, int ph, bool flip,
                                    int x, int y, int w, int h,
                                    const uint8_t *src, uint32_t stride,
                                    int bits, int first_pixel,
                                    const uint32_t *pairs)
{
    if (!fb || !src || !pairs || w <= 0 || h <= 0 || ((w | h | y | pw) & 1) ||
        (bits != 1 && bits != 2 && bits != 4 && bits != 8) || first_pixel < 0 ||
        x < 0 || y < 0 || x > ph - w || y > pw - h) return false;
    const int source_width = w / 2;
    const int source_height = h / 2;
    const int base = flip ? y : pw - y - h;
    const uint32_t mask = (1u << bits) - 1;
    for (int column = 0; column < source_width; ++column) {
        const int left = x + 2 * column;
        uint32_t *row0 = (uint32_t *)(fb + (flip ? ph - 1 - left : left) * pw + base);
        uint32_t *row1 = (uint32_t *)(fb + (flip ? ph - 2 - left : left + 1) * pw + base);
        const uint32_t bit = (uint32_t)(first_pixel + column) * (uint32_t)bits;
        const int shift = 8 - bits - (int)(bit & 7);
        const uint8_t *s = src + (bit >> 3) + (flip ? 0 : (source_height - 1) * stride);
        const intptr_t step = flip ? (intptr_t)stride : -(intptr_t)stride;
        for (int j = 0; j < source_height; ++j, s += step) {
            const uint32_t pair = pairs[(*s >> shift) & mask];
            row0[j] = pair;
            row1[j] = pair;
        }
    }
    return true;
}

inline uint16_t readPanelPixel(const uint16_t *fb, int pw, int ph, bool flip,
                              int x, int y)
{
    return flip ? fb[(ph - 1 - x) * pw + y] : fb[x * pw + pw - 1 - y];
}
