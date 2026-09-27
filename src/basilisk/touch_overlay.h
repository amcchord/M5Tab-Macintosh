/*
 * touch_overlay.h - on-screen keyboard and gaming overlays driven by the
 *                   capacitive touchscreen.
 *
 * A three-finger tap toggles the keyboard overlay; a four-finger tap
 * toggles the gaming (D-pad + action button) overlay. While the overlay
 * is visible, single-finger presses on overlay keys inject Mac ADB
 * keystrokes; single-finger presses outside the overlay footprint still
 * drive the Mac mouse as before.
 *
 * Rendering is done in the video task via a 25% sub-pixel stipple that
 * takes advantage of the 2x Mac->physical pixel scale: we only write the
 * (even, even) sub-pixel of each Mac pixel's 2x2 physical block, so the
 * original Mac content shows through in the other 3 sub-pixels. No alpha
 * blend math required.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "board_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOUCH_OVERLAY_NONE = 0,
    TOUCH_OVERLAY_KEYBOARD,
    TOUCH_OVERLAY_GAMING,
} TouchOverlayMode;

/**
 * @brief Initialize overlay state. Needs the physical display size (post
 *        HAL rotation) and the Mac framebuffer size so mouse coordinates
 *        can be scaled correctly.
 */
void TouchOverlay_Init(int display_w, int display_h,
                       int mac_w, int mac_h);

/**
 * @brief Release any ADB keys currently held by the overlay. Called from
 *        InputExit so we don't leave modifiers stuck down.
 */
void TouchOverlay_Shutdown(void);

/**
 * @brief Enable/disable all touch-driven input (mouse + overlay). Called
 *        from InputSetTouchEnabled.
 */
void TouchOverlay_SetTouchEnabled(bool enabled);

/**
 * @brief Pump one frame of multi-touch input. Drives gesture detection,
 *        overlay key handling, and single-finger Mac mouse emulation.
 *        Called from the input task.
 */
void TouchOverlay_Update(const BoardTouchMulti *multi);

/**
 * @brief Current overlay mode (used by the compositor to early-out).
 */
TouchOverlayMode TouchOverlay_GetMode(void);

/**
 * @brief True when any overlay is visible.
 */
bool TouchOverlay_IsVisible(void);

// Called by the video task after collecting dirty tiles, once per batch.
void TouchOverlay_BeginFrame(void);

/**
 * @brief Composite the captured overlay onto a single RGB565 tile that
 *        the video task is about to push to the panel. `tile_x`/`tile_y`
 *        are the tile's top-left in physical display pixels; `tile_w` /
 *        `tile_h` are the tile's dimensions in physical pixels. `pixels`
 *        is a row-major RGB565 buffer of size tile_w*tile_h.
 *
 *        Fast-path returns immediately when the overlay is hidden.
 */
void TouchOverlay_CompositeTile(int tile_x, int tile_y,
                                int tile_w, int tile_h,
                                uint16_t *pixels);

/**
 * @brief True when the overlay captured by TouchOverlay_BeginFrame
 *        intersects the physical tile, i.e. CompositeTile would draw.
 */
bool TouchOverlay_TileCovered(int tile_x, int tile_y, int tile_w, int tile_h);

#ifdef __cplusplus
} /* extern "C" */
#endif
