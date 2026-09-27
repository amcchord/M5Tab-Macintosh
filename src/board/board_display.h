/* Display HAL: board-specific panel setup, shared RGB565 surface and cache
 * publication. Boot UI owns MiniGfx until it hands off to the video task.
 * The video task converts guest pixels, composites overlays, then pushes
 * landscape tiles into that same panel-owned surface. No asynchronous copies.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

bool BoardDisplay_Init(void);
int  BoardDisplay_Width(void);
int  BoardDisplay_Height(void);
// Begin/End serialize a batch; PushTile copies synchronously. End publishes
// CPU cache lines to scanout. End must follow a successful Begin. On any
// failure the caller retains its dirty tiles and retries the batch.
bool BoardDisplay_BeginTiles(void);
bool BoardDisplay_EndTiles(void);
bool BoardDisplay_PushTile(int x, int y, int w, int h, const uint16_t *pixels);
void BoardDisplay_SetBacklight(int percent);

/**
 * @brief Set whether the framebuffer should be rotated 180 degrees
 *        before being pushed to the panel.
 *
 *  The Tab5 default is true (matches v4.0, "USB-C port on the left"
 *  hold orientation); false flips the image so the USB-C port is on
 *  the right. The Waveshare 10.1" panel orientation is fixed by the
 *  ribbon location, so this is a no-op there.
 *
 *  Must be called before BoardDisplay_Init() takes effect, OR after
 *  init but before any further BoardDisplay_PushTile / Present calls,
 *  otherwise the tile-rotation map and the panel image will disagree
 *  for one frame. Boot GUI applies it after the user dismisses the
 *  settings screen and before the splash transition.
 */
void BoardDisplay_SetFlip180(bool flip);

/**
 * @brief Flush the software drawing surface to the physical panel. On
 *        both boards MiniGfx shares the panel framebuffer; this publishes
 *        CPU cache lines for continuous DSI scanout.
 */
void BoardDisplay_Present(void);

#ifdef __cplusplus
} /* extern "C" */

/* Typed C++ accessor - returns the per-board drawing surface. Both
 * supported boards use MiniGfx on the panel-owned framebuffer, uniform across
 * boards and callers don't need board-specific drawing code. */

#if defined(BOARD_M5STACK_TAB5) || defined(BOARD_WAVESHARE_P4_101)
#include "mini_gfx/mini_gfx.h"
MiniGfx &BoardDisplay_Gfx_Board(void);
static inline MiniGfx &BoardDisplay_Gfx(void) { return BoardDisplay_Gfx_Board(); }
#endif

#endif /* __cplusplus */
