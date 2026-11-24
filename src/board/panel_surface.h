#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

namespace panel_surface_detail {

// Sized for the 32x40 tile grid in board_config.h so production tiles finish in
// a single pass while arbitrarily large tiles chunk cleanly on stack.
static constexpr int kChunkW = 32;
static constexpr int kChunkH = 40;

union TileStage {
    uint32_t bits[kChunkH];
    uint8_t bytes[kChunkH * kChunkW];
};

#if defined(__GNUC__)
#define PANEL_SURFACE_ALWAYS_INLINE __attribute__((always_inline)) inline
#define PANEL_SURFACE_NOINLINE __attribute__((noinline))
#else
#define PANEL_SURFACE_ALWAYS_INLINE inline
#define PANEL_SURFACE_NOINLINE
#endif

static inline uint32_t read_u32_aligned(const void *src)
{
    typedef uint32_t __attribute__((__may_alias__)) u32_alias;
    return *static_cast<const u32_alias *>(__builtin_assume_aligned(src, 4));
}

static inline void write_u32_aligned(void *dst, uint32_t val)
{
    typedef uint32_t __attribute__((__may_alias__)) u32_alias;
    *static_cast<u32_alias *>(__builtin_assume_aligned(dst, 4)) = val;
}

static PANEL_SURFACE_ALWAYS_INLINE void emit_u32_row(uintptr_t dst_addr, const uint32_t *buf, int n)
{
    uint32_t *dst = reinterpret_cast<uint32_t *>(dst_addr);
    int j = 0;
    for (; j + 4 <= n; j += 4) {
        const uint32_t p0 = buf[j];
        const uint32_t p1 = buf[j + 1];
        const uint32_t p2 = buf[j + 2];
        const uint32_t p3 = buf[j + 3];
        write_u32_aligned(dst + j,     p0);
        write_u32_aligned(dst + j + 1, p1);
        write_u32_aligned(dst + j + 2, p2);
        write_u32_aligned(dst + j + 3, p3);
    }
    for (; j < n; ++j) {
        write_u32_aligned(dst + j, buf[j]);
    }
}

static inline void emit_u16_row(uintptr_t dst_addr, const uint16_t *buf, int n)
{
    uint16_t *dst = reinterpret_cast<uint16_t *>(dst_addr);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if ((dst_addr & 1u) == 0 && n >= 2) {
        int j = 0;
        if ((dst_addr & 2u) != 0) {
            dst[0] = buf[0];
            dst += 1;
            ++j;
        }
        uint32_t *dst32 = reinterpret_cast<uint32_t *>(dst);
        int k = 0;
        for (; j + 8 <= n; j += 8, k += 4) {
            const uint32_t w0 = (uint32_t)buf[j]     | ((uint32_t)buf[j + 1] << 16);
            const uint32_t w1 = (uint32_t)buf[j + 2] | ((uint32_t)buf[j + 3] << 16);
            const uint32_t w2 = (uint32_t)buf[j + 4] | ((uint32_t)buf[j + 5] << 16);
            const uint32_t w3 = (uint32_t)buf[j + 6] | ((uint32_t)buf[j + 7] << 16);
            write_u32_aligned(dst32 + k,     w0);
            write_u32_aligned(dst32 + k + 1, w1);
            write_u32_aligned(dst32 + k + 2, w2);
            write_u32_aligned(dst32 + k + 3, w3);
        }
        for (; j + 2 <= n; j += 2, ++k) {
            const uint32_t w = (uint32_t)buf[j] | ((uint32_t)buf[j + 1] << 16);
            write_u32_aligned(dst32 + k, w);
        }
        if (j < n) {
            dst[k * 2] = buf[j];
        }
        return;
    }
#endif
    for (int j = 0; j < n; ++j) {
        dst[j] = buf[j];
    }
}

template <int Scale> struct ScaleTraits;

template <> struct ScaleTraits<2> {
    typedef uint32_t PixelOut;
    static inline void emit(uintptr_t row_addr, ptrdiff_t row_step_bytes,
                            const uint32_t *buf, int n)
    {
        emit_u32_row(row_addr, buf, n);
        emit_u32_row(row_addr + row_step_bytes, buf, n);
    }
    static inline void emit1(uintptr_t row_addr, ptrdiff_t row_step_bytes, uint32_t val)
    {
        write_u32_aligned(reinterpret_cast<void *>(row_addr), val);
        write_u32_aligned(reinterpret_cast<void *>(row_addr + row_step_bytes), val);
    }
};

template <> struct ScaleTraits<1> {
    typedef uint16_t PixelOut;
    static inline void emit(uintptr_t row_addr, ptrdiff_t,
                            const uint16_t *buf, int n)
    {
        emit_u16_row(row_addr, buf, n);
    }
    static inline void emit1(uintptr_t row_addr, ptrdiff_t, uint16_t val)
    {
        *reinterpret_cast<uint16_t *>(row_addr) = val;
    }
};

template <int Scale>
static inline void writeSingleRowIndexed(uint16_t *fb, int pw, int ph, bool flip,
                                         int x, int base, int total_sw,
                                         const uint8_t *src, int bits,
                                         int first_pixel, const uint32_t *pairs)
{
    typedef typename ScaleTraits<Scale>::PixelOut PixelOut;
    const ptrdiff_t row_step_bytes = (flip ? -(ptrdiff_t)pw : (ptrdiff_t)pw) * (ptrdiff_t)sizeof(uint16_t);
    const ptrdiff_t col_step_bytes = row_step_bytes * Scale;
    const int start_py = flip ? (ph - 1 - x) : x;
    uintptr_t row_addr = reinterpret_cast<uintptr_t>(fb + (size_t)start_py * pw + base);

    switch (bits) {
        case 1: {
            const PixelOut c0 = (PixelOut)pairs[0];
            const PixelOut c1 = (PixelOut)pairs[1];
            const PixelOut xor_c = c0 ^ c1;
            const uint8_t *s = src + ((size_t)first_pixel >> 3);
            int rem = total_sw;
            const int bit_off = first_pixel & 7;
            if (bit_off != 0) {
                const uint8_t b = *s++;
                const int take = (8 - bit_off) < rem ? (8 - bit_off) : rem;
                for (int i = 0; i < take; ++i) {
                    const uint32_t bit = (b >> (7 - bit_off - i)) & 1u;
                    ScaleTraits<Scale>::emit1(row_addr, row_step_bytes, c0 ^ (xor_c & (PixelOut)(0u - bit)));
                    row_addr += col_step_bytes;
                }
                rem -= take;
            }
            for (; rem >= 8; rem -= 8) {
                const uint8_t b = *s++;
                for (int sh = 7; sh >= 0; --sh) {
                    const uint32_t bit = (b >> sh) & 1u;
                    ScaleTraits<Scale>::emit1(row_addr, row_step_bytes, c0 ^ (xor_c & (PixelOut)(0u - bit)));
                    row_addr += col_step_bytes;
                }
            }
            if (rem > 0) {
                const uint8_t b = *s;
                for (int i = 0; i < rem; ++i) {
                    const uint32_t bit = (b >> (7 - i)) & 1u;
                    ScaleTraits<Scale>::emit1(row_addr, row_step_bytes, c0 ^ (xor_c & (PixelOut)(0u - bit)));
                    row_addr += col_step_bytes;
                }
            }
            break;
        }
        case 2: {
            const uint8_t *s = src + ((size_t)first_pixel >> 2);
            int rem = total_sw;
            const int px_off = first_pixel & 3;
            if (px_off != 0) {
                const uint8_t b = *s++;
                const int take = (4 - px_off) < rem ? (4 - px_off) : rem;
                for (int i = 0; i < take; ++i) {
                    ScaleTraits<Scale>::emit1(row_addr, row_step_bytes,
                                              (PixelOut)pairs[(b >> (6 - ((px_off + i) << 1))) & 3u]);
                    row_addr += col_step_bytes;
                }
                rem -= take;
            }
            for (; rem >= 4; rem -= 4) {
                const uint8_t b = *s++;
                ScaleTraits<Scale>::emit1(row_addr, row_step_bytes, (PixelOut)pairs[b >> 6]);
                row_addr += col_step_bytes;
                ScaleTraits<Scale>::emit1(row_addr, row_step_bytes, (PixelOut)pairs[(b >> 4) & 3u]);
                row_addr += col_step_bytes;
                ScaleTraits<Scale>::emit1(row_addr, row_step_bytes, (PixelOut)pairs[(b >> 2) & 3u]);
                row_addr += col_step_bytes;
                ScaleTraits<Scale>::emit1(row_addr, row_step_bytes, (PixelOut)pairs[b & 3u]);
                row_addr += col_step_bytes;
            }
            if (rem > 0) {
                const uint8_t b = *s;
                for (int i = 0; i < rem; ++i) {
                    ScaleTraits<Scale>::emit1(row_addr, row_step_bytes,
                                              (PixelOut)pairs[(b >> (6 - (i << 1))) & 3u]);
                    row_addr += col_step_bytes;
                }
            }
            break;
        }
        case 4: {
            const uint8_t *s = src + ((size_t)first_pixel >> 1);
            int rem = total_sw;
            if (first_pixel & 1) {
                ScaleTraits<Scale>::emit1(row_addr, row_step_bytes, (PixelOut)pairs[*s++ & 0x0Fu]);
                row_addr += col_step_bytes;
                --rem;
            }
            for (; rem >= 2; rem -= 2) {
                const uint8_t b = *s++;
                ScaleTraits<Scale>::emit1(row_addr, row_step_bytes, (PixelOut)pairs[b >> 4]);
                row_addr += col_step_bytes;
                ScaleTraits<Scale>::emit1(row_addr, row_step_bytes, (PixelOut)pairs[b & 0x0Fu]);
                row_addr += col_step_bytes;
            }
            if (rem > 0) {
                ScaleTraits<Scale>::emit1(row_addr, row_step_bytes, (PixelOut)pairs[*s >> 4]);
            }
            break;
        }
        case 8: {
            const uint8_t *s = src + (size_t)first_pixel;
            for (int c = 0; c < total_sw; ++c) {
                ScaleTraits<Scale>::emit1(row_addr, row_step_bytes, (PixelOut)pairs[s[c]]);
                row_addr += col_step_bytes;
            }
            break;
        }
    }
}

static inline void stage1bpp(uint32_t *bits_row, const uint8_t *src, uint32_t stride,
                             int total_sh, int j0, int chunk_h, bool flip,
                             size_t col_px, int chunk_w)
{
    const size_t first_byte = col_px >> 3;
    const int bit_off = (int)(col_px & 7);
    const int need_bytes = (bit_off + chunk_w + 7) >> 3;
    const int r_top = flip ? j0 : (total_sh - j0 - chunk_h);
    const uint8_t *src_row = src + (size_t)r_top * stride + first_byte;
    int k = flip ? 0 : (chunk_h - 1);
    const int k_step = flip ? 1 : -1;

    if (bit_off == 0 && chunk_w == 32) {
        if (((uintptr_t)src_row & 3u) == 0 && (stride & 3u) == 0) {
            for (int r = 0; ; ) {
                uint32_t v = read_u32_aligned(src_row);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                v = ((v & 0x000000FFu) << 24) |
                    ((v & 0x0000FF00u) << 8)  |
                    ((v & 0x00FF0000u) >> 8)  |
                    (v >> 24);
#endif
                bits_row[k] = v;
                if (++r == chunk_h) break;
                src_row += stride;
                k += k_step;
            }
        } else {
            for (int r = 0; ; ) {
                bits_row[k] = ((uint32_t)src_row[0] << 24) |
                              ((uint32_t)src_row[1] << 16) |
                              ((uint32_t)src_row[2] << 8)  |
                               (uint32_t)src_row[3];
                if (++r == chunk_h) break;
                src_row += stride;
                k += k_step;
            }
        }
        return;
    }

    for (int r = 0; ; ) {
        uint32_t w32 = (uint32_t)src_row[0] << 24;
        if (need_bytes >= 2) w32 |= (uint32_t)src_row[1] << 16;
        if (need_bytes >= 3) w32 |= (uint32_t)src_row[2] << 8;
        if (need_bytes >= 4) w32 |= (uint32_t)src_row[3];
        if (bit_off) {
            w32 <<= bit_off;
            if (need_bytes == 5) w32 |= (uint32_t)src_row[4] >> (8 - bit_off);
        }
        bits_row[k] = w32;
        if (++r == chunk_h) break;
        src_row += stride;
        k += k_step;
    }
}

static inline void copy_row_u32(uint8_t *dst_row, const uint8_t *src_row, int words)
{
    for (int i = 0; i < words; ++i) {
        write_u32_aligned(dst_row + i * 4, read_u32_aligned(src_row + i * 4));
    }
}

static inline void stageBytes(uint8_t *staged, int stage_stride,
                              const uint8_t *src, uint32_t stride,
                              int total_sh, int j0, int chunk_h, bool flip,
                              size_t first_byte, int need_bytes)
{
    const int r_top = flip ? j0 : (total_sh - j0 - chunk_h);
    const uint8_t *src_row = src + (size_t)r_top * stride + first_byte;
    uint8_t *dst_row = staged + (flip ? 0 : (chunk_h - 1) * stage_stride);
    const int dst_step = flip ? stage_stride : -stage_stride;
    const bool aligned4 = (((uintptr_t)src_row & 3u) == 0 && (stride & 3u) == 0);

    if (need_bytes == 32 && aligned4) {
        for (int r = 0; ; ) {
            copy_row_u32(dst_row, src_row, 8);
            if (++r == chunk_h) break;
            src_row += stride;
            dst_row += dst_step;
        }
    } else if (need_bytes == 16 && aligned4) {
        for (int r = 0; ; ) {
            copy_row_u32(dst_row, src_row, 4);
            if (++r == chunk_h) break;
            src_row += stride;
            dst_row += dst_step;
        }
    } else if (need_bytes == 8 && aligned4) {
        for (int r = 0; ; ) {
            copy_row_u32(dst_row, src_row, 2);
            if (++r == chunk_h) break;
            src_row += stride;
            dst_row += dst_step;
        }
    } else {
        for (int r = 0; ; ) {
            memcpy(__builtin_assume_aligned(dst_row, 4), src_row, (size_t)need_bytes);
            if (++r == chunk_h) break;
            src_row += stride;
            dst_row += dst_step;
        }
    }
}

template <int Scale>
inline bool writePanelIndexedTileImpl(uint16_t *fb, int pw, int ph, bool flip,
                                      int x, int y, int w, int h,
                                      const uint8_t *src, uint32_t stride,
                                      int bits, int first_pixel,
                                      const uint32_t *pairs)
{
    typedef typename ScaleTraits<Scale>::PixelOut PixelOut;
    const int total_sw = w / Scale;
    const int total_sh = h / Scale;
    const int base = flip ? y : pw - y - h;
    if (total_sh == 1) {
        writeSingleRowIndexed<Scale>(fb, pw, ph, flip, x, base, total_sw,
                                     src, bits, first_pixel, pairs);
        return true;
    }

    const ptrdiff_t row_step_bytes = (flip ? -(ptrdiff_t)pw : (ptrdiff_t)pw) * (ptrdiff_t)sizeof(uint16_t);
    const ptrdiff_t col_step_bytes = row_step_bytes * Scale;

    TileStage stage;
    PixelOut col_buf[kChunkH];

    for (int j0 = 0; j0 < total_sh; j0 += kChunkH) {
        const int rem_h = total_sh - j0;
        const int chunk_h = rem_h < kChunkH ? rem_h : kChunkH;
        const size_t dst_col_off = (size_t)base + (size_t)j0 * Scale;

        for (int cx = 0; cx < total_sw; cx += kChunkW) {
            const int rem_w = total_sw - cx;
            const int chunk_w = rem_w < kChunkW ? rem_w : kChunkW;
            const size_t col_px = (size_t)first_pixel + (size_t)cx;
            const int start_py = flip ? (ph - 1 - (x + cx * Scale)) : (x + cx * Scale);
            uintptr_t row_addr = reinterpret_cast<uintptr_t>(
                fb + (size_t)start_py * pw + dst_col_off);

            switch (bits) {
                case 1: {
                    const PixelOut c0_pair = (PixelOut)pairs[0];
                    const PixelOut c1_pair = (PixelOut)pairs[1];
                    const PixelOut xor_pair = c0_pair ^ c1_pair;
                    stage1bpp(stage.bits, src, stride, total_sh, j0, chunk_h,
                              flip, col_px, chunk_w);
                    for (int c = 0; c < chunk_w; ++c) {
                        const int shift = 31 - c;
                        int j = 0;
                        for (; j + 4 <= chunk_h; j += 4) {
                            const uint32_t b0 = (stage.bits[j]     >> shift) & 1u;
                            const uint32_t b1 = (stage.bits[j + 1] >> shift) & 1u;
                            const uint32_t b2 = (stage.bits[j + 2] >> shift) & 1u;
                            const uint32_t b3 = (stage.bits[j + 3] >> shift) & 1u;
                            col_buf[j]     = c0_pair ^ (xor_pair & (PixelOut)(0u - b0));
                            col_buf[j + 1] = c0_pair ^ (xor_pair & (PixelOut)(0u - b1));
                            col_buf[j + 2] = c0_pair ^ (xor_pair & (PixelOut)(0u - b2));
                            col_buf[j + 3] = c0_pair ^ (xor_pair & (PixelOut)(0u - b3));
                        }
                        for (; j < chunk_h; ++j) {
                            const uint32_t b = (stage.bits[j] >> shift) & 1u;
                            col_buf[j] = c0_pair ^ (xor_pair & (PixelOut)(0u - b));
                        }
                        ScaleTraits<Scale>::emit(row_addr, row_step_bytes, col_buf, chunk_h);
                        row_addr += col_step_bytes;
                    }
                    break;
                }
                case 2: {
                    constexpr int kStageStride = 16;
                    const int px_off = (int)(col_px & 3);
                    const int need_bytes = (px_off + chunk_w + 3) >> 2;
                    stageBytes(stage.bytes, kStageStride, src, stride, total_sh,
                               j0, chunk_h, flip, col_px >> 2, need_bytes);
                    for (int c = 0; c < chunk_w; ++c) {
                        const int p = px_off + c;
                        const int shift = 6 - ((p & 3) << 1);
                        const uint8_t *sp = stage.bytes + (p >> 2);
                        int j = 0;
                        for (; j + 4 <= chunk_h; j += 4, sp += kStageStride * 4) {
                            col_buf[j]     = (PixelOut)pairs[(sp[0] >> shift) & 3u];
                            col_buf[j + 1] = (PixelOut)pairs[(sp[kStageStride] >> shift) & 3u];
                            col_buf[j + 2] = (PixelOut)pairs[(sp[kStageStride * 2] >> shift) & 3u];
                            col_buf[j + 3] = (PixelOut)pairs[(sp[kStageStride * 3] >> shift) & 3u];
                        }
                        for (; j < chunk_h; ++j, sp += kStageStride)
                            col_buf[j] = (PixelOut)pairs[(*sp >> shift) & 3u];
                        ScaleTraits<Scale>::emit(row_addr, row_step_bytes, col_buf, chunk_h);
                        row_addr += col_step_bytes;
                    }
                    break;
                }
                case 4: {
                    constexpr int kStageStride = 20;
                    const int odd_start = (int)(col_px & 1);
                    const int need_bytes = (odd_start + chunk_w + 1) >> 1;
                    stageBytes(stage.bytes, kStageStride, src, stride, total_sh,
                               j0, chunk_h, flip, col_px >> 1, need_bytes);
                    for (int c = 0; c < chunk_w; ++c) {
                        const int p = odd_start + c;
                        const uint8_t *sp = stage.bytes + (p >> 1);
                        int j = 0;
                        if ((p & 1) == 0) {
                            for (; j + 4 <= chunk_h; j += 4, sp += kStageStride * 4) {
                                col_buf[j]     = (PixelOut)pairs[sp[0] >> 4];
                                col_buf[j + 1] = (PixelOut)pairs[sp[kStageStride] >> 4];
                                col_buf[j + 2] = (PixelOut)pairs[sp[kStageStride * 2] >> 4];
                                col_buf[j + 3] = (PixelOut)pairs[sp[kStageStride * 3] >> 4];
                            }
                            for (; j < chunk_h; ++j, sp += kStageStride)
                                col_buf[j] = (PixelOut)pairs[*sp >> 4];
                        } else {
                            for (; j + 4 <= chunk_h; j += 4, sp += kStageStride * 4) {
                                col_buf[j]     = (PixelOut)pairs[sp[0] & 0x0Fu];
                                col_buf[j + 1] = (PixelOut)pairs[sp[kStageStride] & 0x0Fu];
                                col_buf[j + 2] = (PixelOut)pairs[sp[kStageStride * 2] & 0x0Fu];
                                col_buf[j + 3] = (PixelOut)pairs[sp[kStageStride * 3] & 0x0Fu];
                            }
                            for (; j < chunk_h; ++j, sp += kStageStride)
                                col_buf[j] = (PixelOut)pairs[*sp & 0x0Fu];
                        }
                        ScaleTraits<Scale>::emit(row_addr, row_step_bytes, col_buf, chunk_h);
                        row_addr += col_step_bytes;
                    }
                    break;
                }
                case 8: {
                    constexpr int kStageStride = 32;
                    stageBytes(stage.bytes, kStageStride, src, stride, total_sh,
                               j0, chunk_h, flip, col_px, chunk_w);
                    for (int c = 0; c < chunk_w; ++c) {
                        const uint8_t *sp = stage.bytes + c;
                        int j = 0;
                        for (; j + 4 <= chunk_h; j += 4, sp += kStageStride * 4) {
                            col_buf[j]     = (PixelOut)pairs[sp[0]];
                            col_buf[j + 1] = (PixelOut)pairs[sp[kStageStride]];
                            col_buf[j + 2] = (PixelOut)pairs[sp[kStageStride * 2]];
                            col_buf[j + 3] = (PixelOut)pairs[sp[kStageStride * 3]];
                        }
                        for (; j < chunk_h; ++j, sp += kStageStride)
                            col_buf[j] = (PixelOut)pairs[*sp];
                        ScaleTraits<Scale>::emit(row_addr, row_step_bytes, col_buf, chunk_h);
                        row_addr += col_step_bytes;
                    }
                    break;
                }
            }
        }
    }
    return true;
}

static PANEL_SURFACE_NOINLINE bool writePanelIndexedTile2x_unaligned(
    uint16_t *fb, int pw, int ph, bool flip,
    int x, int y, int w, int h,
    const uint8_t *src, uint32_t stride,
    int bits, int first_pixel, const uint32_t *pairs)
{
    const int source_width = w / 2;
    const int source_height = h / 2;
    const int base = flip ? y : pw - y - h;
    const uint32_t mask = (1u << bits) - 1;
    const int bits_log2 = (bits == 1) ? 0 : (bits == 2) ? 1 : (bits == 4) ? 2 : 3;
    for (int column = 0; column < source_width; ++column) {
        const int left = x + 2 * column;
        uint16_t *row0 = fb + (size_t)(flip ? ph - 1 - left : left) * pw + base;
        uint16_t *row1 = fb + (size_t)(flip ? ph - 2 - left : left + 1) * pw + base;
        const size_t bit = ((size_t)first_pixel + (size_t)column) << bits_log2;
        const int shift = 8 - bits - (int)(bit & 7);
        const uint8_t *s = src + (bit >> 3) + (flip ? 0 : (size_t)(source_height - 1) * stride);
        const ptrdiff_t step = flip ? (ptrdiff_t)stride : -(ptrdiff_t)stride;
        for (int j = 0; j < source_height; ++j) {
            const uint32_t pair = pairs[(*s >> shift) & mask];
            memcpy(row0 + j * 2, &pair, 4);
            memcpy(row1 + j * 2, &pair, 4);
            if (j + 1 < source_height) s += step;
        }
    }
    return true;
}

} // namespace panel_surface_detail

// Render w/2 x h/2 indexed source pixels (1, 2, 4 or 8 bits, big-endian bit
// order within a byte), doubled in both directions, straight into the
// portrait panel at landscape (x, y, w, h). `src` addresses the first source
// row and `first_pixel` is the tile's first source column. `pairs` maps an
// index to its RGB565 color replicated in both halfwords, so each source
// pixel becomes one aligned 32-bit store in each of its two portrait rows.
// The source pixel range must fit within stride. The geometry matches
// writePanelTile for an equivalent pixel-doubled tile.
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
    const int bits_log2 = (bits == 1) ? 0 : (bits == 2) ? 1 : (bits == 4) ? 2 : 3;
    if ((((uint64_t)first_pixel + (uint64_t)source_width) << bits_log2) >
        ((uint64_t)stride << 3))
        return false;
    if (__builtin_expect(((uintptr_t)fb & 3u) != 0, 0)) {
        return panel_surface_detail::writePanelIndexedTile2x_unaligned(
            fb, pw, ph, flip, x, y, w, h, src, stride, bits, first_pixel, pairs);
    }
    return panel_surface_detail::writePanelIndexedTileImpl<2>(
        fb, pw, ph, flip, x, y, w, h, src, stride, bits, first_pixel, pairs);
}

// One-to-one indexed tile write: render w x h indexed source pixels (1, 2, 4
// or 8 bits, big-endian bit order within a byte) straight into the portrait
// panel at landscape (x, y, w, h). `src` addresses the first source row and
// `first_pixel` is the tile's first source column. `pairs` maps an index to
// its RGB565 color in the low halfword, so a replicated pair table
// (color * 0x00010001u) works unchanged. The source pixel range must fit
// within stride.
inline bool writePanelIndexedTile1x(uint16_t *fb, int pw, int ph, bool flip,
                                    int x, int y, int w, int h,
                                    const uint8_t *src, uint32_t stride,
                                    int bits, int first_pixel,
                                    const uint32_t *pairs)
{
    if (!fb || !src || !pairs || w <= 0 || h <= 0 ||
        (bits != 1 && bits != 2 && bits != 4 && bits != 8) || first_pixel < 0 ||
        x < 0 || y < 0 || x > ph - w || y > pw - h) return false;
    const int bits_log2 = (bits == 1) ? 0 : (bits == 2) ? 1 : (bits == 4) ? 2 : 3;
    if ((((uint64_t)first_pixel + (uint64_t)w) << bits_log2) >
        ((uint64_t)stride << 3))
        return false;
    return panel_surface_detail::writePanelIndexedTileImpl<1>(
        fb, pw, ph, flip, x, y, w, h, src, stride, bits, first_pixel, pairs);
}

inline uint16_t readPanelPixel(const uint16_t *fb, int pw, int ph, bool flip,
                              int x, int y)
{
    return flip ? fb[(ph - 1 - x) * pw + y] : fb[x * pw + pw - 1 - y];
}
