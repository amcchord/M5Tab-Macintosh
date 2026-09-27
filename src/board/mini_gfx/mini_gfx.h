/*
 * mini_gfx.h - tiny RGB565 text/rect/primitive renderer for the boot GUI
 *              on boards that lack M5GFX/LovyanGFX.
 *
 * The boot GUI draws with a small API surface (fillRect, drawRect,
 * drawFastHLine/VLine, drawLine, drawCircle, drawPixel, drawString, ...).
 * MiniGfx provides exactly those methods on top of a landscape RGB565
 * panel-owned framebuffer, plus coordinate rotation so we can drive a portrait
 * MIPI-DSI panel as a landscape display.
 *
 * Draws update the backing framebuffer immediately. Call flushAll() to
 * publish the changed CPU cache lines to continuous panel scanout.
 *
 * Method names and signatures mirror M5GFX / LovyanGFX for the subset that
 * boot_gui.cpp uses, so the same boot_gui code compiles against either
 * backend.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* Text datum constants matching LovyanGFX / M5GFX exactly. */
#ifndef MC_DATUM
#define TL_DATUM 0   /* Top-Left     */
#define TC_DATUM 1   /* Top-Center   */
#define TR_DATUM 2   /* Top-Right    */
#define ML_DATUM 3   /* Middle-Left  */
#define MC_DATUM 4   /* Middle-Center*/
#define MR_DATUM 5   /* Middle-Right */
#define BL_DATUM 6
#define BC_DATUM 7
#define BR_DATUM 8
#endif

class MiniGfx {
public:
    MiniGfx();

    // Attach the DPI-owned framebuffer. Logical coordinates are landscape;
    // storage is portrait. MiniGfx drawing goes through the CPU cache and is
    // published by flushAll(); the emulator's tile renderer writes through
    // scanoutFb() instead (see below).
    bool beginExternalFb(void *external_fb, int logical_w, int logical_h,
                         int panel_w, int panel_h);

    /* Flip the landscape view 180 degrees. Default is off, matching the
     * 90-degree CW mapping (lx, ly) -> (_pw - 1 - ly, lx) used on boards
     * whose panel ribbon exits at the bottom of the landscape view. When
     * enabled the mapping becomes (lx, ly) -> (ly, _ph - 1 - lx) which
     * is equivalent to rotating the rendered landscape output 180
     * degrees. Call before any draws; toggling at runtime does not
     * re-render already-drawn pixels. */
    void setFlip180(bool on) { _flip180 = on; }
    bool flip180(void) const { return _flip180; }

    /* LovyanGFX-compatible drawing API (subset used by boot_gui.cpp). */
    int  width(void)  const { return _lw; }
    int  height(void) const { return _lh; }

    void fillScreen(uint32_t color);
    void fillRect(int x, int y, int w, int h, uint32_t color);
    void drawRect(int x, int y, int w, int h, uint32_t color);
    void drawFastHLine(int x, int y, int w, uint32_t color);
    void drawFastVLine(int x, int y, int h, uint32_t color);
    void drawLine(int x0, int y0, int x1, int y1, uint32_t color);
    void drawPixel(int x, int y, uint32_t color);
    void drawCircle(int cx, int cy, int r, uint32_t color);
    void fillCircle(int cx, int cy, int r, uint32_t color);
    void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color);

    void setTextColor(uint32_t color)               { _text_color = color; _text_bg_transparent = true; }
    void setTextColor(uint32_t color, uint32_t bg)  { _text_color = color; _text_bg = bg; _text_bg_transparent = false; }
    void setTextSize(uint8_t size)                  { _text_size = size < 1 ? 1 : size; }
    void setTextDatum(uint8_t datum)                { _text_datum = datum; }

    void drawString(const char *str, int x, int y);

    /* Copy landscape RGB565 pixels into the rotated portrait framebuffer. */
    void pushImage(int x, int y, int w, int h, const uint16_t *pixels);

    // Flush boot-UI damage, retaining concurrent writes or failed writeback.
    // The written-back lines are also dropped from the cache, so no stale
    // cached copy can later be written back over pixels stored through
    // scanoutFb().
    void flushAll(void);
    // Publish a half-open range of physical portrait rows after a tile batch
    // written through a cached view (needed only when scanoutIsCached()).
    bool flushRows(int first, int last);

    /* Raw framebuffer (portrait orientation, size panel_w * panel_h). */
    uint16_t *portraitFb(void) { return _fb; }
    /* The same pixels through the non-cacheable PSRAM alias when the
     * framebuffer lives in PSRAM, otherwise the cached pointer. Streaming
     * emulator tiles this way keeps megabytes of panel traffic out of the L1
     * data cache and L2 that the emulator core shares, and stores are visible
     * to scanout without a writeback. */
    uint16_t *scanoutFb(void)  { return _scanout; }
    bool scanoutIsCached(void) const { return _scanout == _fb; }
    int       panelW(void)     { return _pw; }
    int       panelH(void)     { return _ph; }

private:
    uint16_t *_fb          = nullptr;  /* RGB565, portrait, _pw x _ph */
    uint16_t *_scanout     = nullptr;  /* _fb, or its non-cacheable alias */
    int      _lw           = 0;        /* logical landscape width  */
    int      _lh           = 0;        /* logical landscape height */
    int      _pw           = 0;        /* panel portrait width     */
    int      _ph           = 0;        /* panel portrait height    */
    bool     _flip180      = false;    /* rotate landscape view 180 degrees */

    bool     _dirty        = false;   /* any draw op sets this; flushAll clears it */

    uint32_t _text_color   = 0xFFFFu;  /* white  */
    uint32_t _text_bg      = 0x0000u;  /* black  */
    bool     _text_bg_transparent = true;
    uint8_t  _text_size    = 1;
    uint8_t  _text_datum   = TL_DATUM;

    /* Map (lx, ly) logical -> (px, py) portrait and write one pixel. */
    inline void writeLogicalPixel(int lx, int ly, uint16_t color);
    /* Fill a logical rectangle (landscape coords) with a solid color. */
    void fillLogicalRect(int lx, int ly, int lw, int lh, uint16_t color);
    /* Text helpers */
    int  glyphWidthPx(uint8_t size) const  { return 6 * size; }
    int  glyphHeightPx(uint8_t size) const { return 8 * size; }
    int  measureStringPx(const char *str) const;
    void drawGlyph(int x, int y, char c, uint32_t fg, uint32_t bg, bool transparent, uint8_t size);
};
