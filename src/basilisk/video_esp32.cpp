/*
 *  video_esp32.cpp - Video/graphics emulation for ESP32-P4 with M5GFX
 *
 *  BasiliskII ESP32 Port
 *
 *  Dual-core optimized: Video rendering runs on Core 0, CPU emulation on Core 1
 *  
 *  OPTIMIZATIONS:
 *  1. 8-bit indexed frame buffer - minimizes PSRAM bandwidth
 *     - mac_frame_buffer: CPU writes here (8-bit indexed, 230KB)
 *     - Conversion to RGB565 happens at display write time
 *  2. Write-time dirty tracking - CPU marks tiles dirty as it writes
 *     - No per-frame comparison needed (eliminates ~460KB PSRAM traffic)
 *     - Dirty tiles tracked via atomic bitmap operations
 *  3. Tile-based partial updates - only updates changed screen regions
 *     - Screen divided into 16x9 grid of 40x40 pixel tiles (144 tiles total)
 *     - Only renders and pushes tiles that have changed
 *     - Falls back to full update if >80% of tiles are dirty (reduces API overhead)
 *     - Working buffers placed in internal SRAM for fast access
 *  
 *  TUNING PARAMETERS (defined below):
 *  - TILE_WIDTH/TILE_HEIGHT: Tile size in Mac pixels (40x40 default)
 *  - DIRTY_THRESHOLD_PERCENT: Threshold for switching to full update (80% default)
 *  - VIDEO_SIGNAL_INTERVAL: Frame rate target in main_esp32.cpp (~15 FPS)
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

// ESP-IDF memory attributes (DRAM_ATTR for internal SRAM placement)
#include "esp_attr.h"

// Cache control for DMA visibility
#if __has_include(<esp_cache.h>)
#include <esp_cache.h>
#define HAS_ESP_CACHE 1
#else
#define HAS_ESP_CACHE 0
#endif

// Cache line size for ESP32-P4 (64 bytes)
#define CACHE_LINE_SIZE 64

#define DEBUG 1
#include "debug.h"

// Display configuration - 640x360 with 2x pixel doubling for 1280x720 display
#define MAC_SCREEN_WIDTH  640
#define MAC_SCREEN_HEIGHT 360
#define MAC_SCREEN_DEPTH  VDEPTH_8BIT  // 8-bit indexed color
#define PIXEL_SCALE       2            // 2x scaling to fill 1280x720

// Physical display dimensions
#define DISPLAY_WIDTH     1280
#define DISPLAY_HEIGHT    720

// Tile-based dirty tracking configuration
// Tile size: 40x40 Mac pixels (80x80 display pixels after 2x scaling)
// Grid: 16 columns x 9 rows = 144 tiles total
// Coverage: 640x360 exactly (40*16=640, 40*9=360)
#define TILE_WIDTH        40
#define TILE_HEIGHT       40
#define TILES_X           16
#define TILES_Y           9
#define TOTAL_TILES       (TILES_X * TILES_Y)  // 144 tiles

// Dirty tile threshold - if more than this percentage of tiles are dirty,
// do a full update instead of partial
// NOTE: Set to 101 to ALWAYS use tile mode - tile updates are actually faster
// than full streaming even when all tiles are dirty, because tile mode uses
// double-buffered DMA while streaming mode processes rows sequentially
#define DIRTY_THRESHOLD_PERCENT  101

// Video task configuration
#define VIDEO_TASK_STACK_SIZE  8192
#define VIDEO_TASK_PRIORITY    1
#define VIDEO_TASK_CORE        0  // Run on Core 0, leaving Core 1 for CPU emulation

// Frame buffer for Mac emulation (CPU writes here)
static uint8 *mac_frame_buffer = NULL;
static uint32 frame_buffer_size = 0;

// Frame synchronization
static volatile bool frame_ready = false;
static portMUX_TYPE frame_spinlock = portMUX_INITIALIZER_UNLOCKED;

// Video task handle
static TaskHandle_t video_task_handle = NULL;
static volatile bool video_task_running = false;

// Palette (256 RGB565 entries) - in internal SRAM for fast access during rendering
// This is accessed for every pixel during video conversion
DRAM_ATTR static uint16 palette_rgb565[256];

// Flag to track if palette has changed - avoids unnecessary copies in video task
static volatile bool palette_changed = true;

// Dirty tile bitmap - in internal SRAM for fast access during video frame processing
DRAM_ATTR static uint32 dirty_tiles[(TOTAL_TILES + 31) / 32];          // Bitmap of dirty tiles (read by video task)

// Write-time dirty tracking bitmap - marked when CPU writes to framebuffer
// This is double-buffered to avoid race conditions between CPU writes and video task reads
DRAM_ATTR static uint32 write_dirty_tiles[(TOTAL_TILES + 31) / 32];    // Tiles dirtied by CPU writes

// Per-tile render lock bitmap - set while video task is snapshotting a tile
// If CPU tries to write while this is set, the tile is re-marked dirty for next frame
// This prevents torn data from race conditions during snapshot
DRAM_ATTR static uint32 tile_render_active[(TOTAL_TILES + 31) / 32];   // Tiles currently being rendered

// Double-buffered row buffers for streaming full-frame renders with async DMA
// Processes 4 Mac rows at a time (becomes 8 display rows with 2x scaling)
// Size: 1280 pixels * 8 rows * 2 bytes = 20,480 bytes (20KB) per buffer
// Double-buffering allows rendering to one buffer while DMA pushes the other
// In internal SRAM for fast access during full-frame renders
#define STREAMING_ROW_COUNT 8
DRAM_ATTR static uint16 streaming_row_buffer_a[DISPLAY_WIDTH * STREAMING_ROW_COUNT];
DRAM_ATTR static uint16 streaming_row_buffer_b[DISPLAY_WIDTH * STREAMING_ROW_COUNT];
static uint16 *render_buffer = streaming_row_buffer_a;
static uint16 *push_buffer = streaming_row_buffer_b;

static volatile bool force_full_update = true;               // Force full update on first frame or palette change
static int dirty_tile_count = 0;                             // Count of dirty tiles for threshold check

// Display dimensions (from M5.Display)
static int display_width = 0;
static int display_height = 0;

// Video mode info
static video_mode current_mode;

// Current video state cache - updated on mode switch for fast access during rendering
// These are used by the render loops and dirty tracking to handle different bit depths
static volatile video_depth current_depth = VDEPTH_8BIT;  // Current color depth
static volatile uint32 current_bytes_per_row = MAC_SCREEN_WIDTH;  // Bytes per row in frame buffer
static volatile int current_pixels_per_byte = 1;  // Pixels packed per byte (8=1bit, 4=2bit, 2=4bit, 1=8bit)
static volatile int current_bit_shift = 0;  // Bits to shift per pixel (7=1bit, 6=2bit, 4=4bit, 0=8bit)
static volatile uint8 current_pixel_mask = 0xFF;  // Mask for extracting pixel value

// ============================================================================
// Performance profiling counters (lightweight, always enabled)
// ============================================================================
static volatile uint32_t perf_detect_us = 0;        // Time to detect dirty tiles
static volatile uint32_t perf_render_us = 0;        // Time to render and push frame
static volatile uint32_t perf_frame_count = 0;      // Frames rendered
static volatile uint32_t perf_partial_count = 0;    // Partial updates
static volatile uint32_t perf_full_count = 0;       // Full updates
static volatile uint32_t perf_skip_count = 0;       // Skipped frames (no changes)
static volatile uint32_t perf_last_report_ms = 0;   // Last time stats were printed
#define PERF_REPORT_INTERVAL_MS 5000                // Report every 5 seconds

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
 *  Convert RGB888 to swap565 format for M5GFX writePixels
 *  
 *  M5GFX uses byte-swapped RGB565 (swap565_t):
 *  - Low byte:  RRRRRGGG (R5 in bits 7-3, G high 3 bits in bits 2-0)
 *  - High byte: GGGBBBBB (G low 3 bits in bits 7-5, B5 in bits 4-0)
 */
static inline uint16 rgb888_to_rgb565(uint8 r, uint8 g, uint8 b)
{
    // swap565 format: matches M5GFX's internal swap565() function
    return ((r >> 3) << 3 | (g >> 5)) | (((g >> 2) << 5 | (b >> 3)) << 8);
}

/*
 *  Set palette for indexed color modes
 *  Thread-safe: uses spinlock since palette can be updated from CPU emulation
 *  
 *  When palette changes, we force a full screen update since all pixels
 *  may look different even though the framebuffer data hasn't changed.
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
    palette_changed = true;
    portEXIT_CRITICAL(&frame_spinlock);
    
    // Force a full screen update since palette affects all pixels
    force_full_update = true;
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
 *  Helper to update the video state cache based on depth
 */
static void updateVideoStateCache(video_depth depth, uint32 bytes_per_row)
{
    current_depth = depth;
    current_bytes_per_row = bytes_per_row;
    
    switch (depth) {
        case VDEPTH_1BIT:
            current_pixels_per_byte = 8;
            current_bit_shift = 7;
            current_pixel_mask = 0x01;
            break;
        case VDEPTH_2BIT:
            current_pixels_per_byte = 4;
            current_bit_shift = 6;
            current_pixel_mask = 0x03;
            break;
        case VDEPTH_4BIT:
            current_pixels_per_byte = 2;
            current_bit_shift = 4;
            current_pixel_mask = 0x0F;
            break;
        case VDEPTH_8BIT:
        default:
            current_pixels_per_byte = 1;
            current_bit_shift = 0;
            current_pixel_mask = 0xFF;
            break;
    }
    
    Serial.printf("[VIDEO] Mode cache updated: depth=%d, bpr=%d, ppb=%d\n", 
                  (int)depth, (int)bytes_per_row, current_pixels_per_byte);
}

/*
 *  Initialize palette with default colors for the specified depth
 *  
 *  This sets up appropriate default colors:
 *  - 1-bit: Black and white (standard Mac B&W)
 *  - 2-bit: 4-color grayscale (white, light gray, dark gray, black)
 *  - 4-bit: Classic Mac 16-color palette
 *  - 8-bit: Mac 256-color palette (6x6x6 color cube + grayscale ramp)
 *  
 *  Classic Mac convention: index 0 = white, highest index = black
 */
static void initDefaultPalette(video_depth depth)
{
    portENTER_CRITICAL(&frame_spinlock);
    
    switch (depth) {
        case VDEPTH_1BIT:
            // 1-bit: Black and white
            // Index 0 = white, Index 1 = black
            palette_rgb565[0] = rgb888_to_rgb565(255, 255, 255);  // White
            palette_rgb565[1] = rgb888_to_rgb565(0, 0, 0);        // Black
            Serial.println("[VIDEO] Initialized 1-bit B&W palette");
            break;
            
        case VDEPTH_2BIT:
            // 2-bit: 4 levels of gray
            // Index 0 = white, Index 3 = black
            palette_rgb565[0] = rgb888_to_rgb565(255, 255, 255);  // White
            palette_rgb565[1] = rgb888_to_rgb565(170, 170, 170);  // Light gray
            palette_rgb565[2] = rgb888_to_rgb565(85, 85, 85);     // Dark gray
            palette_rgb565[3] = rgb888_to_rgb565(0, 0, 0);        // Black
            Serial.println("[VIDEO] Initialized 2-bit grayscale palette");
            break;
            
        case VDEPTH_4BIT:
            // 4-bit: Classic Mac 16-color palette
            // This matches the standard Mac 16-color CLUT
            {
                static const uint8 mac16[16][3] = {
                    {255, 255, 255},  // 0: White
                    {255, 255, 0},    // 1: Yellow
                    {255, 102, 0},    // 2: Orange
                    {221, 0, 0},      // 3: Red
                    {255, 0, 153},    // 4: Magenta
                    {51, 0, 153},     // 5: Purple
                    {0, 0, 204},      // 6: Blue
                    {0, 153, 255},    // 7: Cyan
                    {0, 170, 0},      // 8: Green
                    {0, 102, 0},      // 9: Dark Green
                    {102, 51, 0},     // 10: Brown
                    {153, 102, 51},   // 11: Tan
                    {187, 187, 187},  // 12: Light Gray
                    {136, 136, 136},  // 13: Medium Gray
                    {68, 68, 68},     // 14: Dark Gray
                    {0, 0, 0}         // 15: Black
                };
                for (int i = 0; i < 16; i++) {
                    palette_rgb565[i] = rgb888_to_rgb565(mac16[i][0], mac16[i][1], mac16[i][2]);
                }
            }
            Serial.println("[VIDEO] Initialized 4-bit 16-color palette");
            break;
            
        case VDEPTH_8BIT:
        default:
            // 8-bit: Mac 256-color palette
            // Uses a 6x6x6 color cube (216 colors) plus grayscale ramp
            // This provides a good default color palette for 256-color mode
            {
                int idx = 0;
                
                // First, create a 6x6x6 color cube (216 colors)
                // This gives 6 levels each of R, G, B: 0, 51, 102, 153, 204, 255
                for (int r = 0; r < 6; r++) {
                    for (int g = 0; g < 6; g++) {
                        for (int b = 0; b < 6; b++) {
                            uint8 rv = r * 51;
                            uint8 gv = g * 51;
                            uint8 bv = b * 51;
                            palette_rgb565[idx++] = rgb888_to_rgb565(rv, gv, bv);
                        }
                    }
                }
                
                // Fill remaining 40 entries with a grayscale ramp
                // This provides smooth grays for UI elements
                for (int i = 0; i < 40; i++) {
                    uint8 gray = (i * 255) / 39;
                    palette_rgb565[idx++] = rgb888_to_rgb565(gray, gray, gray);
                }
            }
            Serial.println("[VIDEO] Initialized 8-bit 256-color palette");
            break;
    }
    
    portEXIT_CRITICAL(&frame_spinlock);
    
    // Force a full screen update since palette changed
    force_full_update = true;
}

/*
 *  Switch to current video mode
 */
void ESP32_monitor_desc::switch_to_current_mode(void)
{
    const video_mode &mode = get_current_mode();
    D(bug("[VIDEO] switch_to_current_mode: %dx%d, depth=%d, bpr=%d\n", 
          mode.x, mode.y, mode.depth, mode.bytes_per_row));
    
    // Update the video state cache for rendering
    updateVideoStateCache(mode.depth, mode.bytes_per_row);
    
    // Initialize default palette for this depth
    // MacOS will set its own palette shortly after, but this ensures
    // the display looks reasonable immediately after the mode switch
    initDefaultPalette(mode.depth);
    
    // Update frame buffer base address
    set_mac_frame_base(MacFrameBaseMac);
    
    // Force a full screen update on mode change (already done by initDefaultPalette)
    force_full_update = true;
}

// ============================================================================
// Packed pixel decoding helpers for 1/2/4-bit modes
// ============================================================================

/*
 *  Decode a row of packed pixels to 8-bit palette indices
 *  
 *  In packed modes, multiple pixels are stored per byte:
 *  - 1-bit: 8 pixels per byte, MSB first (bit 7 = leftmost pixel)
 *  - 2-bit: 4 pixels per byte, MSB first (bits 7-6 = leftmost pixel)
 *  - 4-bit: 2 pixels per byte, MSB first (bits 7-4 = leftmost pixel)
 *  - 8-bit: 1 pixel per byte (no decoding needed)
 *  
 *  @param src       Source row in frame buffer (packed)
 *  @param dst       Destination buffer for 8-bit indices (must hold width pixels)
 *  @param width     Number of pixels to decode
 *  @param depth     Current video depth
 */
static void decodePackedRow(const uint8 *src, uint8 *dst, int width, video_depth depth)
{
    switch (depth) {
        case VDEPTH_1BIT: {
            // 8 pixels per byte, MSB first
            for (int x = 0; x < width; x++) {
                int byte_idx = x / 8;
                int bit_idx = 7 - (x % 8);  // MSB first
                dst[x] = (src[byte_idx] >> bit_idx) & 0x01;
            }
            break;
        }
        case VDEPTH_2BIT: {
            // 4 pixels per byte, MSB first
            for (int x = 0; x < width; x++) {
                int byte_idx = x / 4;
                int shift = 6 - ((x % 4) * 2);  // MSB first: 6, 4, 2, 0
                dst[x] = (src[byte_idx] >> shift) & 0x03;
            }
            break;
        }
        case VDEPTH_4BIT: {
            // 2 pixels per byte, MSB first
            for (int x = 0; x < width; x++) {
                int byte_idx = x / 2;
                int shift = (x % 2 == 0) ? 4 : 0;  // MSB first: high nibble, low nibble
                dst[x] = (src[byte_idx] >> shift) & 0x0F;
            }
            break;
        }
        case VDEPTH_8BIT:
        default:
            // Direct copy, no decoding needed
            memcpy(dst, src, width);
            break;
    }
}

/*
 *  Get pixel index from packed framebuffer at given (x, y) coordinate
 *  Used for single-pixel access when full row decode is overkill
 *  
 *  @param fb        Frame buffer pointer
 *  @param x         X coordinate (pixel)
 *  @param y         Y coordinate (row)
 *  @param bpr       Bytes per row
 *  @param depth     Current video depth
 *  @return          8-bit palette index for the pixel
 */
static inline uint8 getPackedPixel(const uint8 *fb, int x, int y, uint32 bpr, video_depth depth)
{
    const uint8 *row = fb + y * bpr;
    
    switch (depth) {
        case VDEPTH_1BIT: {
            int byte_idx = x / 8;
            int bit_idx = 7 - (x % 8);
            return (row[byte_idx] >> bit_idx) & 0x01;
        }
        case VDEPTH_2BIT: {
            int byte_idx = x / 4;
            int shift = 6 - ((x % 4) * 2);
            return (row[byte_idx] >> shift) & 0x03;
        }
        case VDEPTH_4BIT: {
            int byte_idx = x / 2;
            int shift = (x % 2 == 0) ? 4 : 0;
            return (row[byte_idx] >> shift) & 0x0F;
        }
        case VDEPTH_8BIT:
        default:
            return row[x];
    }
}

/*
 *  Check if a specific tile is marked as dirty
 */
static inline bool isTileDirty(int tile_idx)
{
    return (dirty_tiles[tile_idx / 32] & (1 << (tile_idx % 32))) != 0;
}

/*
 *  Tile render lock functions - used to prevent race conditions during snapshot
 *  When a tile is being rendered (snapshotted), CPU writes to that tile will
 *  be deferred to the next frame by re-marking the tile dirty.
 */
static inline void setTileRenderActive(int tile_idx)
{
    __atomic_or_fetch(&tile_render_active[tile_idx / 32], (1u << (tile_idx % 32)), __ATOMIC_RELEASE);
}

static inline void clearTileRenderActive(int tile_idx)
{
    __atomic_and_fetch(&tile_render_active[tile_idx / 32], ~(1u << (tile_idx % 32)), __ATOMIC_RELEASE);
}

static inline bool isTileRenderActive(int tile_idx)
{
    return (__atomic_load_n(&tile_render_active[tile_idx / 32], __ATOMIC_ACQUIRE) & (1u << (tile_idx % 32))) != 0;
}

/*
 *  Mark a tile as dirty at write-time (called from frame buffer put functions)
 *  This is MUCH faster than per-frame comparison as it only runs on actual writes.
 *  
 *  Handles packed pixel modes by mapping byte offset to pixel coordinates using
 *  current_bytes_per_row and current_pixels_per_byte.
 *  
 *  RACE CONDITION HANDLING:
 *  If the video task is currently rendering (snapshotting) this tile, the snapshot
 *  might contain torn data. However, we unconditionally mark the tile dirty here,
 *  which ensures it will be re-rendered cleanly in the next frame. Combined with
 *  the tile_render_active lock in renderAndPushDirtyTiles(), this provides
 *  eventual consistency - a torn frame may appear briefly but will be fixed
 *  within one frame interval (42ms).
 *  
 *  @param offset  Byte offset into the Mac framebuffer
 */
void VideoMarkDirtyOffset(uint32 offset)
{
    if (offset >= frame_buffer_size) return;
    
    // Get current bytes per row (volatile)
    uint32 bpr = current_bytes_per_row;
    int ppb = current_pixels_per_byte;
    
    // Calculate row from byte offset
    int y = offset / bpr;
    if (y >= MAC_SCREEN_HEIGHT) return;
    
    // Calculate byte position within row
    int byte_in_row = offset % bpr;
    
    // Calculate pixel range that this byte affects
    int pixel_start = byte_in_row * ppb;
    int pixel_end = pixel_start + ppb - 1;
    
    // Clamp to screen width
    if (pixel_start >= MAC_SCREEN_WIDTH) return;
    if (pixel_end >= MAC_SCREEN_WIDTH) pixel_end = MAC_SCREEN_WIDTH - 1;
    
    // Calculate tile range
    int tile_x_start = pixel_start / TILE_WIDTH;
    int tile_x_end = pixel_end / TILE_WIDTH;
    int tile_y = y / TILE_HEIGHT;
    
    // Mark all affected tiles dirty (unconditionally - even if being rendered)
    // This ensures tiles written during rendering are re-rendered next frame
    for (int tile_x = tile_x_start; tile_x <= tile_x_end; tile_x++) {
        int tile_idx = tile_y * TILES_X + tile_x;
        if (tile_idx < TOTAL_TILES) {
            __atomic_or_fetch(&write_dirty_tiles[tile_idx / 32], (1u << (tile_idx % 32)), __ATOMIC_RELAXED);
        }
    }
}

/*
 *  Mark a range of tiles as dirty at write-time
 *  Used for multi-byte writes (lput, wput)
 *  
 *  For packed pixel modes, a multi-byte write can span many pixels across
 *  potentially multiple rows and tiles.
 *  
 *  See VideoMarkDirtyOffset() for race condition handling notes.
 *  
 *  @param offset  Starting byte offset into the Mac framebuffer
 *  @param size    Number of bytes being written
 */
void VideoMarkDirtyRange(uint32 offset, uint32 size)
{
    if (offset >= frame_buffer_size) return;
    
    // Clamp size to framebuffer bounds
    if (offset + size > frame_buffer_size) {
        size = frame_buffer_size - offset;
    }
    
    // Get current bytes per row (volatile)
    uint32 bpr = current_bytes_per_row;
    int ppb = current_pixels_per_byte;
    
    // Calculate start and end rows
    int start_y = offset / bpr;
    int end_y = (offset + size - 1) / bpr;
    
    // For small writes or writes within a single row, just mark individual bytes
    if (end_y == start_y && size <= 4) {
        // Simple case: mark start and end bytes
        VideoMarkDirtyOffset(offset);
        if (size > 1) {
            VideoMarkDirtyOffset(offset + size - 1);
        }
        return;
    }
    
    // For larger writes spanning multiple rows, calculate affected tile columns
    // This is more efficient than marking every byte individually
    int start_byte_in_row = offset % bpr;
    int end_byte_in_row = (offset + size - 1) % bpr;
    
    // Calculate pixel columns affected
    int pixel_col_start = start_byte_in_row * ppb;
    int pixel_col_end = (end_byte_in_row + 1) * ppb - 1;
    
    // For writes spanning multiple rows, the middle rows are fully affected
    // So we need to consider columns from 0 to end for complex cases
    if (end_y > start_y) {
        // Multi-row write: could affect any column
        pixel_col_start = 0;
        pixel_col_end = MAC_SCREEN_WIDTH - 1;
    }
    
    // Calculate tile ranges
    int tile_x_start = pixel_col_start / TILE_WIDTH;
    int tile_x_end = pixel_col_end / TILE_WIDTH;
    if (tile_x_end >= TILES_X) tile_x_end = TILES_X - 1;
    
    int tile_y_start = start_y / TILE_HEIGHT;
    int tile_y_end = end_y / TILE_HEIGHT;
    if (tile_y_end >= TILES_Y) tile_y_end = TILES_Y - 1;
    
    // Mark all affected tiles dirty
    for (int tile_y = tile_y_start; tile_y <= tile_y_end; tile_y++) {
        for (int tile_x = tile_x_start; tile_x <= tile_x_end; tile_x++) {
            int tile_idx = tile_y * TILES_X + tile_x;
            __atomic_or_fetch(&write_dirty_tiles[tile_idx / 32], (1u << (tile_idx % 32)), __ATOMIC_RELAXED);
        }
    }
}

/*
 *  Collect write-dirty tiles into the render dirty bitmap and clear write bitmap
 *  Returns the number of dirty tiles
 *  Called at the start of each video frame
 */
static int collectWriteDirtyTiles(void)
{
    int count = 0;
    
    // Copy write_dirty_tiles to dirty_tiles and count
    for (int i = 0; i < (TOTAL_TILES + 31) / 32; i++) {
        // Atomically read and clear the write dirty bitmap
        uint32 bits = __atomic_exchange_n(&write_dirty_tiles[i], 0, __ATOMIC_RELAXED);
        dirty_tiles[i] = bits;
        
        // Count set bits (popcount)
        while (bits) {
            count += (bits & 1);
            bits >>= 1;
        }
    }
    
    return count;
}

/*
 *  Copy a single tile's source data from framebuffer to a snapshot buffer
 *  This creates a consistent snapshot of the tile to avoid race conditions
 *  when the CPU is writing to the framebuffer while we're rendering.
 *  
 *  For packed pixel modes, decodes to 8-bit indices in the snapshot buffer.
 *  
 *  @param src_buffer     Mac framebuffer (may be packed or 8-bit)
 *  @param tile_x         Tile column index (0 to TILES_X-1)
 *  @param tile_y         Tile row index (0 to TILES_Y-1)
 *  @param snapshot       Output buffer (TILE_WIDTH * TILE_HEIGHT bytes, always 8-bit indices)
 */
static void snapshotTile(uint8 *src_buffer, int tile_x, int tile_y, uint8 *snapshot)
{
    int src_start_x = tile_x * TILE_WIDTH;
    int src_start_y = tile_y * TILE_HEIGHT;
    
    // Get current depth and bytes per row (volatile, so copy locally)
    video_depth depth = current_depth;
    uint32 bpr = current_bytes_per_row;
    
    // Copy and decode each row of the tile to the contiguous snapshot buffer
    uint8 *dst = snapshot;
    
    if (depth == VDEPTH_8BIT) {
        // 8-bit mode: direct copy, no decoding needed
        for (int row = 0; row < TILE_HEIGHT; row++) {
            uint8 *src = src_buffer + (src_start_y + row) * bpr + src_start_x;
            memcpy(dst, src, TILE_WIDTH);
            dst += TILE_WIDTH;
        }
    } else {
        // Packed mode: need to decode pixels
        // For each row, extract the tile's pixel range from the packed source
        for (int row = 0; row < TILE_HEIGHT; row++) {
            uint8 *src_row = src_buffer + (src_start_y + row) * bpr;
            
            // Decode TILE_WIDTH pixels starting at src_start_x
            for (int x = 0; x < TILE_WIDTH; x++) {
                int pixel_x = src_start_x + x;
                
                switch (depth) {
                    case VDEPTH_1BIT: {
                        int byte_idx = pixel_x / 8;
                        int bit_idx = 7 - (pixel_x % 8);
                        *dst++ = (src_row[byte_idx] >> bit_idx) & 0x01;
                        break;
                    }
                    case VDEPTH_2BIT: {
                        int byte_idx = pixel_x / 4;
                        int shift = 6 - ((pixel_x % 4) * 2);
                        *dst++ = (src_row[byte_idx] >> shift) & 0x03;
                        break;
                    }
                    case VDEPTH_4BIT: {
                        int byte_idx = pixel_x / 2;
                        int shift = (pixel_x % 2 == 0) ? 4 : 0;
                        *dst++ = (src_row[byte_idx] >> shift) & 0x0F;
                        break;
                    }
                    default:
                        *dst++ = src_row[pixel_x];
                        break;
                }
            }
        }
    }
}

/*
 *  Render a tile from a contiguous snapshot buffer (not from framebuffer)
 *  This ensures we render from consistent data that won't change mid-render.
 *  
 *  @param snapshot        Tile snapshot buffer (TILE_WIDTH * TILE_HEIGHT bytes, contiguous)
 *  @param local_palette   Pre-copied palette for thread safety
 *  @param out_buffer      Output buffer for RGB565 pixels
 */
static void renderTileFromSnapshot(uint8 *snapshot, uint16 *local_palette, uint16 *out_buffer)
{
    int tile_pixel_width = TILE_WIDTH * PIXEL_SCALE;  // 80 pixels
    
    uint8 *src = snapshot;
    uint16 *out = out_buffer;
    
    // Process each row of the Mac tile
    for (int row = 0; row < TILE_HEIGHT; row++) {
        // Output row pointers (two rows for 2x vertical scaling)
        uint16 *dst_row0 = out;
        uint16 *dst_row1 = out + tile_pixel_width;
        
        // Process 4 pixels at a time for better memory bandwidth
        int x = 0;
        for (; x < TILE_WIDTH - 3; x += 4) {
            // Read 4 source pixels at once (32-bit read)
            uint32 src4 = *((uint32 *)src);
            src += 4;
            
            // Convert each pixel through palette and write 2x2 scaled
            uint16 c0 = local_palette[src4 & 0xFF];
            uint16 c1 = local_palette[(src4 >> 8) & 0xFF];
            uint16 c2 = local_palette[(src4 >> 16) & 0xFF];
            uint16 c3 = local_palette[(src4 >> 24) & 0xFF];
            
            // Write to row 0 (2 pixels per source pixel)
            dst_row0[0] = c0; dst_row0[1] = c0;
            dst_row0[2] = c1; dst_row0[3] = c1;
            dst_row0[4] = c2; dst_row0[5] = c2;
            dst_row0[6] = c3; dst_row0[7] = c3;
            
            // Write to row 1 (duplicate of row 0)
            dst_row1[0] = c0; dst_row1[1] = c0;
            dst_row1[2] = c1; dst_row1[3] = c1;
            dst_row1[4] = c2; dst_row1[5] = c2;
            dst_row1[6] = c3; dst_row1[7] = c3;
            
            dst_row0 += 8;
            dst_row1 += 8;
        }
        
        // Handle remaining pixels (TILE_WIDTH=40 is divisible by 4, so this rarely runs)
        for (; x < TILE_WIDTH; x++) {
            uint16 c = local_palette[*src++];
            dst_row0[0] = c; dst_row0[1] = c;
            dst_row1[0] = c; dst_row1[1] = c;
            dst_row0 += 2;
            dst_row1 += 2;
        }
        
        // Move output pointer by 2 rows (2x vertical scaling)
        out += tile_pixel_width * 2;
    }
}

/*
 *  Render and push only dirty tiles to the display
 *  RACE-CONDITION FIX: Uses per-tile render lock and double-buffered DMA.
 *  
 *  This prevents visual glitches (especially around the mouse cursor) caused by
 *  the CPU writing to the framebuffer while we're reading it:
 *  1. Set tile_render_active before snapshot
 *  2. If CPU writes during snapshot, tile is re-marked dirty for next frame
 *  3. Double-buffered output allows DMA overlap with rendering
 *  
 *  @param src_buffer     Mac framebuffer (8-bit indexed)
 *  @param local_palette  Pre-copied palette for thread safety
 */
static void renderAndPushDirtyTiles(uint8 *src_buffer, uint16 *local_palette)
{
    // Double-buffered tile snapshot buffers (40x40 = 1600 bytes each)
    // Static to avoid stack allocation on each call
    // In internal SRAM for fast access during partial updates
    DRAM_ATTR static uint8 tile_snapshot_a[TILE_WIDTH * TILE_HEIGHT];
    DRAM_ATTR static uint8 tile_snapshot_b[TILE_WIDTH * TILE_HEIGHT];
    
    // Double-buffered RGB565 output buffers (80x80 = 12,800 bytes each)
    // In internal SRAM for fast access during partial updates
    DRAM_ATTR static uint16 tile_buffer_a[TILE_WIDTH * PIXEL_SCALE * TILE_HEIGHT * PIXEL_SCALE];
    DRAM_ATTR static uint16 tile_buffer_b[TILE_WIDTH * PIXEL_SCALE * TILE_HEIGHT * PIXEL_SCALE];
    
    // Buffer pointers for double-buffering
    uint8 *current_snapshot = tile_snapshot_a;
    uint8 *next_snapshot = tile_snapshot_b;
    uint16 *current_buffer = tile_buffer_a;
    uint16 *next_buffer = tile_buffer_b;
    
    int tile_pixel_width = TILE_WIDTH * PIXEL_SCALE;
    int tile_pixel_height = TILE_HEIGHT * PIXEL_SCALE;
    int tiles_rendered = 0;
    bool dma_pending = false;
    
    M5.Display.startWrite();
    
    for (int ty = 0; ty < TILES_Y; ty++) {
        for (int tx = 0; tx < TILES_X; tx++) {
            int tile_idx = ty * TILES_X + tx;
            
            // Skip tiles that aren't dirty
            if (!isTileDirty(tile_idx)) {
                continue;
            }
            
            // STEP 1: Mark tile as being rendered (prevents CPU from tearing)
            setTileRenderActive(tile_idx);
            
            // STEP 2: Take a mini-snapshot of just this tile
            // While render_active is set, CPU writes will re-mark tile dirty
            snapshotTile(src_buffer, tx, ty, current_snapshot);
            
            // STEP 3: Clear render lock - snapshot is complete
            // Any CPU writes after this point will be visible in next frame
            clearTileRenderActive(tile_idx);
            
            // Memory barrier to ensure snapshot is complete before rendering
            __sync_synchronize();
            
            // STEP 4: Render from the snapshot (not from the live framebuffer)
            renderTileFromSnapshot(current_snapshot, local_palette, current_buffer);
            
            // STEP 5: Wait for any pending DMA before using its buffer
            if (dma_pending) {
                M5.Display.waitDMA();
                dma_pending = false;
            }
            
            // STEP 6: Push to display using async DMA
            int dst_start_x = tx * tile_pixel_width;
            int dst_start_y = ty * tile_pixel_height;
            
            M5.Display.setAddrWindow(dst_start_x, dst_start_y, tile_pixel_width, tile_pixel_height);
            M5.Display.writePixelsDMA(current_buffer, tile_pixel_width * tile_pixel_height);
            dma_pending = true;
            
            // STEP 7: Swap buffers for next tile
            // This allows rendering next tile while DMA pushes current
            uint8 *tmp_snap = current_snapshot;
            current_snapshot = next_snapshot;
            next_snapshot = tmp_snap;
            
            uint16 *tmp_buf = current_buffer;
            current_buffer = next_buffer;
            next_buffer = tmp_buf;
            
            tiles_rendered++;
            
            // Every 8 tiles, yield to let other tasks run
            // This prevents starvation during full-screen updates
            if ((tiles_rendered & 0x07) == 0) {
                taskYIELD();
            }
        }
    }
    
    // Wait for final DMA to complete before ending write session
    if (dma_pending) {
        M5.Display.waitDMA();
    }
    
    M5.Display.endWrite();
}

/*
 *  Render frame buffer directly to display using streaming (no intermediate PSRAM buffer)
 *  
 *  This optimized version eliminates the 1.8MB dsi_framebuffer by:
 *  1. Processing 2 Mac rows at a time (becomes 4 display rows with 2x scaling)
 *  2. Converting 8-bit indexed to RGB565 into internal SRAM row buffer
 *  3. Immediately pushing to display via M5GFX
 *  
 *  PSRAM traffic: ~230KB read (mac_frame_buffer only)
 *  vs old method: ~230KB read + 1.8MB write + 1.8MB read = ~3.8MB
 *  
 *  Supports all bit depths (1/2/4/8-bit) by decoding packed pixels first.
 */
static void renderFrameStreaming(uint8 *src_buffer, uint16 *local_palette)
{
    if (!src_buffer) return;
    
    // Get current depth and bytes per row (volatile, so copy locally)
    video_depth depth = current_depth;
    uint32 bpr = current_bytes_per_row;
    
    // Row decode buffer for packed pixel modes
    // In internal SRAM for fast access during rendering
    DRAM_ATTR static uint8 decoded_row[MAC_SCREEN_WIDTH];
    
    // Track if we have a pending DMA transfer
    bool dma_pending = false;
    int pending_display_y = 0;
    
    M5.Display.startWrite();
    
    // Process 4 Mac rows at a time (produces 8 display rows with 2x scaling)
    // Double-buffering: render to one buffer while DMA pushes the other
    for (int mac_y = 0; mac_y < MAC_SCREEN_HEIGHT; mac_y += 4) {
        uint16 *out = render_buffer;
        
        // Process 4 Mac rows into render_buffer
        for (int row_offset = 0; row_offset < 4; row_offset++) {
            int y = mac_y + row_offset;
            if (y >= MAC_SCREEN_HEIGHT) break;
            
            // Get source row pointer
            uint8 *src_row = src_buffer + y * bpr;
            
            // Decode the row if needed (converts packed pixels to 8-bit indices)
            uint8 *pixel_row;
            if (depth == VDEPTH_8BIT) {
                // 8-bit mode: direct access, no decoding needed
                pixel_row = src_row;
            } else {
                // Packed mode: decode to 8-bit indices
                decodePackedRow(src_row, decoded_row, MAC_SCREEN_WIDTH, depth);
                pixel_row = decoded_row;
            }
            
            // Output row pointers for the two scaled display rows
            uint16 *dst_row0 = out;
            uint16 *dst_row1 = out + DISPLAY_WIDTH;
            
            // Process 4 decoded pixels at a time for better memory bandwidth
            int x = 0;
            for (; x < MAC_SCREEN_WIDTH - 3; x += 4) {
                // Read 4 decoded pixels at once (32-bit read from 8-bit indices)
                uint32 src4 = *((uint32 *)(pixel_row + x));
                
                // Convert each pixel through palette and write 2x2 scaled
                uint16 c0 = local_palette[src4 & 0xFF];
                uint16 c1 = local_palette[(src4 >> 8) & 0xFF];
                uint16 c2 = local_palette[(src4 >> 16) & 0xFF];
                uint16 c3 = local_palette[(src4 >> 24) & 0xFF];
                
                // Write to row 0 (2 pixels per source pixel)
                dst_row0[0] = c0; dst_row0[1] = c0;
                dst_row0[2] = c1; dst_row0[3] = c1;
                dst_row0[4] = c2; dst_row0[5] = c2;
                dst_row0[6] = c3; dst_row0[7] = c3;
                
                // Write to row 1 (duplicate of row 0)
                dst_row1[0] = c0; dst_row1[1] = c0;
                dst_row1[2] = c1; dst_row1[3] = c1;
                dst_row1[4] = c2; dst_row1[5] = c2;
                dst_row1[6] = c3; dst_row1[7] = c3;
                
                dst_row0 += 8;
                dst_row1 += 8;
            }
            
            // Handle remaining pixels (if width not divisible by 4)
            for (; x < MAC_SCREEN_WIDTH; x++) {
                uint16 c = local_palette[pixel_row[x]];
                dst_row0[0] = c; dst_row0[1] = c;
                dst_row1[0] = c; dst_row1[1] = c;
                dst_row0 += 2;
                dst_row1 += 2;
            }
            
            // Move output pointer by 2 display rows (2x vertical scaling)
            out += DISPLAY_WIDTH * 2;
        }
        
        // Wait for any pending DMA transfer to complete before swapping buffers
        if (dma_pending) {
            M5.Display.waitDMA();
            dma_pending = false;
        }
        
        // Swap buffers - render_buffer becomes push_buffer for DMA
        uint16 *temp = render_buffer;
        render_buffer = push_buffer;
        push_buffer = temp;
        
        // Start async DMA push of the just-rendered buffer (now in push_buffer)
        // 8 display rows * 1280 pixels = 10240 pixels per chunk
        int display_y = mac_y * PIXEL_SCALE;
        M5.Display.setAddrWindow(0, display_y, DISPLAY_WIDTH, STREAMING_ROW_COUNT);
        M5.Display.writePixelsDMA(push_buffer, DISPLAY_WIDTH * STREAMING_ROW_COUNT);
        dma_pending = true;
        pending_display_y = display_y;
        
        // Yield every 32 Mac rows (8 iterations) to let IDLE task run
        // This prevents watchdog timeout during full-frame renders
        if ((mac_y & 0x1F) == 0) {
            taskYIELD();
        }
    }
    
    // Wait for final DMA transfer to complete
    if (dma_pending) {
        M5.Display.waitDMA();
    }
    
    M5.Display.endWrite();
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
}

/*
 *  Report video performance stats periodically
 */
static void reportVideoPerfStats(void)
{
    uint32_t now = millis();
    if (now - perf_last_report_ms >= PERF_REPORT_INTERVAL_MS) {
        perf_last_report_ms = now;
        
        uint32_t total_frames = perf_full_count + perf_partial_count + perf_skip_count;
        if (total_frames > 0) {
            Serial.printf("[VIDEO PERF] frames=%u (full=%u partial=%u skip=%u)\n",
                          total_frames, perf_full_count, perf_partial_count, perf_skip_count);
            Serial.printf("[VIDEO PERF] avg: detect=%uus render=%uus\n",
                          perf_detect_us / (total_frames > 0 ? total_frames : 1),
                          perf_render_us / (total_frames > 0 ? total_frames : 1));
        }
        
        // Reset counters for next interval
        perf_detect_us = 0;
        perf_render_us = 0;
        perf_frame_count = 0;
        perf_partial_count = 0;
        perf_full_count = 0;
        perf_skip_count = 0;
    }
}

/*
 *  Optimized video rendering task - uses WRITE-TIME dirty tracking
 *  
 *  Key optimizations over the old triple-buffer approach:
 *  1. NO frame snapshot copy - we read directly from mac_frame_buffer
 *  2. NO per-frame comparison - dirty tiles are marked at write time by memory.cpp
 *  3. Event-driven with timeout - wakes on notification OR after 67ms max
 *  
 *  This eliminates ~230KB memcpy per frame and expensive tile comparisons.
 *  Dirty tracking overhead is spread across actual CPU writes instead of
 *  being a bulk operation every frame.
 */
static void videoRenderTaskOptimized(void *param)
{
    UNUSED(param);
    Serial.println("[VIDEO] Video render task started on Core 0 (write-time dirty tracking)");
    
    // Reconfigure watchdog to be more lenient for video rendering
    // Video frames can take 50-100ms, so we need a longer timeout
    // Also disable panic so it just logs a warning instead of rebooting
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 10000,      // 10 second timeout (very generous)
        .idle_core_mask = 0,      // Don't monitor IDLE tasks (they get starved by video)
        .trigger_panic = false    // Don't reboot on timeout, just warn
    };
    esp_task_wdt_reconfigure(&wdt_config);
    Serial.println("[VIDEO] Watchdog reconfigured: 10s timeout, no panic, IDLE not monitored");
    
    // Wait a moment for everything to initialize
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Local palette copy for thread safety
    uint16 local_palette[256];
    
    // Initialize perf reporting timer
    perf_last_report_ms = millis();
    
    // Minimum frame interval (42ms = ~24 FPS)
    // 24fps is cinema standard and perceptually smooth
    const TickType_t min_frame_ticks = pdMS_TO_TICKS(42);
    TickType_t last_frame_ticks = xTaskGetTickCount();
    
    while (video_task_running) {
        // Note: Watchdog is configured with 10s timeout and no panic,
        // so we don't need to reset it frequently
        
        // Event-driven: wait for frame signal with timeout
        // This replaces the old polling loop - task sleeps until signaled
        // Max wait time ensures we still render periodically even if no signal
        uint32_t notification = ulTaskNotifyTake(pdTRUE, min_frame_ticks);
        
        // Also check legacy frame_ready flag for compatibility
        bool should_render = (notification > 0) || frame_ready;
        frame_ready = false;
        
        // Rate limit: ensure minimum time between frames
        TickType_t now = xTaskGetTickCount();
        TickType_t elapsed = now - last_frame_ticks;
        if (should_render && elapsed < min_frame_ticks) {
            // Too soon - skip this frame signal, we'll render on next timeout
            continue;
        }
        
        // Only render if we have something to render
        if (!should_render && !force_full_update) {
            // Timeout with nothing to do - check for write-dirty tiles anyway
            // This handles cases where writes happened but no explicit signal
        }
        
        uint32_t t0, t1;
        
        // Take a snapshot of the palette only if it changed (thread-safe)
        // This avoids 512-byte memcpy and spinlock contention on every frame
        if (palette_changed) {
            portENTER_CRITICAL(&frame_spinlock);
            memcpy(local_palette, palette_rgb565, 256 * sizeof(uint16));
            palette_changed = false;
            portEXIT_CRITICAL(&frame_spinlock);
        }
        
        // Collect dirty tiles from write-time tracking
        t0 = micros();
        dirty_tile_count = collectWriteDirtyTiles();
        t1 = micros();
        perf_detect_us += (t1 - t0);
        
        // If force_full_update is set (palette change, first frame), mark ALL tiles dirty
        // This ensures we always use tile mode (faster than streaming mode)
        if (force_full_update) {
            // Mark all tiles as dirty
            for (int i = 0; i < (TOTAL_TILES + 31) / 32; i++) {
                dirty_tiles[i] = 0xFFFFFFFF;
            }
            dirty_tile_count = TOTAL_TILES;
            force_full_update = false;
            perf_full_count++;
        }
        
        // RENDER - always use tile mode (faster than streaming even for full screen)
        if (dirty_tile_count > 0) {
            // Render and push only dirty tiles
            t0 = micros();
            renderAndPushDirtyTiles(mac_frame_buffer, local_palette);
            t1 = micros();
            perf_render_us += (t1 - t0);
            
            perf_partial_count++;
        } else {
            // No tiles dirty, nothing to do!
            perf_skip_count++;
        }
        
        perf_frame_count++;
        last_frame_ticks = now;
        
        // Report performance stats periodically
        reportVideoPerfStats();
    }
    
    Serial.println("[VIDEO] Video render task exiting");
    vTaskDelete(NULL);
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
    
    // Verify display size matches our expectations
    if (display_width != DISPLAY_WIDTH || display_height != DISPLAY_HEIGHT) {
        Serial.printf("[VIDEO] WARNING: Expected %dx%d display, got %dx%d\n", 
                      DISPLAY_WIDTH, DISPLAY_HEIGHT, display_width, display_height);
    }
    
    // Allocate Mac frame buffer in PSRAM
    // For 640x360 @ 8-bit = 230,400 bytes
    frame_buffer_size = MAC_SCREEN_WIDTH * MAC_SCREEN_HEIGHT;
    
    mac_frame_buffer = (uint8 *)ps_malloc(frame_buffer_size);
    if (!mac_frame_buffer) {
        Serial.println("[VIDEO] ERROR: Failed to allocate Mac frame buffer in PSRAM!");
        return false;
    }
    
    Serial.printf("[VIDEO] Mac frame buffer allocated: %p (%d bytes)\n", mac_frame_buffer, frame_buffer_size);
    
    // Clear frame buffer to gray
    memset(mac_frame_buffer, 0x80, frame_buffer_size);
    
    // Initialize dirty tracking and render lock
    memset(dirty_tiles, 0, sizeof(dirty_tiles));
    memset(write_dirty_tiles, 0, sizeof(write_dirty_tiles));
    memset(tile_render_active, 0, sizeof(tile_render_active));
    force_full_update = true;  // Force full update on first frame
    
    // Clear display to dark gray using streaming row buffer
    uint16 gray565 = rgb888_to_rgb565(64, 64, 64);
    for (int i = 0; i < DISPLAY_WIDTH * STREAMING_ROW_COUNT; i++) {
        streaming_row_buffer_a[i] = gray565;
    }
    M5.Display.startWrite();
    for (int y = 0; y < DISPLAY_HEIGHT; y += STREAMING_ROW_COUNT) {
        M5.Display.setAddrWindow(0, y, DISPLAY_WIDTH, STREAMING_ROW_COUNT);
        M5.Display.writePixels(streaming_row_buffer_a, DISPLAY_WIDTH * STREAMING_ROW_COUNT);
    }
    M5.Display.endWrite();
    Serial.println("[VIDEO] Initial screen cleared");
    
    // Set up Mac frame buffer pointers
    MacFrameBaseHost = mac_frame_buffer;
    MacFrameSize = frame_buffer_size;
    MacFrameLayout = FLAYOUT_DIRECT;
    
    // Initialize default palette for 8-bit mode (256 colors)
    // This sets up a proper color palette instead of grayscale,
    // so MacOS will default to "256 colors" instead of "256 grays"
    initDefaultPalette(VDEPTH_8BIT);
    
    // Create video mode vector with all supported depths
    // Per Basilisk II rules: lowest depth must be available in all resolutions,
    // and if a resolution has a depth, it must have all lower depths too.
    // We support 1/2/4/8 bit depths at 640x360.
    vector<video_mode> modes;
    video_mode mode;
    mode.x = MAC_SCREEN_WIDTH;
    mode.y = MAC_SCREEN_HEIGHT;
    mode.resolution_id = 0x80;
    mode.user_data = 0;
    
    // Add 1-bit mode (black and white)
    mode.depth = VDEPTH_1BIT;
    mode.bytes_per_row = TrivialBytesPerRow(MAC_SCREEN_WIDTH, VDEPTH_1BIT);  // 80 bytes
    modes.push_back(mode);
    Serial.printf("[VIDEO] Added mode: 1-bit, %d bytes/row\n", mode.bytes_per_row);
    
    // Add 2-bit mode (4 colors)
    mode.depth = VDEPTH_2BIT;
    mode.bytes_per_row = TrivialBytesPerRow(MAC_SCREEN_WIDTH, VDEPTH_2BIT);  // 160 bytes
    modes.push_back(mode);
    Serial.printf("[VIDEO] Added mode: 2-bit, %d bytes/row\n", mode.bytes_per_row);
    
    // Add 4-bit mode (16 colors)
    mode.depth = VDEPTH_4BIT;
    mode.bytes_per_row = TrivialBytesPerRow(MAC_SCREEN_WIDTH, VDEPTH_4BIT);  // 320 bytes
    modes.push_back(mode);
    Serial.printf("[VIDEO] Added mode: 4-bit, %d bytes/row\n", mode.bytes_per_row);
    
    // Add 8-bit mode (256 colors) - this is our default
    mode.depth = VDEPTH_8BIT;
    mode.bytes_per_row = TrivialBytesPerRow(MAC_SCREEN_WIDTH, VDEPTH_8BIT);  // 640 bytes
    modes.push_back(mode);
    Serial.printf("[VIDEO] Added mode: 8-bit, %d bytes/row\n", mode.bytes_per_row);
    
    // Store current mode info (8-bit default)
    current_mode = mode;
    
    // Initialize the video state cache for 8-bit mode
    updateVideoStateCache(VDEPTH_8BIT, mode.bytes_per_row);
    
    // Create monitor descriptor with 8-bit as default depth
    the_monitor = new ESP32_monitor_desc(modes, VDEPTH_8BIT, 0x80);
    VideoMonitors.push_back(the_monitor);
    
    // Set Mac frame buffer base address
    the_monitor->set_mac_frame_base(MacFrameBaseMac);
    
    // Start video rendering task on Core 0
    // Use the optimized version that does render + push
    video_task_running = true;
    BaseType_t result = xTaskCreatePinnedToCore(
        videoRenderTaskOptimized,
        "VideoTask",
        VIDEO_TASK_STACK_SIZE,
        NULL,
        VIDEO_TASK_PRIORITY,
        &video_task_handle,
        VIDEO_TASK_CORE
    );
    
    if (result != pdPASS) {
        Serial.println("[VIDEO] ERROR: Failed to start video task!");
        // Continue anyway - will fall back to synchronous refresh
    } else {
        Serial.printf("[VIDEO] Video task created on Core %d\n", VIDEO_TASK_CORE);
    }
    
    Serial.printf("[VIDEO] Mac frame base: 0x%08X\n", MacFrameBaseMac);
    Serial.printf("[VIDEO] Dirty tracking: %dx%d tiles (%d total), threshold %d%%\n", 
                  TILES_X, TILES_Y, TOTAL_TILES, DIRTY_THRESHOLD_PERCENT);
    Serial.println("[VIDEO] VideoInit complete (with dirty tile tracking)");
    
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
    
    // Clear dirty tracking and render lock (safety for potential re-init)
    memset(dirty_tiles, 0, sizeof(dirty_tiles));
    memset(write_dirty_tiles, 0, sizeof(write_dirty_tiles));
    memset(tile_render_active, 0, sizeof(tile_render_active));
    
    if (mac_frame_buffer) {
        free(mac_frame_buffer);
        mac_frame_buffer = NULL;
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
 *  
 *  Uses FreeRTOS task notification for event-driven wake-up.
 *  The video task sleeps until notified, saving CPU cycles.
 */
void VideoSignalFrameReady(void)
{
    // Set legacy flag for compatibility
    frame_ready = true;
    
    // Send task notification to wake up video task immediately
    // This is more efficient than polling - video task sleeps until notified
    if (video_task_handle != NULL) {
        xTaskNotifyGive(video_task_handle);
    }
}

/*
 *  Video refresh - legacy synchronous function
 *  Now just signals the video task instead of doing the work directly
 *  This allows CPU emulation to continue while video task handles rendering
 */
void VideoRefresh(void)
{
    if (!mac_frame_buffer || !video_task_running) {
        // Fallback: if video task not running, do nothing
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
 *  Get pointer to frame buffer (the buffer that CPU uses)
 */
uint8 *VideoGetFrameBuffer(void)
{
    return mac_frame_buffer;
}

/*
 *  Get frame buffer size
 */
uint32 VideoGetFrameBufferSize(void)
{
    return frame_buffer_size;
}
