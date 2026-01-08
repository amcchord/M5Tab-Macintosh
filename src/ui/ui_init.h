/**
 * @file ui_init.h
 * Display Initialization for Tab5 720p Display
 * Uses M5GFX for direct rendering (no LVGL)
 */

#ifndef UI_INIT_H
#define UI_INIT_H

#include <Arduino.h>
#include <M5Unified.h>
#include <M5GFX.h>

// Display dimensions for Tab5
#define DISPLAY_WIDTH  1280
#define DISPLAY_HEIGHT 720

// Custom colors (RGB565)
#define COLOR_BG_DARK      0x0861  // #0D1117
#define COLOR_BG_CARD      0x10C2  // #161B22
#define COLOR_BG_HEADER    0x2124  // #21262D
#define COLOR_TEXT_PRIMARY 0xFFFF  // #FFFFFF
#define COLOR_TEXT_DIM     0x8C92  // #8B949E
#define COLOR_ACCENT_CYAN  0x5D1F  // #58A6FF
#define COLOR_ACCENT_GREEN 0x3E6A  // #3FB950
#define COLOR_WARN_YELLOW  0xD4C4  // #D29922
#define COLOR_CRIT_RED     0xF8A9  // #F85149
#define COLOR_PURPLE       0xA39E  // #A371F7

/**
 * @brief Initialize display
 * Must be called after M5.begin()
 */
void ui_init(void);

/**
 * @brief Get the display canvas for drawing
 */
M5Canvas* ui_get_canvas(void);

/**
 * @brief Push canvas to display (double buffering)
 */
void ui_push(void);

#endif // UI_INIT_H
