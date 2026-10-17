/*
 *  preboot_ui.h - Mac OS 8 "Platinum" drawing kit for the preboot screens
 *
 *  BasiliskII ESP32 Port
 *
 *  The preboot UI draws on a logical canvas at half the panel resolution
 *  (640x360 on the Tab5, 640x400 on the Waveshare), the same size as the
 *  emulated Mac screen. Each logical pixel is an 8-bit palette index; Present()
 *  pushes the changed area through the emulator's own 2x indexed tile path, so
 *  every logical pixel is exactly a 2x2 block on the panel.
 *
 *  Widget geometry and colors were measured from Mac OS 8.1 running in this
 *  emulator. Text uses Charcoal 12 (assets/fonts/charcoal12.bdf).
 *
 *  Drawing happens on one task (the boot GUI / main task). Present() and
 *  CaptureFrame() serialize on an internal lock.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "board_config.h"

namespace PrebootUI {

struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    Rect inset(int d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
};

/* Fixed palette entries. Indices >= kFirstDynamic are assigned on demand by
 * ColorFor() (artwork such as the splash icon). */
enum Color : uint8_t {
    kBlack, kWhite,
    kGray3A, kGray63, kGray73, kGray94, kGrayA5, kGrayB5, kGrayC5, kGrayCE,
    kGrayDE,      /* window frame and title bar face */
    kGrayE6,      /* dialog body, button face, menu bar */
    kGrayF7,
    kAccentLight, kAccent, kAccentShadow, kAccentDark,   /* lavender scroll thumb */
    kDesktop, kDesktopSpeck1, kDesktopSpeck2, kDesktopSpeck3,
    kRed, kDarkRed, kOrange, kYellow, kGreen, kDarkGreen, kBlue, kPurple,
    kFirstDynamic
};

constexpr int kScale = BOARD_PIXEL_SCALE;  /* panel pixels per logical pixel, each axis */
constexpr int kMenuBarHeight = 20;
constexpr int kTitleBarHeight = 20;  /* window frame top row to content top line */
constexpr int kLineHeight = 16;      /* Charcoal 12 line spacing */
constexpr int kCapHeight = 9;

/* ---- Canvas lifetime and output ---------------------------------------- */

/* Allocate the canvas (idempotent). False if memory is unavailable. */
bool Begin(void);
/* Free the canvas before the emulator claims memory and the panel. */
void Release(void);
bool Active(void);
int Width(void);
int Height(void);

/* Push everything that changed since the last Present() to the panel. */
void Present(void);
/* Force the next Present() to push the whole canvas. */
void InvalidateAll(void);

/* Copy the canvas as 8-bit indices plus an RGB565 palette (256 entries).
 * Safe from another task; waits for an in-progress Present(). */
bool CaptureFrame(uint8_t *pixels, size_t capacity, uint16_t *palette,
                  uint16_t *width, uint16_t *height);

/* Compare every panel pixel with the last presented canvas, doubled.
 * False when no canvas is active. */
bool VerifyPanel(uint32_t *checked, uint32_t *mismatched, int *first_x, int *first_y);

/* Palette index for an arbitrary RGB565 color (exact match or new entry;
 * nearest existing entry once the palette is full). */
uint8_t ColorFor(uint16_t rgb565);

/* ---- Primitives (logical coordinates, clipped) ------------------------- */

void Fill(const Rect &r, uint8_t color);
void Fill(int x, int y, int w, int h, uint8_t color);
void HLine(int x, int y, int w, uint8_t color);
void VLine(int x, int y, int h, uint8_t color);
void Pixel(int x, int y, uint8_t color);
void FrameRect(const Rect &r, uint8_t color);
/* Draw a w x h sprite of palette indices; 0xFF entries are transparent. */
void Sprite(int x, int y, int w, int h, const uint8_t *indices);
/* Draw RGB565 artwork with an optional 1-bit mask (MSB first rows). */
void Image565(int x, int y, int w, int h, const uint16_t *pixels,
              const uint8_t *mask = nullptr, int mask_row_bytes = 0);
/* Tile RGB565 artwork across a rectangle, anchored at the screen origin. */
void Tile565(const Rect &r, const uint16_t *tile, int tw, int th);

/* ---- Text (Charcoal 12) ------------------------------------------------ */

int TextWidth(const char *s);
/* Draw with the baseline at `baseline` (capitals occupy the 9 rows above). */
void Text(int x, int baseline, const char *s, uint8_t color);
/* Baseline that vertically centers capitals in a box of height h at y. */
inline int BaselineIn(int y, int h) { return y + (h - kCapHeight) / 2 + kCapHeight; }
/* Copy `s` into `out`, shortened with "..." so it fits in max_width. */
void FitText(const char *s, int max_width, char *out, size_t capacity);
enum Align { kLeft, kCenter, kRight };
void TextIn(const Rect &r, const char *s, uint8_t color, Align align = kLeft);

/* ---- Platinum widgets -------------------------------------------------- */

/* Original periwinkle desktop texture (Mac OS 8 default colors). */
void Desktop(const Rect &r);
/* The splash's 50% gray tile at 1:1 logical scale. */
void SplashBackground(void);
/* Menu bar with a rainbow Apple, decorative menu titles and a clock. */
void MenuBar(const char *clock_text);
/* Rounded black screen corners (drawn last, over everything). */
void ScreenCorners(void);

enum WindowStyle { kDialogWindow, kAlertWindow };
/* Movable-modal dialog (pinstriped title bar) or alert (no title).
 * Returns the content rectangle. */
Rect Window(const Rect &frame, const char *title, WindowStyle style = kDialogWindow);
/* Height of the frame above / around the content for a window style. */
Rect WindowContent(const Rect &frame, WindowStyle style = kDialogWindow);

enum ButtonState { kButtonNormal, kButtonPressed, kButtonDisabled };
/* Push button; a default button adds the 3-pixel ring outside `r`. */
void Button(const Rect &r, const char *label, ButtonState state, bool is_default = false);
/* Square key cap used by the on-screen keyboard. */
void KeyCap(const Rect &r, const char *label, bool pressed, bool latched = false);

void Checkbox(int x, int y, bool checked, bool pressed = false);
void Radio(int x, int y, bool selected, bool pressed = false);
/* Checkbox/radio with a Charcoal label; returns the touch rectangle. */
Rect LabeledCheckbox(int x, int y, const char *label, bool checked, bool pressed = false);
Rect LabeledRadio(int x, int y, const char *label, bool selected, bool pressed = false);

/* Etched primary group box with a title interrupting its top edge. */
void GroupBox(const Rect &r, const char *title);
/* Sunken white edit field with a black frame. */
void TextField(const Rect &r, const char *text, bool placeholder, bool focused = false);
/* Sunken white list frame; returns the interior. */
Rect ListFrame(const Rect &r);
/* Vertical scroll bar. `total` rows, `visible` rows shown from `first`.
 * Inactive (no thumb, gray arrows) when everything fits. */
void ScrollBar(const Rect &r, int total, int visible, int first,
               int pressed_arrow = 0 /* -1 up, +1 down */);
constexpr int kScrollBarWidth = 16;

/* ---- Small icons ------------------------------------------------------- */

enum Icon { kIconHardDisk, kIconCD, kIconFolder, kIconNone, kIconLock,
            kIconNote, kIconCaution, kIconStop, kIconCard, kIconMac };
/* 16x16 list icons and the lock (9x11); 32x32 for alert icons. */
/* `inverted` draws black outlines in white, for selected (black) list rows. */
void DrawIcon(Icon icon, int x, int y, bool inverted = false);
/* Signal bars (0..4) in a 15x12 box; `inverted` for selected list rows. */
void SignalBars(int x, int y, int bars, bool inverted = false);

} // namespace PrebootUI
