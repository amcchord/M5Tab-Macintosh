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

#include <M5Unified.h>
#include "lgfx/v1/platforms/esp32p4/Panel_DSI.hpp"

static const char *TAG = "board_display";

/* All supported Tab5 panel revisions are natively 720x1280 portrait
 * (see M5GFX.cpp, which applies this geometry after autodetection). */
static constexpr int PANEL_W = 720;
static constexpr int PANEL_H = 1280;

/* Form the protected base member pointer in a derived scope; apply it to
 * the actual Panel_DSI base. No cast to a fictitious derived object. */
struct Tab5DsiAccess : public lgfx::Panel_DSI {
    static esp_lcd_panel_handle_t handle(lgfx::Panel_DSI *panel)
    {
        return panel->*(&Tab5DsiAccess::_disp_panel_handle);
    }
};

static esp_lcd_panel_handle_t        s_panel    = nullptr;
static bool                          s_inited   = false;

extern "C" bool BoardDisplay_Init(void)
{
    if (s_inited) return true;

    ESP_LOGI(TAG, "Initializing Tab5 display (direct DSI pipeline)...");

    /* M5.begin() in Board_Init already detected and brought up the panel.
     * Reach into M5.Display to grab the esp_lcd_panel_handle_t it
     * created so we can drive the DSI bus directly. */
    auto *panel_dev = M5.Display.getPanel();
    if (!panel_dev) {
        ESP_LOGE(TAG, "M5.Display has no panel device");
        return false;
    }
    s_panel = Tab5DsiAccess::handle(static_cast<lgfx::Panel_DSI *>(panel_dev));
    if (!s_panel) {
        ESP_LOGE(TAG, "Tab5 Panel_DSI has no esp_lcd_panel_handle");
        return false;
    }

    esp_err_t err;

    /* Use the scanout allocation directly. The shared surface publishes
     * CPU cache lines without a second PSRAM-to-PSRAM DMA2D copy. */
    void *dpi_fb = nullptr;
    err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &dpi_fb);
    if (err != ESP_OK || !dpi_fb) {
        ESP_LOGE(TAG, "get_frame_buffer failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!BoardDisplay_AttachSurface(dpi_fb, PANEL_W, PANEL_H)) return false;

    /* Tab5 ships with the panel ribbon at the top of the landscape view
     * when using the default 90-CW mapping. Default to flipping the
     * landscape output 180 degrees so the Mac desktop appears right-
     * side up when the device is held with the USB-C port on the left.
     * The boot GUI can override this via BoardDisplay_SetFlip180() once
     * it knows the user's preference - the call drives both this flip
     * and the touch driver's coordinate transform. */
    BoardDisplay_SetFlip180(true);

    /* Do not explicitly flush here. beginExternalFb
     * already zeroed the DPI framebuffer, and the DPI is continuously
     * scanning that same FB, so an explicit black flush would be a
     * second visible refresh before the first real content (splash)
     * lands. The caller (mac_splash::paint_splash) will overwrite every
     * pixel and call BoardDisplay_Present, which does the cache sync.   */

    s_inited = true;
    ESP_LOGI(TAG, "Display up: logical %dx%d, panel %dx%d",
             BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT, PANEL_W, PANEL_H);
    return true;
}

extern "C" void BoardDisplay_SetBacklight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* M5Unified already configured the Tab5 backlight LEDC (GPIO22 PWM
     * on a channel allocated by M5GFX). Reuse its setter. Maps 0-100
     * percent to the 0-255 brightness range LovyanGFX expects. */
    M5.Display.setBrightness((percent * 255) / 100);
}

/* Whether the landscape framebuffer is currently flipped 180. Initialised
 * in BoardDisplay_Init() from the panel default and then updated by the
 * boot GUI when the user changes the Rotate-180 checkbox. The touch
 * driver reads this through BoardTouch_IsFlipped180() so its coordinate
 * transform stays in sync with what's on screen. */
static bool s_flip180_active = true;

extern "C" void BoardDisplay_SetFlip180(bool flip)
{
    BoardDisplay_Gfx().setFlip180(flip);
    __atomic_store_n(&s_flip180_active, flip, __ATOMIC_RELEASE);
}

/* Allow the touch HAL to query the current state without a public header
 * for an internal-only flag. */
extern "C" bool BoardDisplayTab5_GetFlip180(void)
{
    return __atomic_load_n(&s_flip180_active, __ATOMIC_ACQUIRE);
}
