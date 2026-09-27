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
    if (!writePanelTile(s_gfx.portraitFb(), s_gfx.panelW(), s_gfx.panelH(),
                         flip, x, y, w, h, pixels)) return false;
    const int first = flip ? s_gfx.panelH() - x - w : x;
    const int last = first + w;
    if (first < s_first_row) s_first_row = first;
    if (last > s_last_row) s_last_row = last;
    return true;
}

extern "C" bool BoardDisplay_EndTiles(void)
{
    const bool ok = s_first_row >= s_last_row || s_gfx.flushRows(s_first_row, s_last_row);
    xSemaphoreGive(s_surface_mutex);
    return ok;
}

extern "C" void BoardDisplay_Present(void)
{
    if (!s_surface_mutex) return;
    xSemaphoreTake(s_surface_mutex, portMAX_DELAY);
    s_gfx.flushAll();
    xSemaphoreGive(s_surface_mutex);
}

MiniGfx &BoardDisplay_Gfx_Board(void) { return s_gfx; }
