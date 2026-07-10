/*
 * board_touch_tab5.cpp - touch HAL for M5Stack Tab5.
 *
 * We pull raw portrait (720x1280) coordinates straight from the
 * detected GT911 or ST712x driver and rotate them ourselves to match MiniGfx's
 * logical landscape (1280x720). Going through M5.Touch.getDetail()
 * would apply M5GFX's display rotation, but we no longer draw through
 * M5.Display - our framebuffer rotation is done inside MiniGfx - so
 * the two rotations were off by 90 degrees (X and Y appeared swapped).
 *
 * Multi-touch: M5.Touch caches the controller's reported points in
 * TOUCH_MAX_POINTS slots; we iterate them all for overlays that need
 * 3/4-finger gesture detection.
 */

#include "board_touch.h"
#include "board_config.h"

#include <M5Unified.h>

static constexpr int TAB5_PANEL_W_PORTRAIT = 720;
static constexpr int TAB5_PANEL_H_PORTRAIT = 1280;

extern "C" bool BoardTouch_Init(void)
{
    /* M5.begin() already detected and set up the Tab5 touch controller. */
    return true;
}

extern "C" void BoardTouch_Update(void)
{
    /* M5.update() pumps the touch state. It's already called from
     * Board_Update(), so nothing to do here. */
}

/* Map raw portrait (720x1280) to logical landscape (1280x720). Two
 * orientations are supported, gated on the same flag the display HAL
 * uses to decide whether MiniGfx flips its tile output.
 *
 * Rotate 180 = true (v4.0 default, USB-C port on the left):
 *   logical (lx, ly) -> portrait (px, py) = (ly, _ph - 1 - lx)
 *   Inverse: lx = (_ph - 1) - py, ly = px
 *
 * Rotate 180 = false (USB-C port on the right):
 *   logical (lx, ly) -> portrait (px, py) = (_pw - 1 - ly, lx)
 *   Inverse: lx = py, ly = (_pw - 1) - px
 */
extern "C" bool BoardDisplayTab5_GetFlip180(void);

static inline void portrait_to_landscape(int raw_x, int raw_y, int *out_x, int *out_y)
{
    int px = raw_x;
    int py = raw_y;
    if (px < 0) px = 0;
    if (px >= TAB5_PANEL_W_PORTRAIT) px = TAB5_PANEL_W_PORTRAIT - 1;
    if (py < 0) py = 0;
    if (py >= TAB5_PANEL_H_PORTRAIT) py = TAB5_PANEL_H_PORTRAIT - 1;

    if (BoardDisplayTab5_GetFlip180()) {
        *out_x = (TAB5_PANEL_H_PORTRAIT - 1) - py;
        *out_y = px;
    } else {
        *out_x = py;
        *out_y = (TAB5_PANEL_W_PORTRAIT - 1) - px;
    }
}

extern "C" BoardTouchDetail BoardTouch_GetDetail(void)
{
    auto det = M5.Touch.getDetail();
    BoardTouchDetail out;
    out.pressed = det.isPressed();
    out.x = 0;
    out.y = 0;
    out.id = -1;

    if (out.pressed) {
        auto raw = M5.Touch.getTouchPointRaw(0);
        portrait_to_landscape(raw.x, raw.y, &out.x, &out.y);
        out.id = raw.id;
    }
    return out;
}

extern "C" void BoardTouch_GetMulti(BoardTouchMulti *out)
{
    if (!out) return;

    out->count = 0;
    for (int i = 0; i < BOARD_TOUCH_MAX_POINTS; ++i) {
        out->p[i].pressed = false;
        out->p[i].x = 0;
        out->p[i].y = 0;
        out->p[i].id = -1;
    }

    uint8_t n = M5.Touch.getCount();
    if (n > BOARD_TOUCH_MAX_POINTS) n = BOARD_TOUCH_MAX_POINTS;

    for (uint8_t i = 0; i < n; ++i) {
        auto raw = M5.Touch.getTouchPointRaw(i);
        BoardTouchDetail *d = &out->p[out->count++];
        d->pressed = true;
        portrait_to_landscape(raw.x, raw.y, &d->x, &d->y);
        d->id = raw.id;
    }
}
