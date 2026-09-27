/*
 * mini_gfx.cpp - tiny RGB565 text/rect/line renderer for the Waveshare
 *                board's boot GUI.
 *
 * Coordinate system:
 *   - "Logical" coordinates are landscape (logical_w x logical_h).
 *   - "Physical" panel coordinates are portrait (panel_w x panel_h).
 *   - Default rotation is 90 degrees CW scan:
 *       logical (lx, ly)  ==>  physical (panel_w - 1 - ly, lx)
 *     - logical (0, 0)          -> physical (panel_w - 1, 0)          -- top-right
 *     - logical (logical_w-1,0) -> physical (panel_w - 1, logical_w-1)-- bottom-right
 *     - logical (0, logical_h-1)-> physical (0, 0)
 *   - When setFlip180(true) is active the landscape view is rotated 180
 *     degrees on the panel, which is equivalent to a 90 CCW scan:
 *       logical (lx, ly)  ==>  physical (ly, panel_h - 1 - lx)
 *     - logical (0, 0)          -> physical (0, panel_h - 1)          -- bottom-left
 *     - logical (logical_w-1,0) -> physical (0, 0)                    -- top-left
 *
 *   With logical_w == panel_h and logical_h == panel_w (typical for a
 *   90-degree rotation), both axes map 1:1 in either orientation.
 */

#include "mini_gfx.h"
#include "mini_gfx_font.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_cache.h"

static const char *TAG = "mini_gfx";

MiniGfx::MiniGfx() = default;

bool MiniGfx::beginExternalFb(void *external_fb,
                              int logical_w, int logical_h,
                              int panel_w,   int panel_h)
{
    if (!external_fb || logical_w != panel_h || logical_h != panel_w ||
        panel_w <= 0 || panel_h <= 0) {
        ESP_LOGE(TAG, "beginExternalFb: invalid framebuffer or geometry");
        return false;
    }
    _lw = logical_w;
    _lh = logical_h;
    _pw = panel_w;
    _ph = panel_h;
    _fb = static_cast<uint16_t *>(external_fb);
    /* Start from a known state so early reads see a clean canvas. */
    const size_t n = static_cast<size_t>(_pw) * static_cast<size_t>(_ph);
    for (size_t i = 0; i < n; ++i) _fb[i] = 0;
    __atomic_store_n(&_dirty, true, __ATOMIC_RELEASE);
    return true;
}

inline void MiniGfx::writeLogicalPixel(int lx, int ly, uint16_t color)
{
    if (lx < 0 || lx >= _lw || ly < 0 || ly >= _lh) return;
    int px, py;
    if (_flip180) {
        /* 180-degree flipped landscape: (lx, ly) -> (ly, _ph - 1 - lx). */
        px = ly;
        py = _ph - 1 - lx;
    } else {
        px = _pw - 1 - ly;
        py = lx;
    }
    _fb[py * _pw + px] = color;
    __atomic_store_n(&_dirty, true, __ATOMIC_RELEASE);
}

void MiniGfx::fillLogicalRect(int lx, int ly, int lw, int lh, uint16_t color)
{
    if (lw <= 0 || lh <= 0) return;
    if (lx < 0) { lw += lx; lx = 0; }
    if (ly < 0) { lh += ly; ly = 0; }
    if (lx + lw > _lw) lw = _lw - lx;
    if (ly + lh > _lh) lh = _lh - ly;
    if (lw <= 0 || lh <= 0) return;

    /* In portrait memory, a logical rectangle (lx..lx+lw, ly..ly+lh) maps
     * to physical columns/rows depending on which 90-degree rotation is
     * active. The default (CW) mapping makes logical x increase downward
     * in portrait; the flipped (CCW, 180-degree landscape) mapping makes
     * logical x increase upward. Both iterate in physical-row-major order
     * because each physical row is contiguous memory.                     */
    int px0, px1, py0, py1;
    if (_flip180) {
        px0 = ly;                       /* inclusive */
        px1 = ly + lh;                  /* exclusive */
        py0 = _ph - (lx + lw);          /* inclusive */
        py1 = _ph - lx;                 /* exclusive */
    } else {
        px0 = _pw - ly - lh;            /* inclusive */
        px1 = _pw - ly;                 /* exclusive */
        py0 = lx;                       /* inclusive */
        py1 = lx + lw;                  /* exclusive */
    }

    for (int py = py0; py < py1; ++py) {
        uint16_t *row = &_fb[py * _pw];
        for (int px = px0; px < px1; ++px) {
            row[px] = color;
        }
    }
    __atomic_store_n(&_dirty, true, __ATOMIC_RELEASE);
}

static inline uint16_t rgb565_of(uint32_t c)
{
    /* Allow callers to pass either full 32-bit RGB or already-packed RGB565.
     * Boot_gui.cpp uses LovyanGFX color constants which are already RGB565
     * in the low 16 bits, so a simple mask is sufficient. */
    return static_cast<uint16_t>(c & 0xFFFFu);
}

void MiniGfx::fillScreen(uint32_t color)
{
    /* Fast path: memset-like fill of the whole portrait framebuffer. */
    uint16_t c = rgb565_of(color);
    const size_t n = static_cast<size_t>(_pw) * static_cast<size_t>(_ph);
    uint16_t *p = _fb;
    for (size_t i = 0; i < n; ++i) p[i] = c;
    __atomic_store_n(&_dirty, true, __ATOMIC_RELEASE);
}

void MiniGfx::fillRect(int x, int y, int w, int h, uint32_t color)
{
    fillLogicalRect(x, y, w, h, rgb565_of(color));
}

void MiniGfx::drawRect(int x, int y, int w, int h, uint32_t color)
{
    if (w <= 0 || h <= 0) return;
    uint16_t c = rgb565_of(color);
    fillLogicalRect(x, y, w, 1, c);
    fillLogicalRect(x, y + h - 1, w, 1, c);
    fillLogicalRect(x, y, 1, h, c);
    fillLogicalRect(x + w - 1, y, 1, h, c);
}

void MiniGfx::drawFastHLine(int x, int y, int w, uint32_t color)
{
    fillLogicalRect(x, y, w, 1, rgb565_of(color));
}

void MiniGfx::drawFastVLine(int x, int y, int h, uint32_t color)
{
    fillLogicalRect(x, y, 1, h, rgb565_of(color));
}

void MiniGfx::drawPixel(int x, int y, uint32_t color)
{
    writeLogicalPixel(x, y, rgb565_of(color));
}

void MiniGfx::drawLine(int x0, int y0, int x1, int y1, uint32_t color)
{
    uint16_t c = rgb565_of(color);
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        writeLogicalPixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void MiniGfx::drawCircle(int cx, int cy, int r, uint32_t color)
{
    if (r <= 0) return;
    uint16_t c = rgb565_of(color);
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        writeLogicalPixel(cx + x, cy + y, c);
        writeLogicalPixel(cx + y, cy + x, c);
        writeLogicalPixel(cx - y, cy + x, c);
        writeLogicalPixel(cx - x, cy + y, c);
        writeLogicalPixel(cx - x, cy - y, c);
        writeLogicalPixel(cx - y, cy - x, c);
        writeLogicalPixel(cx + y, cy - x, c);
        writeLogicalPixel(cx + x, cy - y, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x + 1); }
    }
}

void MiniGfx::fillCircle(int cx, int cy, int r, uint32_t color)
{
    if (r <= 0) return;
    uint16_t c = rgb565_of(color);
    for (int dy = -r; dy <= r; ++dy) {
        int span = static_cast<int>(__builtin_sqrtf(static_cast<float>(r * r - dy * dy)));
        fillLogicalRect(cx - span, cy + dy, 2 * span + 1, 1, c);
    }
}

void MiniGfx::fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color)
{
    uint16_t c = rgb565_of(color);
    /* Sort by y ascending */
    if (y0 > y1) { int tx = x0; x0 = x1; x1 = tx; int ty = y0; y0 = y1; y1 = ty; }
    if (y1 > y2) { int tx = x1; x1 = x2; x2 = tx; int ty = y1; y1 = y2; y2 = ty; }
    if (y0 > y1) { int tx = x0; x0 = x1; x1 = tx; int ty = y0; y0 = y1; y1 = ty; }

    if (y0 == y2) {
        /* Degenerate: single horizontal line */
        int minx = x0, maxx = x0;
        if (x1 < minx) minx = x1; if (x1 > maxx) maxx = x1;
        if (x2 < minx) minx = x2; if (x2 > maxx) maxx = x2;
        fillLogicalRect(minx, y0, maxx - minx + 1, 1, c);
        return;
    }

    int total_h = y2 - y0;
    for (int y = y0; y <= y2; ++y) {
        bool second_half = (y > y1) || (y1 == y0);
        int segment_h = second_half ? (y2 - y1) : (y1 - y0);
        if (segment_h == 0) continue;
        int alpha = y - y0;
        int beta  = second_half ? (y - y1) : alpha;
        int a_num = (x2 - x0) * alpha;
        int ax = x0 + a_num / total_h;
        int bx = second_half ? (x1 + (x2 - x1) * beta / segment_h)
                             : (x0 + (x1 - x0) * beta / segment_h);
        if (ax > bx) { int t = ax; ax = bx; bx = t; }
        fillLogicalRect(ax, y, bx - ax + 1, 1, c);
    }
}

void MiniGfx::drawGlyph(int x, int y, char c, uint32_t fg, uint32_t bg, bool transparent, uint8_t size)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *glyph = MINI_GFX_FONT_5x7[static_cast<int>(c) - 0x20];
    uint16_t fg565 = rgb565_of(fg);
    uint16_t bg565 = rgb565_of(bg);

    /* Cell is 6x8 (5px glyph + 1px gap on right, 7px glyph + 1px gap at bottom). */
    for (int col = 0; col < 6; ++col) {
        uint8_t column_bits = (col < 5) ? glyph[col] : 0;
        for (int row = 0; row < 8; ++row) {
            bool on = (row < 7) && (column_bits & (1u << row));
            if (on) {
                fillLogicalRect(x + col * size, y + row * size, size, size, fg565);
            } else if (!transparent) {
                fillLogicalRect(x + col * size, y + row * size, size, size, bg565);
            }
        }
    }
}

int MiniGfx::measureStringPx(const char *str) const
{
    int n = 0;
    while (*str++) n++;
    return n * 6 * _text_size;
}

void MiniGfx::drawString(const char *str, int x, int y)
{
    if (!str) return;
    const int advance_x = 6 * _text_size;
    const int glyph_h   = 8 * _text_size;
    const int text_w    = measureStringPx(str);

    int draw_x = x;
    int draw_y = y;

    switch (_text_datum) {
        case TC_DATUM: draw_x = x - text_w / 2; draw_y = y; break;
        case TR_DATUM: draw_x = x - text_w;     draw_y = y; break;
        case ML_DATUM: draw_x = x;              draw_y = y - glyph_h / 2; break;
        case MC_DATUM: draw_x = x - text_w / 2; draw_y = y - glyph_h / 2; break;
        case MR_DATUM: draw_x = x - text_w;     draw_y = y - glyph_h / 2; break;
        case BL_DATUM: draw_x = x;              draw_y = y - glyph_h; break;
        case BC_DATUM: draw_x = x - text_w / 2; draw_y = y - glyph_h; break;
        case BR_DATUM: draw_x = x - text_w;     draw_y = y - glyph_h; break;
        case TL_DATUM:
        default: break;
    }

    for (const char *p = str; *p; ++p) {
        drawGlyph(draw_x, draw_y, *p, _text_color, _text_bg, _text_bg_transparent, _text_size);
        draw_x += advance_x;
    }
}

void MiniGfx::pushImage(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (!pixels || w <= 0 || h <= 0) return;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            writeLogicalPixel(x + i, y + j, pixels[j * w + i]);
        }
    }
    __atomic_store_n(&_dirty, true, __ATOMIC_RELEASE);
}

bool MiniGfx::flushRows(int first, int last)
{
    if (!_fb || first < 0 || last <= first || last > _ph) return false;
    const esp_err_t err = esp_cache_msync(_fb + first * _pw,
        (last - first) * _pw * sizeof(uint16_t),
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    if (err != ESP_OK) ESP_LOGE(TAG, "panel cache sync failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

void MiniGfx::flushAll(void)
{
    // The boot touch task can publish while the GUI draws on the other core.
    // Consume damage BEFORE writeback so a concurrent draw always schedules
    // another flush. Clearing it afterwards can permanently lose that draw.
    if (!__atomic_exchange_n(&_dirty, false, __ATOMIC_ACQ_REL)) return;
    if (!flushRows(0, _ph)) __atomic_store_n(&_dirty, true, __ATOMIC_RELEASE);
}
