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

inline uint16_t readPanelPixel(const uint16_t *fb, int pw, int ph, bool flip,
                              int x, int y)
{
    return flip ? fb[(ph - 1 - x) * pw + y] : fb[x * pw + pw - 1 - y];
}
