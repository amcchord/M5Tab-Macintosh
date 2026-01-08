/**
 * @file ui_init.cpp
 * Display Initialization Implementation
 * Uses M5Canvas for double-buffered rendering
 */

#include "ui_init.h"

// Static canvas for double-buffering
static M5Canvas canvas(&M5.Display);
static bool initialized = false;

void ui_init(void)
{
    if (initialized) {
        return;
    }
    
    Serial.println("[UI] Initializing display...");
    
    // Create canvas with PSRAM for better performance
    // For 720p, use a smaller canvas and render in sections if needed
    // The Tab5 has 32MB PSRAM, so we can use a full-size canvas
    canvas.setPsram(true);
    canvas.setColorDepth(16);  // RGB565
    canvas.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    
    // Fill with background color
    canvas.fillScreen(COLOR_BG_DARK);
    
    initialized = true;
    Serial.printf("[UI] Canvas created: %dx%d\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

M5Canvas* ui_get_canvas(void)
{
    return &canvas;
}

void ui_push(void)
{
    canvas.pushSprite(0, 0);
}
