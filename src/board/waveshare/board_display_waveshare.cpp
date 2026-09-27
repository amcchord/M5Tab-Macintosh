/* One CPU-owned RGB565 scanout surface for boot UI and emulated tiles.
 * Rotate directly into the panel framebuffer and publish once per batch.
 * No DMA2D copy, reusable DMA scratch, or copy-completion ISR is involved. */

#include "board_display.h"
#include "board_config.h"

#include "mini_gfx/mini_gfx.h"
#include "board_display_surface.h"

#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"

#include "bsp/esp32_p4_wifi6_touch_lcd_x.h"
#include "bsp/display.h"

static const char *TAG = "board_display";

/* Panel is 800x1280 portrait natively - from BSP display.h constants. */
static constexpr int PANEL_W = 800;
static constexpr int PANEL_H = 1280;

static esp_lcd_panel_handle_t    s_panel    = nullptr;
static esp_lcd_panel_io_handle_t s_panel_io = nullptr;
static bool                      s_inited   = false;

extern "C" bool BoardDisplay_Init(void)
{
    if (s_inited) return true;

    ESP_LOGI(TAG, "Initializing Waveshare P4 10.1 display...");

    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_i2c_init failed: %s", esp_err_to_name(err));
        return false;
    }

    bsp_display_config_t cfg = {};
    err = bsp_display_new(&cfg, &s_panel, &s_panel_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_display_new failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Backlight on. The BSP already configured the LEDC PWM inside
     * bsp_display_new -> bsp_display_brightness_init. */
    bsp_display_brightness_set(100);

    void *dpi_fb = nullptr;
    err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &dpi_fb);
    if (err != ESP_OK || !dpi_fb) return false;
    if (!BoardDisplay_AttachSurface(dpi_fb, PANEL_W, PANEL_H)) return false;
    BoardDisplay_Present();

    s_inited = true;
    ESP_LOGI(TAG, "Display up: logical %dx%d, panel %dx%d",
             BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT, PANEL_W, PANEL_H);
    return true;
}

extern "C" void BoardDisplay_SetBacklight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    bsp_display_brightness_set(percent);
}

/* Waveshare 10.1" panel orientation is fixed by the ribbon location;
 * accept the request but do nothing so the boot GUI's Rotate-180
 * toggle is a harmless no-op on this board. */
extern "C" void BoardDisplay_SetFlip180(bool flip)
{
    (void)flip;
}
