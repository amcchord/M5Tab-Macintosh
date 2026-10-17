#include "board_display.h"
#include "board_display_surface.h"
#include "panel_surface.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static MiniGfx s_gfx;
static SemaphoreHandle_t s_surface_mutex = nullptr;
static int s_first_row = 0, s_last_row = 0;

bool BoardDisplay_AttachSurface(void *framebuffer, int panel_width, int panel_height)
{
    if (s_surface_mutex) return true;
    if (panel_width != BOARD_DISPLAY_HEIGHT || panel_height != BOARD_DISPLAY_WIDTH)
        return false;
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (!mutex) return false;
    if (!s_gfx.beginExternalFb(framebuffer, BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT,
                               panel_width, panel_height)) {
        vSemaphoreDelete(mutex);
        return false;
    }
    s_surface_mutex = mutex;
    return true;
}

extern "C" int BoardDisplay_Width(void) { return BOARD_DISPLAY_WIDTH; }
extern "C" int BoardDisplay_Height(void) { return BOARD_DISPLAY_HEIGHT; }

extern "C" bool BoardDisplay_BeginTiles(void)
{
    if (!s_surface_mutex) return false;
    xSemaphoreTake(s_surface_mutex, portMAX_DELAY);
    s_first_row = s_gfx.panelH();
    s_last_row = 0;
    return true;
}

extern "C" bool BoardDisplay_PushTile(int x, int y, int w, int h, const uint16_t *pixels)
{
    const bool flip = s_gfx.flip180();
    if (!writePanelTile(s_gfx.scanoutFb(), s_gfx.panelW(), s_gfx.panelH(),
                         flip, x, y, w, h, pixels)) return false;
    const int first = flip ? s_gfx.panelH() - x - w : x;
    const int last = first + w;
    if (first < s_first_row) s_first_row = first;
    if (last > s_last_row) s_last_row = last;
    return true;
}

extern "C" bool BoardDisplay_PushIndexedTile(int x, int y, int w, int h, const uint8_t *src,
                                             uint32_t stride, int bits, int first_pixel,
                                             const uint32_t *pairs)
{
    const bool flip = s_gfx.flip180();
    const bool written = (BOARD_PIXEL_SCALE == 1)
        ? writePanelIndexedTile1x(s_gfx.scanoutFb(), s_gfx.panelW(), s_gfx.panelH(),
                                  flip, x, y, w, h, src, stride, bits, first_pixel, pairs)
        : writePanelIndexedTile2x(s_gfx.scanoutFb(), s_gfx.panelW(), s_gfx.panelH(),
                                  flip, x, y, w, h, src, stride, bits, first_pixel, pairs);
    if (!written) return false;
    const int first = flip ? s_gfx.panelH() - x - w : x;
    const int last = first + w;
    if (first < s_first_row) s_first_row = first;
    if (last > s_last_row) s_last_row = last;
    return true;
}

extern "C" bool BoardDisplay_EndTiles(void)
{
    // Tiles are written through the non-cacheable scanout view when one
    // exists; only a cached view needs its rows written back.
    const bool ok = s_first_row >= s_last_row || !s_gfx.scanoutIsCached() ||
                    s_gfx.flushRows(s_first_row, s_last_row);
    xSemaphoreGive(s_surface_mutex);
    return ok;
}

extern "C" bool BoardDisplay_CapturePanel(uint8_t *pixels, size_t capacity, uint16_t *palette,
                                          uint16_t *width, uint16_t *height, int step)
{
    if (!s_surface_mutex || !pixels || !palette || !width || !height || step < 1) return false;
    const int w = BOARD_DISPLAY_WIDTH / step, h = BOARD_DISPLAY_HEIGHT / step;
    if (capacity < (size_t)w * h) return false;
    const uint16_t *fb = s_gfx.scanoutFb();
    const bool flip = s_gfx.flip180();
    // Open-addressed color -> index table; only the automation task calls this.
    static int16_t slots[1024];
    for (int16_t &slot : slots) slot = -1;
    int count = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint16_t c = readPanelPixel(fb, s_gfx.panelW(), s_gfx.panelH(), flip,
                                              x * step, y * step);
            unsigned hash = (c * 40503u) >> 6 & 1023u;
            int index = -1;
            while (slots[hash] >= 0) {
                if (palette[slots[hash]] == c) { index = slots[hash]; break; }
                hash = (hash + 1) & 1023u;
            }
            if (index < 0 && count < 256) {
                index = count++;
                palette[index] = c;
                slots[hash] = (int16_t)index;
            } else if (index < 0) {
                long best = -1;
                for (int i = 0; i < 256; ++i) {
                    const int dr = (c >> 11) - (palette[i] >> 11);
                    const int dg = ((c >> 5) & 63) - ((palette[i] >> 5) & 63);
                    const int db = (c & 31) - (palette[i] & 31);
                    const long d = 4L * dr * dr + dg * dg + 4L * db * db;
                    if (best < 0 || d < best) { best = d; index = i; }
                }
            }
            pixels[(size_t)y * w + x] = (uint8_t)index;
        }
    for (int i = count; i < 256; ++i) palette[i] = 0;
    *width = (uint16_t)w;
    *height = (uint16_t)h;
    return true;
}

extern "C" void BoardDisplay_Present(void)
{
    if (!s_surface_mutex) return;
    xSemaphoreTake(s_surface_mutex, portMAX_DELAY);
    s_gfx.flushAll();
    xSemaphoreGive(s_surface_mutex);
}

MiniGfx &BoardDisplay_Gfx_Board(void) { return s_gfx; }
