/*
 *  video_esp32.cpp - Video/graphics emulation for ESP32 with M5GFX
 *
 *  BasiliskII ESP32 Port
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "adb.h"
#include "prefs.h"
#include "video.h"
#include "video_defs.h"

#include <M5Unified.h>
#include <M5GFX.h>

#define DEBUG 1
#include "debug.h"

// Display configuration
#define MAC_SCREEN_WIDTH  640
#define MAC_SCREEN_HEIGHT 480
#define MAC_SCREEN_DEPTH  VDEPTH_8BIT  // 8-bit indexed color

// Frame buffer (allocated in PSRAM)
static uint8 *frame_buffer = NULL;
static uint32 frame_buffer_size = 0;

// Palette (256 RGB entries)
static uint16 palette_rgb565[256];

// Display dimensions
static int display_width = 0;
static int display_height = 0;

// Canvas for double buffering
static M5Canvas *canvas = NULL;

// Video mode info
static video_mode current_mode;

// Monitor descriptor for ESP32
class ESP32_monitor_desc : public monitor_desc {
public:
    ESP32_monitor_desc(const vector<video_mode> &available_modes, video_depth default_depth, uint32 default_id)
        : monitor_desc(available_modes, default_depth, default_id) {}
    
    virtual void switch_to_current_mode(void);
    virtual void set_palette(uint8 *pal, int num);
    virtual void set_gamma(uint8 *gamma, int num);
};

// Pointer to our monitor
static ESP32_monitor_desc *the_monitor = NULL;

/*
 *  Convert RGB888 to RGB565
 */
static inline uint16 rgb888_to_rgb565(uint8 r, uint8 g, uint8 b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

/*
 *  Set palette for indexed color modes
 */
void ESP32_monitor_desc::set_palette(uint8 *pal, int num)
{
    D(bug("[VIDEO] set_palette: %d entries\n", num));
    
    for (int i = 0; i < num && i < 256; i++) {
        uint8 r = pal[i * 3 + 0];
        uint8 g = pal[i * 3 + 1];
        uint8 b = pal[i * 3 + 2];
        palette_rgb565[i] = rgb888_to_rgb565(r, g, b);
    }
}

/*
 *  Set gamma table (same as palette for now)
 */
void ESP32_monitor_desc::set_gamma(uint8 *gamma, int num)
{
    // For indexed modes, gamma is applied through palette
    // For direct modes, we ignore gamma on ESP32 for simplicity
    UNUSED(gamma);
    UNUSED(num);
}

/*
 *  Switch to current video mode
 */
void ESP32_monitor_desc::switch_to_current_mode(void)
{
    D(bug("[VIDEO] switch_to_current_mode\n"));
    
    // Update frame buffer base address
    set_mac_frame_base(MacFrameBaseMac);
}

/*
 *  Initialize video driver
 */
bool VideoInit(bool classic)
{
    Serial.println("[VIDEO] VideoInit starting...");
    
    UNUSED(classic);
    
    // Get display dimensions
    display_width = M5.Display.width();
    display_height = M5.Display.height();
    Serial.printf("[VIDEO] Display size: %dx%d\n", display_width, display_height);
    
    // Allocate frame buffer in PSRAM
    // For 640x480 @ 8-bit = 307,200 bytes
    frame_buffer_size = MAC_SCREEN_WIDTH * MAC_SCREEN_HEIGHT;
    frame_buffer = (uint8 *)ps_malloc(frame_buffer_size);
    
    if (!frame_buffer) {
        Serial.println("[VIDEO] ERROR: Failed to allocate frame buffer in PSRAM!");
        return false;
    }
    
    Serial.printf("[VIDEO] Frame buffer allocated at %p (%d bytes)\n", 
                  frame_buffer, frame_buffer_size);
    
    // Clear frame buffer to gray
    memset(frame_buffer, 0x80, frame_buffer_size);
    
    // Set up Mac frame buffer pointers
    MacFrameBaseHost = frame_buffer;
    MacFrameSize = frame_buffer_size;
    MacFrameLayout = FLAYOUT_DIRECT;
    
    // Create canvas for rendering
    canvas = new M5Canvas(&M5.Display);
    if (!canvas) {
        Serial.println("[VIDEO] ERROR: Failed to create canvas!");
        free(frame_buffer);
        frame_buffer = NULL;
        return false;
    }
    
    // Create sprite with 16-bit color depth
    canvas->createSprite(MAC_SCREEN_WIDTH, MAC_SCREEN_HEIGHT);
    canvas->setColorDepth(16);  // RGB565 output
    
    Serial.println("[VIDEO] Canvas created");
    
    // Initialize default palette (grayscale with Mac-style inversion)
    // Classic Mac: 0=white, 255=black
    for (int i = 0; i < 256; i++) {
        uint8 gray = 255 - i;  // Invert for Mac palette
        palette_rgb565[i] = rgb888_to_rgb565(gray, gray, gray);
    }
    
    // Set up video mode
    current_mode.x = MAC_SCREEN_WIDTH;
    current_mode.y = MAC_SCREEN_HEIGHT;
    current_mode.resolution_id = 0x80;
    current_mode.depth = MAC_SCREEN_DEPTH;
    current_mode.bytes_per_row = MAC_SCREEN_WIDTH;  // 8-bit = 1 byte per pixel
    current_mode.user_data = 0;
    
    // Create video mode vector
    vector<video_mode> modes;
    modes.push_back(current_mode);
    
    // Create monitor descriptor
    the_monitor = new ESP32_monitor_desc(modes, MAC_SCREEN_DEPTH, 0x80);
    VideoMonitors.push_back(the_monitor);
    
    // Set Mac frame buffer base address
    the_monitor->set_mac_frame_base(MacFrameBaseMac);
    
    Serial.printf("[VIDEO] Mac frame base: 0x%08X\n", MacFrameBaseMac);
    Serial.println("[VIDEO] VideoInit complete");
    
    return true;
}

/*
 *  Deinitialize video driver
 */
void VideoExit(void)
{
    Serial.println("[VIDEO] VideoExit");
    
    if (canvas) {
        canvas->deleteSprite();
        delete canvas;
        canvas = NULL;
    }
    
    if (frame_buffer) {
        free(frame_buffer);
        frame_buffer = NULL;
    }
    
    // Clear monitors vector
    VideoMonitors.clear();
    
    if (the_monitor) {
        delete the_monitor;
        the_monitor = NULL;
    }
}

/*
 *  Video refresh - copy Mac frame buffer to display
 *  This is called periodically to update the screen
 */
void VideoRefresh(void)
{
    if (!frame_buffer || !canvas) return;
    
    // Convert 8-bit indexed to RGB565 and draw to canvas
    uint16 *dest = (uint16 *)canvas->getBuffer();
    if (!dest) return;
    
    // Convert frame buffer to RGB565 using palette
    for (int y = 0; y < MAC_SCREEN_HEIGHT; y++) {
        uint8 *src_row = frame_buffer + (y * MAC_SCREEN_WIDTH);
        uint16 *dest_row = dest + (y * MAC_SCREEN_WIDTH);
        
        for (int x = 0; x < MAC_SCREEN_WIDTH; x++) {
            dest_row[x] = palette_rgb565[src_row[x]];
        }
    }
    
    // Calculate position to center on display
    // 1280x720 display, 640x480 content = centered with black bars
    int x_offset = (display_width - MAC_SCREEN_WIDTH) / 2;
    int y_offset = (display_height - MAC_SCREEN_HEIGHT) / 2;
    
    // Push canvas to display
    canvas->pushSprite(x_offset, y_offset);
}

/*
 *  Set fullscreen mode (no-op on ESP32)
 */
void VideoQuitFullScreen(void)
{
    // No-op
}

/*
 *  Video interrupt handler (60Hz)
 */
void VideoInterrupt(void)
{
    // Trigger ADB interrupt for mouse/keyboard updates
    SetInterruptFlag(INTFLAG_ADB);
}

/*
 *  Get pointer to frame buffer
 */
uint8 *VideoGetFrameBuffer(void)
{
    return frame_buffer;
}

/*
 *  Get frame buffer size
 */
uint32 VideoGetFrameBufferSize(void)
{
    return frame_buffer_size;
}
