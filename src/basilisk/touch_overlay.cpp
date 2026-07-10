/*
 * touch_overlay.cpp - multi-touch keyboard and gaming overlays.
 *
 * See touch_overlay.h for the high-level contract. Internal structure:
 *
 *   1. Static key layouts (keyboard_layout, gaming_layout). Positions are
 *      built lazily from display dimensions supplied to TouchOverlay_Init.
 *
 *   2. Per-slot tracking. Both Tab5 and Waveshare can report up to 5
 *      simultaneous points, but their id semantics differ (Tab5 = stable
 *      GT911 id, Waveshare = per-frame index). We resolve this with a
 *      nearest-neighbour slot matcher that keeps "which key / mouse each
 *      finger owns" across frames.
 *
 *   3. Gesture detector. Watches peak simultaneous finger count; when all
 *      fingers release within a short window and with little centroid
 *      drift, fires the 3- / 4-finger tap toggle.
 *
 *   4. Mouse routing. A dedicated mouse slot drives the existing Mac
 *      mouse logic (cursor + deferred click + drag deadzone), carried
 *      over from input_esp32.cpp.
 *
 *   5. Stipple compositor. Writes the overlay into the 80x80 RGB565 tile
 *      buffer the video task is about to push. Uses the 2x Mac pixel
 *      scale to cheat semi-transparency by writing only 1 of every 4
 *      physical sub-pixels (the (even, even) sub-pixel within each Mac
 *      pixel's 2x2 block).
 */

#include "touch_overlay.h"

/* sysdeps.h must come before adb.h to provide the short integer typedefs
 * (uint8/uint16/uint32) those Basilisk headers depend on. */
#include "sysdeps.h"
#include "adb.h"
#include "input.h"

#include "board_config.h"
#include "chicago_font_data.h"

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ============================================================================
 * Mac ADB keycodes we reference. Mirrors the tables in input_esp32.cpp.
 * ============================================================================
 */
#define MAC_KEY_A         0x00
#define MAC_KEY_S         0x01
#define MAC_KEY_D         0x02
#define MAC_KEY_F         0x03
#define MAC_KEY_H         0x04
#define MAC_KEY_G         0x05
#define MAC_KEY_Z         0x06
#define MAC_KEY_X         0x07
#define MAC_KEY_C         0x08
#define MAC_KEY_V         0x09
#define MAC_KEY_B         0x0B
#define MAC_KEY_Q         0x0C
#define MAC_KEY_W         0x0D
#define MAC_KEY_E         0x0E
#define MAC_KEY_R         0x0F
#define MAC_KEY_Y         0x10
#define MAC_KEY_T         0x11
#define MAC_KEY_1         0x12
#define MAC_KEY_2         0x13
#define MAC_KEY_3         0x14
#define MAC_KEY_4         0x15
#define MAC_KEY_6         0x16
#define MAC_KEY_5         0x17
#define MAC_KEY_EQUAL     0x18
#define MAC_KEY_9         0x19
#define MAC_KEY_7         0x1A
#define MAC_KEY_MINUS     0x1B
#define MAC_KEY_8         0x1C
#define MAC_KEY_0         0x1D
#define MAC_KEY_RBRACK    0x1E
#define MAC_KEY_O         0x1F
#define MAC_KEY_U         0x20
#define MAC_KEY_LBRACK    0x21
#define MAC_KEY_I         0x22
#define MAC_KEY_P         0x23
#define MAC_KEY_RETURN    0x24
#define MAC_KEY_L         0x25
#define MAC_KEY_J         0x26
#define MAC_KEY_QUOTE     0x27
#define MAC_KEY_K         0x28
#define MAC_KEY_SEMI      0x29
#define MAC_KEY_BACKSLASH 0x2A
#define MAC_KEY_COMMA     0x2B
#define MAC_KEY_SLASH     0x2C
#define MAC_KEY_N         0x2D
#define MAC_KEY_M         0x2E
#define MAC_KEY_PERIOD    0x2F
#define MAC_KEY_TAB       0x30
#define MAC_KEY_SPACE     0x31
#define MAC_KEY_GRAVE     0x32
#define MAC_KEY_DELETE    0x33  /* Backspace */
#define MAC_KEY_ESC       0x35
#define MAC_KEY_CTRL      0x36
#define MAC_KEY_CMD       0x37
#define MAC_KEY_SHIFT     0x38
#define MAC_KEY_CAPS      0x39
#define MAC_KEY_OPTION    0x3A
#define MAC_KEY_LEFT      0x3B
#define MAC_KEY_RIGHT     0x3C
#define MAC_KEY_DOWN      0x3D
#define MAC_KEY_UP        0x3E

/* ============================================================================
 * Tunables
 * ============================================================================
 */
#define OVERLAY_MAX_KEYS        72

/* Gesture detector.
 *   HOLD_MS  - max time from first finger down to last finger up.
 *   DRIFT_PX - per-finger displacement budget from its own down position.
 *              Centroid drift is useless here because adding each new
 *              finger can swing the centroid hundreds of pixels even if
 *              the fingers themselves are perfectly still.
 */
#define GESTURE_MAX_HOLD_MS     600
#define GESTURE_MAX_DRIFT_PX    140
#define GESTURE_DEBOUNCE_MS     500

/* Mouse drag deadzone (Mac pixels) - copied from input_esp32.cpp */
#define TAP_MOVEMENT_THRESHOLD  8

/* Stipple colors */
#define OVERLAY_COLOR_BG        0xFFFF  /* white */
#define OVERLAY_COLOR_FG        0x0000  /* black */

/* ============================================================================
 * Key flags
 * ============================================================================
 */
#define KFLAG_MODIFIER          (1 << 0)  /* latched - tap toggles */
#define KFLAG_CAPS              (1 << 1)  /* caps lock - latch only */

typedef struct OverlayKey {
    int16_t  x, y, w, h;      /* physical display pixels */
    uint8_t  mac_keycode;
    uint8_t  flags;
    const char *label;
    /* Runtime state */
    bool     held;            /* true while a finger is currently on it */
    bool     latched;         /* true when modifier is toggled on */
} OverlayKey;

/* ============================================================================
 * State
 * ============================================================================
 */

static TouchOverlayMode s_mode = TOUCH_OVERLAY_NONE;
static bool             s_touch_enabled = true;

static int              s_disp_w = 0;
static int              s_disp_h = 0;
static int              s_mac_w  = 0;
static int              s_mac_h  = 0;

/* Layouts are arrays of keys; s_layout points to whichever is active. */
static OverlayKey       s_kb_layout[OVERLAY_MAX_KEYS];
static int              s_kb_count = 0;
static OverlayKey       s_game_layout[OVERLAY_MAX_KEYS];
static int              s_game_count = 0;

/* Union bounding box of the active overlay, used to skip tiles quickly
 * and to decide whether a single-finger touch should become mouse or key. */
static int              s_overlay_bbox_x = 0;
static int              s_overlay_bbox_y = 0;
static int              s_overlay_bbox_w = 0;
static int              s_overlay_bbox_h = 0;

/* ----- Slot tracking ----- */
struct OverlaySlot {
    bool     active;
    int      last_x, last_y;
    int      down_x, down_y;
    uint32_t down_ms;
    int      hw_id;               /* hw-reported id, used to re-match */
    int      key_index;           /* -1 if not on a key */
    bool     is_mouse;            /* this slot owns the Mac mouse */
};

static const int SLOT_CAPACITY = BOARD_TOUCH_MAX_POINTS;
static OverlaySlot s_slots[BOARD_TOUCH_MAX_POINTS];

/* ----- Gesture state -----
 * Tracks the highest simultaneous finger count and the first-down time
 * since the last "all fingers released" transition. `drift_ok` is flipped
 * false as soon as any single finger moves more than GESTURE_MAX_DRIFT_PX
 * from its own touch-down position (see slot.down_x/y).
 */
static uint8_t  s_gesture_peak       = 0;
static uint32_t s_gesture_first_ms   = 0;
static bool     s_gesture_drift_ok   = true;
static uint32_t s_last_toggle_ms     = 0;
static bool     s_gesture_triggered  = false;  /* set when a toggle fired during the current touch batch */

/* ----- Mouse state (carried over from input_esp32.cpp) ----- */
static bool s_mouse_pressed       = false;
static bool s_mouse_click_pending = false;
static bool s_mouse_is_dragging   = false;
static int  s_mouse_start_x       = 0;
static int  s_mouse_start_y       = 0;
static int  s_mouse_last_x        = 0;
static int  s_mouse_last_y        = 0;

/* ============================================================================
 * Forward decls
 * ============================================================================
 */
static void release_all_keys(void);
static void release_held_modifiers(void);
static void rebuild_overlay_bbox(void);
static void toggle_mode(TouchOverlayMode new_mode);
static void mark_overlay_dirty(void);
static void mark_key_dirty(const OverlayKey *k);

/* ============================================================================
 * Layout construction
 * ============================================================================
 */

static OverlayKey make_key(int x, int y, int w, int h,
                           uint8_t keycode, const char *label, uint8_t flags)
{
    OverlayKey k;
    k.x = (int16_t)x;
    k.y = (int16_t)y;
    k.w = (int16_t)w;
    k.h = (int16_t)h;
    k.mac_keycode = keycode;
    k.flags = flags;
    k.label = label;
    k.held = false;
    k.latched = false;
    return k;
}

static void build_keyboard_layout(void)
{
    s_kb_count = 0;

    /* 11 columns x 5 rows spanning the full display width, anchored at
     * the bottom of the screen. Cells are slightly inset by a 1-px gap
     * that naturally falls out of the 25% stipple pattern (borders of
     * adjacent keys overlap at (even, even) sub-pixels).                */
    const int cols = 11;
    const int rows = 5;
    const int cell_w = s_disp_w / cols;                /* ~116 px */
    const int cell_h = 68;
    const int total_h = cell_h * rows;                 /* 340 px */
    const int base_y  = s_disp_h - total_h;
    const int base_x  = (s_disp_w - cell_w * cols) / 2;

    auto put = [&](int row, int col, int span,
                   uint8_t code, const char *label, uint8_t flags) {
        int x = base_x + col * cell_w;
        int y = base_y + row * cell_h;
        int w = cell_w * span;
        s_kb_layout[s_kb_count++] = make_key(x, y, w, cell_h, code, label, flags);
    };

    /* Row 0: digits */
    put(0, 0,  1, MAC_KEY_1,      "1",   0);
    put(0, 1,  1, MAC_KEY_2,      "2",   0);
    put(0, 2,  1, MAC_KEY_3,      "3",   0);
    put(0, 3,  1, MAC_KEY_4,      "4",   0);
    put(0, 4,  1, MAC_KEY_5,      "5",   0);
    put(0, 5,  1, MAC_KEY_6,      "6",   0);
    put(0, 6,  1, MAC_KEY_7,      "7",   0);
    put(0, 7,  1, MAC_KEY_8,      "8",   0);
    put(0, 8,  1, MAC_KEY_9,      "9",   0);
    put(0, 9,  1, MAC_KEY_0,      "0",   0);
    put(0, 10, 1, MAC_KEY_DELETE, "Del", 0);

    /* Row 1: qwerty */
    put(1, 0,  1, MAC_KEY_Q, "Q", 0);
    put(1, 1,  1, MAC_KEY_W, "W", 0);
    put(1, 2,  1, MAC_KEY_E, "E", 0);
    put(1, 3,  1, MAC_KEY_R, "R", 0);
    put(1, 4,  1, MAC_KEY_T, "T", 0);
    put(1, 5,  1, MAC_KEY_Y, "Y", 0);
    put(1, 6,  1, MAC_KEY_U, "U", 0);
    put(1, 7,  1, MAC_KEY_I, "I", 0);
    put(1, 8,  1, MAC_KEY_O, "O", 0);
    put(1, 9,  1, MAC_KEY_P, "P", 0);
    put(1, 10, 1, MAC_KEY_RETURN, "Ret", 0);

    /* Row 2: asdf */
    put(2, 0,  1, MAC_KEY_A, "A", 0);
    put(2, 1,  1, MAC_KEY_S, "S", 0);
    put(2, 2,  1, MAC_KEY_D, "D", 0);
    put(2, 3,  1, MAC_KEY_F, "F", 0);
    put(2, 4,  1, MAC_KEY_G, "G", 0);
    put(2, 5,  1, MAC_KEY_H, "H", 0);
    put(2, 6,  1, MAC_KEY_J, "J", 0);
    put(2, 7,  1, MAC_KEY_K, "K", 0);
    put(2, 8,  1, MAC_KEY_L, "L", 0);
    put(2, 9,  1, MAC_KEY_SEMI,  ";", 0);
    put(2, 10, 1, MAC_KEY_TAB,   "Tab", 0);

    /* Row 3: zxcv + shift */
    put(3, 0,  1, MAC_KEY_SHIFT,  "Shift", KFLAG_MODIFIER);
    put(3, 1,  1, MAC_KEY_Z, "Z", 0);
    put(3, 2,  1, MAC_KEY_X, "X", 0);
    put(3, 3,  1, MAC_KEY_C, "C", 0);
    put(3, 4,  1, MAC_KEY_V, "V", 0);
    put(3, 5,  1, MAC_KEY_B, "B", 0);
    put(3, 6,  1, MAC_KEY_N, "N", 0);
    put(3, 7,  1, MAC_KEY_M, "M", 0);
    put(3, 8,  1, MAC_KEY_COMMA,  ",", 0);
    put(3, 9,  1, MAC_KEY_PERIOD, ".", 0);
    put(3, 10, 1, MAC_KEY_ESC,    "Esc", 0);

    /* Row 4: modifiers + space + arrows */
    put(4, 0,  1, MAC_KEY_CTRL,   "Ctrl", KFLAG_MODIFIER);
    put(4, 1,  1, MAC_KEY_OPTION, "Opt",  KFLAG_MODIFIER);
    put(4, 2,  1, MAC_KEY_CMD,    "Cmd",  KFLAG_MODIFIER);
    put(4, 3,  5, MAC_KEY_SPACE,  "Space", 0);
    put(4, 8,  1, MAC_KEY_LEFT,   "<",    0);
    put(4, 9,  1, MAC_KEY_DOWN,   "v",    0);
    put(4, 10, 1, MAC_KEY_RIGHT,  ">",    0);

    /* Keyboard footprint is the full row band. */
    s_overlay_bbox_x = 0;
    s_overlay_bbox_y = base_y;
    s_overlay_bbox_w = s_disp_w;
    s_overlay_bbox_h = total_h;
}

static void build_gaming_layout(void)
{
    s_game_count = 0;

    /* D-pad lower-left, action buttons lower-right, sized big enough to
     * tap reliably while watching the rest of the screen.              */
    const int dpad_size    = 110;
    const int dpad_cx      = 150 + dpad_size;
    const int dpad_cy      = s_disp_h - 30 - dpad_size * 3 / 2;

    auto push = [&](int x, int y, int w, int h, uint8_t code, const char *label) {
        s_game_layout[s_game_count++] = make_key(x, y, w, h, code, label, 0);
    };

    /* D-pad plus pattern, centered at (dpad_cx, dpad_cy) */
    push(dpad_cx - dpad_size / 2, dpad_cy - dpad_size - dpad_size / 2,
         dpad_size, dpad_size, MAC_KEY_UP,    "^");
    push(dpad_cx - dpad_size / 2, dpad_cy + dpad_size / 2,
         dpad_size, dpad_size, MAC_KEY_DOWN,  "v");
    push(dpad_cx - dpad_size - dpad_size / 2, dpad_cy - dpad_size / 2,
         dpad_size, dpad_size, MAC_KEY_LEFT,  "<");
    push(dpad_cx + dpad_size / 2, dpad_cy - dpad_size / 2,
         dpad_size, dpad_size, MAC_KEY_RIGHT, ">");

    /* Action buttons on the right. */
    const int btn_size = 120;
    const int right_cx = s_disp_w - 150 - btn_size;
    const int right_cy = s_disp_h - 30 - btn_size * 3 / 2;

    push(right_cx - btn_size / 2, right_cy - btn_size - btn_size / 2,
         btn_size, btn_size, MAC_KEY_ESC,     "Esc");
    push(right_cx + btn_size / 2, right_cy - btn_size / 2,
         btn_size, btn_size, MAC_KEY_RETURN,  "Ret");
    push(right_cx - btn_size - btn_size / 2, right_cy - btn_size / 2,
         btn_size, btn_size, MAC_KEY_OPTION,  "Opt");
    push(right_cx - btn_size / 2, right_cy + btn_size / 2,
         btn_size, btn_size, MAC_KEY_SPACE,   "Space");

    /* Gaming overlay bbox is the union of all keys (for tile composite
     * early-out only; touches outside the bbox still reach the Mac mouse). */
    rebuild_overlay_bbox();
}

static OverlayKey *active_layout(int *out_count)
{
    switch (s_mode) {
        case TOUCH_OVERLAY_KEYBOARD: *out_count = s_kb_count;   return s_kb_layout;
        case TOUCH_OVERLAY_GAMING:   *out_count = s_game_count; return s_game_layout;
        default:                     *out_count = 0;            return NULL;
    }
}

static void rebuild_overlay_bbox(void)
{
    int count = 0;
    OverlayKey *layout = active_layout(&count);
    if (!layout || count <= 0) {
        s_overlay_bbox_x = 0;
        s_overlay_bbox_y = 0;
        s_overlay_bbox_w = 0;
        s_overlay_bbox_h = 0;
        return;
    }

    int x0 = layout[0].x;
    int y0 = layout[0].y;
    int x1 = layout[0].x + layout[0].w;
    int y1 = layout[0].y + layout[0].h;
    for (int i = 1; i < count; ++i) {
        if (layout[i].x         < x0) x0 = layout[i].x;
        if (layout[i].y         < y0) y0 = layout[i].y;
        if (layout[i].x + layout[i].w > x1) x1 = layout[i].x + layout[i].w;
        if (layout[i].y + layout[i].h > y1) y1 = layout[i].y + layout[i].h;
    }
    s_overlay_bbox_x = x0;
    s_overlay_bbox_y = y0;
    s_overlay_bbox_w = x1 - x0;
    s_overlay_bbox_h = y1 - y0;
}

/* ============================================================================
 * Video-side integration (force tile redraw through write_dirty_tiles)
 * ============================================================================
 */

extern "C" void VideoMarkTilesDirtyRect(int px, int py, int pw, int ph);

static void mark_overlay_dirty(void)
{
    if (s_overlay_bbox_w <= 0 || s_overlay_bbox_h <= 0) return;
    VideoMarkTilesDirtyRect(s_overlay_bbox_x, s_overlay_bbox_y,
                            s_overlay_bbox_w, s_overlay_bbox_h);
}

static void mark_key_dirty(const OverlayKey *k)
{
    if (!k || k->w <= 0 || k->h <= 0) return;
    VideoMarkTilesDirtyRect(k->x, k->y, k->w, k->h);
}

/* ============================================================================
 * Hit test
 * ============================================================================
 */

static bool point_in_bbox(int x, int y)
{
    return (x >= s_overlay_bbox_x
            && y >= s_overlay_bbox_y
            && x <  s_overlay_bbox_x + s_overlay_bbox_w
            && y <  s_overlay_bbox_y + s_overlay_bbox_h);
}

static int hit_test_key(int x, int y)
{
    int count = 0;
    OverlayKey *layout = active_layout(&count);
    if (!layout) return -1;
    for (int i = 0; i < count; ++i) {
        const OverlayKey *k = &layout[i];
        if (x >= k->x && y >= k->y
            && x <  k->x + k->w
            && y <  k->y + k->h) {
            return i;
        }
    }
    return -1;
}

/* ============================================================================
 * Key press / release helpers
 * ============================================================================
 */

static void press_key_index(int idx)
{
    int count = 0;
    OverlayKey *layout = active_layout(&count);
    if (!layout || idx < 0 || idx >= count) return;
    OverlayKey *k = &layout[idx];
    if (k->held) return;

    if (k->flags & KFLAG_MODIFIER) {
        /* Tap a modifier to toggle latch. Key "held" for visual feedback
         * only while the finger is on it; the latch state persists. */
        if (k->latched) {
            InputKeyUp(k->mac_keycode);
            k->latched = false;
        } else {
            InputKeyDown(k->mac_keycode);
            k->latched = true;
        }
    } else {
        InputKeyDown(k->mac_keycode);
    }
    k->held = true;
    mark_key_dirty(k);
}

static void release_key_index(int idx)
{
    int count = 0;
    OverlayKey *layout = active_layout(&count);
    if (!layout || idx < 0 || idx >= count) return;
    OverlayKey *k = &layout[idx];
    if (!k->held) return;

    if (k->flags & KFLAG_MODIFIER) {
        /* Latched modifier: ADB state was flipped on press. Nothing to do
         * on finger-up except clear the visual "held" hint. */
    } else {
        InputKeyUp(k->mac_keycode);
    }
    k->held = false;
    mark_key_dirty(k);
}

static void release_all_keys(void)
{
    for (int pass = 0; pass < 2; ++pass) {
        OverlayKey *layout = (pass == 0) ? s_kb_layout : s_game_layout;
        int count = (pass == 0) ? s_kb_count : s_game_count;
        for (int i = 0; i < count; ++i) {
            OverlayKey *k = &layout[i];
            if (k->held && !(k->flags & KFLAG_MODIFIER)) {
                InputKeyUp(k->mac_keycode);
            }
            if ((k->flags & KFLAG_MODIFIER) && k->latched) {
                InputKeyUp(k->mac_keycode);
                k->latched = false;
            }
            k->held = false;
        }
    }
}

static void release_held_modifiers(void)
{
    for (int pass = 0; pass < 2; ++pass) {
        OverlayKey *layout = (pass == 0) ? s_kb_layout : s_game_layout;
        int count = (pass == 0) ? s_kb_count : s_game_count;
        for (int i = 0; i < count; ++i) {
            OverlayKey *k = &layout[i];
            if ((k->flags & KFLAG_MODIFIER) && k->latched) {
                InputKeyUp(k->mac_keycode);
                k->latched = false;
                k->held = false;
                mark_key_dirty(k);
            }
        }
    }
}

/* ============================================================================
 * Mode toggling
 * ============================================================================
 */

static void toggle_mode(TouchOverlayMode target)
{
    if (s_mode == target) {
        /* Same-mode retap hides the overlay. */
        release_all_keys();
        TouchOverlayMode old_mode = s_mode;
        s_mode = TOUCH_OVERLAY_NONE;
        (void)old_mode;
        mark_overlay_dirty();
        s_overlay_bbox_x = s_overlay_bbox_y = 0;
        s_overlay_bbox_w = s_overlay_bbox_h = 0;
        Serial.println("[OVERLAY] Hidden");
        return;
    }

    /* Switch overlays. If one was already up, clear it first. */
    release_all_keys();
    int old_bx = s_overlay_bbox_x;
    int old_by = s_overlay_bbox_y;
    int old_bw = s_overlay_bbox_w;
    int old_bh = s_overlay_bbox_h;

    s_mode = target;
    rebuild_overlay_bbox();
    /* Dirty both old and new regions so the previously-painted overlay
     * pixels get overwritten with fresh Mac content. */
    if (old_bw > 0 && old_bh > 0) {
        VideoMarkTilesDirtyRect(old_bx, old_by, old_bw, old_bh);
    }
    mark_overlay_dirty();

    Serial.printf("[OVERLAY] %s shown\n",
                  target == TOUCH_OVERLAY_KEYBOARD ? "Keyboard" : "Gaming");
}

/* ============================================================================
 * Slot matching
 * ============================================================================
 */

static int find_slot_by_hw_id(int hw_id)
{
    if (hw_id < 0) return -1;
    for (int i = 0; i < SLOT_CAPACITY; ++i) {
        if (s_slots[i].active && s_slots[i].hw_id == hw_id) return i;
    }
    return -1;
}

static int find_slot_by_proximity(int x, int y, int max_dist)
{
    int best = -1;
    int best_d2 = max_dist * max_dist;
    for (int i = 0; i < SLOT_CAPACITY; ++i) {
        if (!s_slots[i].active) continue;
        int dx = s_slots[i].last_x - x;
        int dy = s_slots[i].last_y - y;
        int d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = i;
        }
    }
    return best;
}

static int alloc_free_slot(void)
{
    for (int i = 0; i < SLOT_CAPACITY; ++i) {
        if (!s_slots[i].active) return i;
    }
    return -1;
}

/* ============================================================================
 * Mouse routing
 * ============================================================================
 */

static void convert_to_mac_coords(int px, int py, int *mac_x, int *mac_y)
{
    int mx = (px * s_mac_w) / (s_disp_w ? s_disp_w : 1);
    int my = (py * s_mac_h) / (s_disp_h ? s_disp_h : 1);
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= s_mac_w) mx = s_mac_w - 1;
    if (my >= s_mac_h) my = s_mac_h - 1;
    *mac_x = mx;
    *mac_y = my;
}

static int manhattan(int x1, int y1, int x2, int y2)
{
    int dx = x2 - x1;
    int dy = y2 - y1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx + dy;
}

static void mouse_on_down(int px, int py)
{
    int mx, my;
    convert_to_mac_coords(px, py, &mx, &my);
    ADBSetRelMouseMode(false);
    s_mouse_pressed = true;
    s_mouse_click_pending = true;
    s_mouse_is_dragging = false;
    s_mouse_start_x = mx;
    s_mouse_start_y = my;
    s_mouse_last_x = mx;
    s_mouse_last_y = my;
    ADBMouseMoved(mx, my);
}

static void mouse_on_move(int px, int py)
{
    if (!s_mouse_pressed) return;
    int mx, my;
    convert_to_mac_coords(px, py, &mx, &my);

    /* Deferred mouse-down: after the cursor had one cycle to reach the
     * new position, send the button event. */
    if (s_mouse_click_pending) {
        ADBMouseMoved(s_mouse_start_x, s_mouse_start_y);
        ADBMouseDown(0);
        s_mouse_click_pending = false;
    }

    if (!s_mouse_is_dragging) {
        int d = manhattan(mx, my, s_mouse_start_x, s_mouse_start_y);
        if (d > TAP_MOVEMENT_THRESHOLD) s_mouse_is_dragging = true;
    }

    if (s_mouse_is_dragging) {
        if (mx != s_mouse_last_x || my != s_mouse_last_y) {
            ADBMouseMoved(mx, my);
        }
    }
    s_mouse_last_x = mx;
    s_mouse_last_y = my;
}

static void mouse_on_up(void)
{
    if (!s_mouse_pressed) return;
    if (s_mouse_click_pending) {
        ADBMouseMoved(s_mouse_start_x, s_mouse_start_y);
        ADBMouseDown(0);
        s_mouse_click_pending = false;
    }
    ADBMouseUp(0);
    s_mouse_pressed = false;
    s_mouse_is_dragging = false;
}

static void mouse_cancel(void)
{
    if (!s_mouse_pressed) return;
    /* Cancel without issuing a click. Used when a multi-finger gesture
     * takes over. */
    if (!s_mouse_click_pending) {
        ADBMouseUp(0);
    }
    s_mouse_pressed = false;
    s_mouse_click_pending = false;
    s_mouse_is_dragging = false;
}

/* ============================================================================
 * Gesture detector
 * ============================================================================
 */

static void gesture_reset(void)
{
    s_gesture_peak = 0;
    s_gesture_first_ms = 0;
    s_gesture_drift_ok = true;
    s_gesture_triggered = false;
}

static void gesture_note_down(uint32_t now_ms, uint8_t new_count)
{
    /* First finger of a fresh batch - mark the start time. */
    if (s_gesture_peak == 0) {
        s_gesture_first_ms = now_ms;
        s_gesture_drift_ok = true;
    }
    if (new_count > s_gesture_peak) s_gesture_peak = new_count;
}

static void gesture_note_slot_drift(int slot_i)
{
    /* Any single finger that has moved more than GESTURE_MAX_DRIFT_PX
     * from its down position disqualifies the batch from being treated
     * as a tap. Per-finger drift is robust against the centroid swings
     * that happen when additional fingers touch down in other areas. */
    const OverlaySlot *s = &s_slots[slot_i];
    int dx = s->last_x - s->down_x;
    int dy = s->last_y - s->down_y;
    if (dx * dx + dy * dy > GESTURE_MAX_DRIFT_PX * GESTURE_MAX_DRIFT_PX) {
        s_gesture_drift_ok = false;
    }
}

static bool gesture_maybe_fire_on_lift(uint32_t now_ms)
{
    /* Called when all fingers have just lifted. Returns true if a toggle
     * was fired. */
    uint8_t peak = s_gesture_peak;
    bool drift_ok = s_gesture_drift_ok;
    uint32_t elapsed = now_ms - s_gesture_first_ms;

    gesture_reset();

    if (!drift_ok) return false;
    if (elapsed > GESTURE_MAX_HOLD_MS) return false;
    if (now_ms - s_last_toggle_ms < GESTURE_DEBOUNCE_MS) return false;

    if (peak == 3) {
        toggle_mode(TOUCH_OVERLAY_KEYBOARD);
        s_last_toggle_ms = now_ms;
        return true;
    }
    if (peak >= 4) {
        toggle_mode(TOUCH_OVERLAY_GAMING);
        s_last_toggle_ms = now_ms;
        return true;
    }
    return false;
}

/* ============================================================================
 * Main update
 * ============================================================================
 */

static void handle_slot_down(int slot_i, int px, int py, int hw_id, uint32_t now_ms)
{
    OverlaySlot *s = &s_slots[slot_i];
    s->active = true;
    s->last_x = s->down_x = px;
    s->last_y = s->down_y = py;
    s->down_ms = now_ms;
    s->hw_id = hw_id;
    s->key_index = -1;
    s->is_mouse = false;

    /* Routing:
     *   overlay visible && touch inside a key rect -> key
     *   otherwise                                  -> mouse (one active at a time)
     */
    if (TouchOverlay_IsVisible()) {
        int ki = hit_test_key(px, py);
        if (ki >= 0) {
            s->key_index = ki;
            press_key_index(ki);
            return;
        }
        /* Outside any key - if it's also outside the overlay bbox,
         * let it drive the mouse. Inside the bbox but not on a key
         * (small gaps), swallow it. */
        if (point_in_bbox(px, py)) {
            return;
        }
    }

    /* Mouse: only one finger can drive the mouse at a time. */
    bool mouse_in_use = false;
    for (int i = 0; i < SLOT_CAPACITY; ++i) {
        if (i == slot_i) continue;
        if (s_slots[i].active && s_slots[i].is_mouse) {
            mouse_in_use = true;
            break;
        }
    }
    if (!mouse_in_use) {
        s->is_mouse = true;
        mouse_on_down(px, py);
    }
}

static void handle_slot_move(int slot_i, int px, int py)
{
    OverlaySlot *s = &s_slots[slot_i];
    s->last_x = px;
    s->last_y = py;

    /* Feed per-slot drift into the gesture detector. */
    gesture_note_slot_drift(slot_i);

    if (s->key_index >= 0) {
        /* If the finger slides out of the key it started in, release
         * that key so the visual matches the ADB state. */
        int count = 0;
        OverlayKey *layout = active_layout(&count);
        if (layout && s->key_index < count) {
            const OverlayKey *k = &layout[s->key_index];
            if (px < k->x || py < k->y
                || px >= k->x + k->w || py >= k->y + k->h) {
                release_key_index(s->key_index);
                s->key_index = -1;
            }
        }
    } else if (s->is_mouse) {
        mouse_on_move(px, py);
    }
}

static void handle_slot_up(int slot_i)
{
    OverlaySlot *s = &s_slots[slot_i];
    if (!s->active) return;

    if (s->key_index >= 0) {
        release_key_index(s->key_index);
        s->key_index = -1;
    }
    if (s->is_mouse) {
        mouse_on_up();
        s->is_mouse = false;
    }
    s->active = false;
    s->hw_id = -1;
}

static void cancel_all_slots_for_gesture(void)
{
    /* A gesture fired - retract any touch-initiated state the current
     * batch produced so the user doesn't get stray keystrokes or clicks. */
    for (int i = 0; i < SLOT_CAPACITY; ++i) {
        OverlaySlot *s = &s_slots[i];
        if (!s->active) continue;
        if (s->key_index >= 0) {
            release_key_index(s->key_index);
            s->key_index = -1;
        }
        if (s->is_mouse) {
            mouse_cancel();
            s->is_mouse = false;
        }
    }
}

void TouchOverlay_Update(const BoardTouchMulti *multi)
{
    if (!s_touch_enabled) {
        /* If touch was disabled mid-gesture, make sure we clean up. */
        for (int i = 0; i < SLOT_CAPACITY; ++i) {
            if (s_slots[i].active) handle_slot_up(i);
        }
        return;
    }

    uint32_t now_ms = millis();
    BoardTouchMulti empty;
    empty.count = 0;
    const BoardTouchMulti *m = multi ? multi : &empty;

    /* If a gesture already toggled in this touch batch, ignore every
     * subsequent event until all fingers lift. */
    if (s_gesture_triggered) {
        if (m->count == 0) {
            /* All fingers up - end of batch. */
            s_gesture_triggered = false;
            gesture_reset();
            /* Also ensure all slots cleared (should already be). */
            for (int i = 0; i < SLOT_CAPACITY; ++i) {
                if (s_slots[i].active) handle_slot_up(i);
            }
        }
        return;
    }

    /* ------ Slot matching ------
     * Build a map: for each hardware point, which internal slot does it
     * correspond to? First try hw_id match; fall back to nearest-neighbour.
     */
    int assign[BOARD_TOUCH_MAX_POINTS];
    bool slot_seen[SLOT_CAPACITY];
    bool slot_is_new_this_frame[SLOT_CAPACITY];
    for (int i = 0; i < SLOT_CAPACITY; ++i) {
        slot_seen[i] = false;
        slot_is_new_this_frame[i] = false;
    }

    for (int i = 0; i < m->count; ++i) {
        assign[i] = find_slot_by_hw_id(m->p[i].id);
        if (assign[i] >= 0) {
            slot_seen[assign[i]] = true;
        }
    }
    for (int i = 0; i < m->count; ++i) {
        if (assign[i] >= 0) continue;
        /* Try nearest-neighbour match within 80 px among slots not yet
         * claimed this frame. */
        int best = -1, best_d2 = 80 * 80;
        for (int s = 0; s < SLOT_CAPACITY; ++s) {
            if (!s_slots[s].active || slot_seen[s]) continue;
            int dx = s_slots[s].last_x - m->p[i].x;
            int dy = s_slots[s].last_y - m->p[i].y;
            int d2 = dx * dx + dy * dy;
            if (d2 < best_d2) {
                best_d2 = d2;
                best = s;
            }
        }
        if (best >= 0) {
            assign[i] = best;
            slot_seen[best] = true;
        }
    }
    for (int i = 0; i < m->count; ++i) {
        if (assign[i] >= 0) continue;
        int fs = alloc_free_slot();
        if (fs < 0) continue;  /* more fingers than slots - drop */
        assign[i] = fs;
        slot_seen[fs] = true;
        slot_is_new_this_frame[fs] = true;
        handle_slot_down(fs, m->p[i].x, m->p[i].y, m->p[i].id, now_ms);
    }

    /* Count fingers actually down this frame (skipping any that failed to
     * allocate a slot) and feed the gesture detector. */
    uint8_t live_count = 0;
    for (int s = 0; s < SLOT_CAPACITY; ++s) {
        if (s_slots[s].active && slot_seen[s]) live_count++;
    }
    if (live_count > 0) gesture_note_down(now_ms, live_count);

    /* Move existing slots that were matched this frame. Skip slots we
     * JUST allocated this frame: handle_slot_down already seeded their
     * state, and calling handle_slot_move in the same frame collapses
     * the deferred-click frame delay (leading to spurious drag starts
     * when the finger jitters mid-tap). */
    for (int i = 0; i < m->count; ++i) {
        int si = assign[i];
        if (si < 0) continue;
        if (slot_is_new_this_frame[si]) continue;
        if (s_slots[si].active) {
            /* Update hw_id in case it changed (Waveshare index reshuffle) */
            s_slots[si].hw_id = m->p[i].id;
            handle_slot_move(si, m->p[i].x, m->p[i].y);
        }
    }

    /* Release any slots that no hardware point claimed. */
    for (int s = 0; s < SLOT_CAPACITY; ++s) {
        if (s_slots[s].active && !slot_seen[s]) {
            handle_slot_up(s);
        }
    }

    /* End-of-batch: all fingers up? Check gesture. */
    if (m->count == 0) {
        bool fired = gesture_maybe_fire_on_lift(now_ms);
        if (fired) {
            s_gesture_triggered = true;
            cancel_all_slots_for_gesture();
        }
    }
}

/* ============================================================================
 * Init / shutdown
 * ============================================================================
 */

void TouchOverlay_Init(int display_w, int display_h, int mac_w, int mac_h)
{
    s_disp_w = display_w;
    s_disp_h = display_h;
    s_mac_w  = mac_w;
    s_mac_h  = mac_h;

    s_mode = TOUCH_OVERLAY_NONE;
    for (int i = 0; i < SLOT_CAPACITY; ++i) {
        s_slots[i].active = false;
        s_slots[i].hw_id = -1;
        s_slots[i].key_index = -1;
        s_slots[i].is_mouse = false;
    }
    gesture_reset();
    s_last_toggle_ms = 0;

    build_keyboard_layout();
    build_gaming_layout();

    /* After building, no overlay is active -> empty bbox. */
    s_overlay_bbox_x = s_overlay_bbox_y = 0;
    s_overlay_bbox_w = s_overlay_bbox_h = 0;

    Serial.printf("[OVERLAY] Ready: display=%dx%d mac=%dx%d keys=%d/%d (kb/gaming)\n",
                  s_disp_w, s_disp_h, s_mac_w, s_mac_h,
                  s_kb_count, s_game_count);
}

void TouchOverlay_Shutdown(void)
{
    release_all_keys();
    mouse_cancel();
    for (int i = 0; i < SLOT_CAPACITY; ++i) {
        s_slots[i].active = false;
    }
    s_mode = TOUCH_OVERLAY_NONE;
}

void TouchOverlay_SetTouchEnabled(bool enabled)
{
    if (enabled == s_touch_enabled) return;
    s_touch_enabled = enabled;
    if (!enabled) {
        release_all_keys();
        mouse_cancel();
        for (int i = 0; i < SLOT_CAPACITY; ++i) {
            s_slots[i].active = false;
        }
        gesture_reset();
        mark_overlay_dirty();
    }
}

TouchOverlayMode TouchOverlay_GetMode(void) { return s_mode; }

bool TouchOverlay_IsVisible(void) { return s_mode != TOUCH_OVERLAY_NONE; }

/* ============================================================================
 * Stipple compositor
 * ============================================================================
 *
 * Called per-tile by the video task. `tile_x`/`tile_y` are the tile's
 * top-left in physical pixels; `pixels` is a row-major RGB565 buffer
 * sized tile_w * tile_h. The Mac framebuffer was already pixel-doubled
 * into this tile, so each Mac pixel occupies a 2x2 physical block.
 * Writing only the (even, even) sub-pixel of each block (relative to
 * physical coords) gives exact 25% coverage.
 */

static inline void put_subpixel(uint16_t *buf, int tw, int x, int y, uint16_t c)
{
    buf[(size_t)y * (size_t)tw + (size_t)x] = c;
}

/* Label scale: each source glyph pixel is drawn as a `scale x scale`
 * physical block. Because we still only write (even, even) sub-pixels
 * for the 25% stipple, higher scales don't increase coverage, they just
 * make the label larger. Scale 2 reads comfortably at arm's length;
 * scale 1 was the pre-tweak legacy look.                              */
#define OVERLAY_LABEL_SCALE     2

/* Render a single character glyph into the tile buffer at 25% stipple.
 * Glyph pen is at (pen_x, baseline_y) in physical pixels; baseline_y is
 * the physical y where CHICAGO_ASCENT lives above it. `scale` magnifies
 * each source pixel to a scale x scale physical block. Returns the pen
 * advance in physical pixels (already scaled). */
static int composite_glyph(uint16_t *buf,
                           int tile_x, int tile_y, int tw, int th,
                           int pen_x, int baseline_y, char c,
                           int scale, uint16_t color)
{
    unsigned char uc = (unsigned char)c;
    if (uc < CHICAGO_FIRST_CP || uc > CHICAGO_LAST_CP) uc = '?';
    const ChicagoGlyph *g = &CHICAGO_GLYPH_TABLE[uc - CHICAGO_FIRST_CP];

    /* Scaled glyph top-left in physical pixels. */
    int bm_x = pen_x + g->bbx_x * scale;
    int bm_y = baseline_y - g->bbx_y * scale - g->h * scale;
    int row_bytes = (g->w + 7) / 8;

    for (int gy = 0; gy < g->h; ++gy) {
        const uint8_t *row = &CHICAGO_GLYPH_BLOB[g->offset + gy * row_bytes];
        /* Physical y-range this source row covers. */
        int py0 = bm_y + gy * scale;
        int py1 = py0 + scale;

        for (int gx = 0; gx < g->w; ++gx) {
            uint8_t b = row[gx >> 3];
            if (!(b & (0x80 >> (gx & 7)))) continue;
            int px0 = bm_x + gx * scale;
            int px1 = px0 + scale;

            /* Paint every (even, even) sub-pixel within the scaled block
             * that falls inside the tile. Both scale=1 and scale=2 hit
             * exactly one sub-pixel per source pixel; larger scales add
             * more paint at 25% density.                                */
            for (int py = py0; py < py1; ++py) {
                if ((py & 1) != 0) continue;
                int rel_y = py - tile_y;
                if (rel_y < 0 || rel_y >= th) continue;
                for (int px = px0; px < px1; ++px) {
                    if ((px & 1) != 0) continue;
                    int rel_x = px - tile_x;
                    if (rel_x < 0 || rel_x >= tw) continue;
                    put_subpixel(buf, tw, rel_x, rel_y, color);
                }
            }
        }
    }
    return g->advance * scale;
}

static int measure_label(const char *s, int scale)
{
    int w = 0;
    while (*s) {
        unsigned char uc = (unsigned char)*s++;
        if (uc < CHICAGO_FIRST_CP || uc > CHICAGO_LAST_CP) uc = '?';
        w += CHICAGO_GLYPH_TABLE[uc - CHICAGO_FIRST_CP].advance;
    }
    return w * scale;
}

static void composite_key(uint16_t *buf, int tile_x, int tile_y, int tw, int th,
                          const OverlayKey *k)
{
    /* Intersect key with tile. */
    int kx0 = k->x;
    int ky0 = k->y;
    int kx1 = k->x + k->w;
    int ky1 = k->y + k->h;
    int ix0 = (kx0 > tile_x)      ? kx0 : tile_x;
    int iy0 = (ky0 > tile_y)      ? ky0 : tile_y;
    int ix1 = (kx1 < tile_x + tw) ? kx1 : tile_x + tw;
    int iy1 = (ky1 < tile_y + th) ? ky1 : tile_y + th;
    if (ix0 >= ix1 || iy0 >= iy1) return;

    bool is_filled = k->held || k->latched;
    /* Pressed (finger down): 50% checker of black. Latched modifier: same
     * but slightly different pattern to feel "locked" vs "momentary". */
    uint16_t fill_color = OVERLAY_COLOR_FG;
    bool fill_checker = k->held;     /* ((x^y)&1)==0 */
    bool fill_invchk = k->latched && !k->held;  /* ((x^y)&1)!=0 */

    /* 1. Key background: 25% white stipple on (even, even). */
    for (int py = iy0; py < iy1; ++py) {
        if ((py & 1) != 0) continue;
        int rel_y = py - tile_y;
        for (int px = ix0; px < ix1; ++px) {
            if ((px & 1) != 0) continue;
            int rel_x = px - tile_x;
            put_subpixel(buf, tw, rel_x, rel_y, OVERLAY_COLOR_BG);
        }
    }

    /* 2. Key border: 25% black stipple on the 1px frame. Because we only
     * draw (even, even) sub-pixels the "1px" line turns into a dashed
     * line that reads fine at viewing distance. */
    auto draw_border_pixel = [&](int px, int py) {
        if ((px & 1) != 0 || (py & 1) != 0) return;
        if (px < tile_x || py < tile_y
            || px >= tile_x + tw || py >= tile_y + th) return;
        put_subpixel(buf, tw, px - tile_x, py - tile_y, OVERLAY_COLOR_FG);
    };
    /* Top + bottom edges */
    for (int px = kx0; px < kx1; ++px) {
        draw_border_pixel(px, ky0);
        draw_border_pixel(px, ky1 - 1);
    }
    /* Left + right edges */
    for (int py = ky0; py < ky1; ++py) {
        draw_border_pixel(kx0, py);
        draw_border_pixel(kx1 - 1, py);
    }

    /* 3. Pressed / latched fill. */
    if (is_filled) {
        for (int py = iy0; py < iy1; ++py) {
            int rel_y = py - tile_y;
            for (int px = ix0; px < ix1; ++px) {
                int rel_x = px - tile_x;
                if (fill_checker) {
                    if (((px ^ py) & 1) != 0) continue;   /* 50% checker */
                } else if (fill_invchk) {
                    if (((px ^ py) & 1) == 0) continue;   /* opposite checker */
                }
                put_subpixel(buf, tw, rel_x, rel_y, fill_color);
            }
        }
    }

    /* 4. Label. Center the string in the key bounds, scaled up so each
     * source glyph pixel becomes a scale x scale block. Coverage stays
     * at 25% because we still paint only (even, even) sub-pixels.      */
    if (k->label && k->label[0]) {
        const int scale = OVERLAY_LABEL_SCALE;
        int label_w_px = measure_label(k->label, scale);
        int label_h_px = (CHICAGO_ASCENT + CHICAGO_DESCENT) * scale;
        int pen_x = k->x + (k->w - label_w_px) / 2;
        int baseline_y = k->y + (k->h + label_h_px) / 2 - CHICAGO_DESCENT * scale;
        /* Snap pen so glyph stipple lands on even-even sub-pixels. */
        pen_x &= ~1;
        baseline_y &= ~1;

        /* Quick reject: label's vertical band doesn't hit the tile. */
        int label_top = baseline_y - CHICAGO_ASCENT * scale;
        int label_bot = baseline_y + CHICAGO_DESCENT * scale;
        if (label_bot < tile_y || label_top >= tile_y + th) {
            /* skip label but still drew background/border above */
        } else {
            uint16_t lbl_color = (is_filled && k->held) ? OVERLAY_COLOR_BG
                                                        : OVERLAY_COLOR_FG;
            const char *p = k->label;
            while (*p) {
                /* Horizontal quick reject per-glyph is handled inside
                 * composite_glyph (pixel-level clipping); no per-glyph
                 * bbox computation needed here for short labels.        */
                pen_x += composite_glyph(buf, tile_x, tile_y, tw, th,
                                         pen_x, baseline_y, *p, scale, lbl_color);
                ++p;
            }
        }
    }
}

void TouchOverlay_CompositeTile(int tile_x, int tile_y,
                                int tile_w, int tile_h,
                                uint16_t *pixels)
{
    if (s_mode == TOUCH_OVERLAY_NONE) return;
    if (!pixels || tile_w <= 0 || tile_h <= 0) return;

    /* Early-out when the tile doesn't intersect the overlay bbox. */
    if (tile_x + tile_w <= s_overlay_bbox_x
        || tile_y + tile_h <= s_overlay_bbox_y
        || tile_x >= s_overlay_bbox_x + s_overlay_bbox_w
        || tile_y >= s_overlay_bbox_y + s_overlay_bbox_h) {
        return;
    }

    int count = 0;
    OverlayKey *layout = active_layout(&count);
    if (!layout) return;

    for (int i = 0; i < count; ++i) {
        OverlayKey *k = &layout[i];
        if (k->x + k->w <= tile_x
            || k->y + k->h <= tile_y
            || k->x >= tile_x + tile_w
            || k->y >= tile_y + tile_h) {
            continue;
        }
        composite_key(pixels, tile_x, tile_y, tile_w, tile_h, k);
    }
}
