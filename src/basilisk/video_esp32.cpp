/*
 *  video_esp32.cpp - Video/graphics emulation for ESP32 with M5GFX
 *
 *  BasiliskII ESP32 Port
 *
 *  Dual-core optimized: Video rendering runs on Core 0, CPU emulation on Core 1
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

// FreeRTOS for dual-core support
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Watchdog timer control
#include "esp_task_wdt.h"

#define DEBUG 1
#include "debug.h"

// Display configuration - 640x360 with 2x pixel doubling for 1280x720 display
#define MAC_SCREEN_WIDTH  640
#define MAC_SCREEN_HEIGHT 360
#define MAC_SCREEN_DEPTH  VDEPTH_8BIT  // 8-bit indexed color
#define PIXEL_SCALE       2            // 2x scaling to fill 1280x720

// Video task configuration
#define VIDEO_TASK_STACK_SIZE  8192
#define VIDEO_TASK_PRIORITY    1
#define VIDEO_TASK_CORE        0  // Run on Core 0, leaving Core 1 for CPU emulation

// Double-buffered frame buffers (allocated in PSRAM)
static uint8 *frame_buffer_write = NULL;    // CPU emulation writes here
static uint8 *frame_buffer_display = NULL;  // Video task reads from here
static uint32 frame_buffer_size = 0;

// Frame synchronization
static volatile bool frame_ready = false;
static SemaphoreHandle_t frame_mutex = NULL;
static portMUX_TYPE frame_spinlock = portMUX_INITIALIZER_UNLOCKED;

// Video task handle
static TaskHandle_t video_task_handle = NULL;
static volatile bool video_task_running = false;

// Palette (256 RGB entries) - dynamically allocated in PSRAM
static uint16 *palette_rgb565 = NULL;

// Display dimensions
static int display_width = 0;
static int display_height = 0;

// Canvas for rendering (used by video task)
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
 *  Convert RGB888 to RGB565 (with R/B swap for display)
 */
static inline uint16 rgb888_to_rgb565(uint8 r, uint8 g, uint8 b)
{
    // Swap R and B to match display's BGR565 format
    return ((b & 0xF8) << 8) | ((g & 0xFC) << 3) | (r >> 3);
}

/*
 *  Set palette for indexed color modes
 *  Thread-safe: uses spinlock since palette can be updated from CPU emulation
 */
void ESP32_monitor_desc::set_palette(uint8 *pal, int num)
{
    D(bug("[VIDEO] set_palette: %d entries\n", num));
    
    portENTER_CRITICAL(&frame_spinlock);
    for (int i = 0; i < num && i < 256; i++) {
        uint8 r = pal[i * 3 + 0];
        uint8 g = pal[i * 3 + 1];
        uint8 b = pal[i * 3 + 2];
        palette_rgb565[i] = rgb888_to_rgb565(r, g, b);
    }
    portEXIT_CRITICAL(&frame_spinlock);
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
 *  Render a frame buffer to the display canvas and push to screen
 *  Called from video task on Core 0
 */
static void renderFrameToDisplay(uint8 *src_buffer)
{
    if (!src_buffer || !canvas) return;
    
    // Convert 8-bit indexed to RGB565 and draw to canvas
    uint16 *dest = (uint16 *)canvas->getBuffer();
    if (!dest) return;
    
    // Take a snapshot of the palette (thread-safe)
    uint16 local_palette[256];
    portENTER_CRITICAL(&frame_spinlock);
    memcpy(local_palette, palette_rgb565, 256 * sizeof(uint16));
    portEXIT_CRITICAL(&frame_spinlock);
    
    // Optimized conversion: process 4 pixels at a time using 32-bit reads
    int total_pixels = MAC_SCREEN_WIDTH * MAC_SCREEN_HEIGHT;
    uint8 *src = src_buffer;
    uint16 *dst = dest;
    
    // Process 4 pixels at a time for better memory bandwidth
    int chunks = total_pixels >> 2;  // Divide by 4
    for (int i = 0; i < chunks; i++) {
        // Read 4 source pixels at once
        uint32 src4 = *((uint32 *)src);
        src += 4;
        
        // Convert each pixel through palette
        *dst++ = local_palette[src4 & 0xFF];
        *dst++ = local_palette[(src4 >> 8) & 0xFF];
        *dst++ = local_palette[(src4 >> 16) & 0xFF];
        *dst++ = local_palette[(src4 >> 24) & 0xFF];
    }
    
    // Handle remaining pixels (if width*height not divisible by 4)
    int remaining = total_pixels & 3;
    for (int i = 0; i < remaining; i++) {
        *dst++ = local_palette[*src++];
    }
    
    // Push canvas to display with 2x scaling
    // 640x360 * 2 = 1280x720 exactly fills the display
    canvas->pushRotateZoom(display_width / 2, display_height / 2, 0.0f, 
                           (float)PIXEL_SCALE, (float)PIXEL_SCALE);
}

/*
 *  Video rendering task - runs on Core 0
 *  Handles frame buffer conversion and display updates independently from CPU emulation
 *
 *  Uses copy-based double buffering: the CPU always writes to frame_buffer_write (via MacFrameBaseHost),
 *  and this task copies that buffer to frame_buffer_display before rendering.
 *  This avoids race conditions from swapping pointers while CPU is writing.
 */
static void videoRenderTask(void *param)
{
    UNUSED(param);
    Serial.println("[VIDEO] Video render task started on Core 0");
    
    // Unsubscribe this task from the watchdog timer
    // The video rendering can take variable time and shouldn't trigger WDT
    esp_task_wdt_delete(NULL);
    
    // Wait a moment for everything to initialize
    vTaskDelay(pdMS_TO_TICKS(100));
    
    while (video_task_running) {
        // Check if a new frame is ready
        if (frame_ready) {
            frame_ready = false;
            
            // Copy the current write buffer to the display buffer
            // This is safe because:
            // - MacFrameBaseHost always points to frame_buffer_write (never changes)
            // - CPU might write during copy, but that just means some scanlines are from
            //   the old frame and some from the new - acceptable visual tearing
            // - memcpy is fast (~230KB copy from PSRAM takes ~1ms at 360MHz)
            memcpy(frame_buffer_display, frame_buffer_write, frame_buffer_size);
            
            // Render the copied display buffer
            renderFrameToDisplay(frame_buffer_display);
        } else {
            // No new frame signal, but still refresh display periodically
            // Re-render the last copied display buffer
            if (frame_buffer_display) {
                renderFrameToDisplay(frame_buffer_display);
            }
        }
        
        // Always delay to allow IDLE task to run and prevent watchdog issues
        vTaskDelay(pdMS_TO_TICKS(16));  // ~60 FPS max, leaves time for other tasks
    }
    
    Serial.println("[VIDEO] Video render task exiting");
    vTaskDelete(NULL);
}

/*
 *  Start the video rendering task on Core 0
 */
static bool startVideoTask(void)
{
    // Create mutex for frame buffer synchronization
    frame_mutex = xSemaphoreCreateMutex();
    if (!frame_mutex) {
        Serial.println("[VIDEO] ERROR: Failed to create frame mutex!");
        return false;
    }
    
    video_task_running = true;
    
    // Create video task pinned to Core 0
    BaseType_t result = xTaskCreatePinnedToCore(
        videoRenderTask,
        "VideoTask",
        VIDEO_TASK_STACK_SIZE,
        NULL,
        VIDEO_TASK_PRIORITY,
        &video_task_handle,
        VIDEO_TASK_CORE
    );
    
    if (result != pdPASS) {
        Serial.println("[VIDEO] ERROR: Failed to create video task!");
        vSemaphoreDelete(frame_mutex);
        frame_mutex = NULL;
        video_task_running = false;
        return false;
    }
    
    Serial.printf("[VIDEO] Video task created on Core %d\n", VIDEO_TASK_CORE);
    return true;
}

/*
 *  Stop the video rendering task
 */
static void stopVideoTask(void)
{
    if (video_task_running) {
        video_task_running = false;
        
        // Give task time to exit
        vTaskDelay(pdMS_TO_TICKS(100));
        
        if (video_task_handle) {
            video_task_handle = NULL;
        }
    }
    
    if (frame_mutex) {
        vSemaphoreDelete(frame_mutex);
        frame_mutex = NULL;
    }
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
    
    // Allocate palette in PSRAM
    palette_rgb565 = (uint16 *)ps_malloc(256 * sizeof(uint16));
    if (!palette_rgb565) {
        Serial.println("[VIDEO] ERROR: Failed to allocate palette in PSRAM!");
        return false;
    }
    memset(palette_rgb565, 0, 256 * sizeof(uint16));
    
    // Allocate double-buffered frame buffers in PSRAM
    // For 640x360 @ 8-bit = 230,400 bytes per buffer
    frame_buffer_size = MAC_SCREEN_WIDTH * MAC_SCREEN_HEIGHT;
    
    frame_buffer_write = (uint8 *)ps_malloc(frame_buffer_size);
    if (!frame_buffer_write) {
        Serial.println("[VIDEO] ERROR: Failed to allocate write frame buffer in PSRAM!");
        return false;
    }
    
    frame_buffer_display = (uint8 *)ps_malloc(frame_buffer_size);
    if (!frame_buffer_display) {
        Serial.println("[VIDEO] ERROR: Failed to allocate display frame buffer in PSRAM!");
        free(frame_buffer_write);
        frame_buffer_write = NULL;
        return false;
    }
    
    Serial.printf("[VIDEO] Double-buffered frame buffers allocated:\n");
    Serial.printf("[VIDEO]   Write buffer:   %p (%d bytes)\n", frame_buffer_write, frame_buffer_size);
    Serial.printf("[VIDEO]   Display buffer: %p (%d bytes)\n", frame_buffer_display, frame_buffer_size);
    
    // Clear frame buffers to gray
    memset(frame_buffer_write, 0x80, frame_buffer_size);
    memset(frame_buffer_display, 0x80, frame_buffer_size);
    
    // Set up Mac frame buffer pointers (CPU writes to write buffer)
    MacFrameBaseHost = frame_buffer_write;
    MacFrameSize = frame_buffer_size;
    MacFrameLayout = FLAYOUT_DIRECT;
    
    // Create canvas for rendering
    canvas = new M5Canvas(&M5.Display);
    if (!canvas) {
        Serial.println("[VIDEO] ERROR: Failed to create canvas!");
        free(frame_buffer_write);
        free(frame_buffer_display);
        frame_buffer_write = NULL;
        frame_buffer_display = NULL;
        return false;
    }
    
    // Create sprite with 16-bit color depth
    canvas->setColorDepth(16);  // RGB565 output - set BEFORE createSprite
    canvas->createSprite(MAC_SCREEN_WIDTH, MAC_SCREEN_HEIGHT);
    
    // Clear canvas to gray (matching initial frame buffer state)
    // This prevents showing uninitialized memory (green/garbage)
    canvas->fillScreen(TFT_DARKGREY);
    
    // Push initial gray screen to display
    canvas->pushRotateZoom(display_width / 2, display_height / 2, 0.0f, 
                           (float)PIXEL_SCALE, (float)PIXEL_SCALE);
    
    Serial.println("[VIDEO] Canvas created and cleared");
    
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
    
    // Start video rendering task on Core 0
    if (!startVideoTask()) {
        Serial.println("[VIDEO] ERROR: Failed to start video task!");
        // Continue anyway - will fall back to synchronous refresh
    }
    
    Serial.printf("[VIDEO] Mac frame base: 0x%08X\n", MacFrameBaseMac);
    Serial.println("[VIDEO] VideoInit complete (dual-core mode)");
    
    return true;
}

/*
 *  Deinitialize video driver
 */
void VideoExit(void)
{
    Serial.println("[VIDEO] VideoExit");
    
    // Stop video task first
    stopVideoTask();
    
    if (canvas) {
        canvas->deleteSprite();
        delete canvas;
        canvas = NULL;
    }
    
    if (frame_buffer_write) {
        free(frame_buffer_write);
        frame_buffer_write = NULL;
    }
    
    if (frame_buffer_display) {
        free(frame_buffer_display);
        frame_buffer_display = NULL;
    }
    
    if (palette_rgb565) {
        free(palette_rgb565);
        palette_rgb565 = NULL;
    }
    
    // Clear monitors vector
    VideoMonitors.clear();
    
    if (the_monitor) {
        delete the_monitor;
        the_monitor = NULL;
    }
}

/*
 *  Signal that a new frame is ready for display
 *  Called from CPU emulation (Core 1) to notify video task (Core 0)
 *  This is non-blocking - CPU emulation continues immediately
 */
void VideoSignalFrameReady(void)
{
    // Simply set the flag - video task will pick it up
    // No blocking, no waiting for display to finish
    frame_ready = true;
}

/*
 *  Video refresh - legacy synchronous function
 *  Now just signals the video task instead of doing the work directly
 *  This allows CPU emulation to continue while video task handles rendering
 */
void VideoRefresh(void)
{
    if (!frame_buffer_write || !video_task_running) {
        // Fallback: if video task not running, do nothing
        // (or could do synchronous refresh as fallback)
        return;
    }
    
    // Signal video task that a new frame is ready
    VideoSignalFrameReady();
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
 *  Get pointer to frame buffer (the write buffer that CPU uses)
 */
uint8 *VideoGetFrameBuffer(void)
{
    return frame_buffer_write;
}

/*
 *  Get frame buffer size
 */
uint32 VideoGetFrameBufferSize(void)
{
    return frame_buffer_size;
}
