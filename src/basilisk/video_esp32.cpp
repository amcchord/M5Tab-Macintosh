/*
 *  video_esp32.cpp - Video/graphics emulation for ESP32-P4
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
#include "automation.h"

#include "board_config.h"
#include "board_display.h"
#include "touch_overlay.h"

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

#ifndef VIDEO_DIRTY_MARK_NOOP
#define VIDEO_DIRTY_MARK_NOOP 0
#endif

#ifndef VIDEO_MAX_DEPTH_BITS
#define VIDEO_MAX_DEPTH_BITS 8
#endif

#if VIDEO_MAX_DEPTH_BITS == 1
#define MAC_SCREEN_DEPTH VDEPTH_1BIT
#elif VIDEO_MAX_DEPTH_BITS == 2
#define MAC_SCREEN_DEPTH VDEPTH_2BIT
#elif VIDEO_MAX_DEPTH_BITS == 4
#define MAC_SCREEN_DEPTH VDEPTH_4BIT
#elif VIDEO_MAX_DEPTH_BITS == 8
#define MAC_SCREEN_DEPTH VDEPTH_8BIT
#else
#error "VIDEO_MAX_DEPTH_BITS must be 1, 2, 4, or 8"
#endif

// Display configuration is board-specific (see src/board/board_config.h).
// Tab5: 640x360 Mac @ 2x = 1280x720. Waveshare: 640x400 Mac @ 2x = 1280x800.
#define MAC_SCREEN_WIDTH  BOARD_MAC_SCREEN_WIDTH
#define MAC_SCREEN_HEIGHT BOARD_MAC_SCREEN_HEIGHT
#define PIXEL_SCALE       BOARD_PIXEL_SCALE

// Physical display dimensions (post pixel-doubling)
#define DISPLAY_WIDTH     BOARD_DISPLAY_WIDTH
#define DISPLAY_HEIGHT    BOARD_DISPLAY_HEIGHT

// Tile-based dirty tracking. Grid dims come from board_config.h so each
// board gets a 16x(N) grid that tiles its Mac framebuffer exactly.
#define TILE_WIDTH        BOARD_TILE_WIDTH
#define TILE_HEIGHT       BOARD_TILE_HEIGHT
#define TILES_X           BOARD_TILES_X
#define TILES_Y           BOARD_TILES_Y
#define TOTAL_TILES       (TILES_X * TILES_Y)

// Dirty tile threshold - if more than this percentage of tiles are dirty,
// do a full update instead of partial
// NOTE: Set to 101 to ALWAYS use tile mode - tile updates are actually faster
// than full streaming even when all tiles are dirty, because tile mode uses
// double-buffered DMA while streaming mode processes rows sequentially
#define DIRTY_THRESHOLD_PERCENT  101

// Video task configuration
#define VIDEO_TASK_STACK_SIZE  8192
#define VIDEO_TASK_PRIORITY    2
#define VIDEO_TASK_CORE        0  // Run on Core 0, leaving Core 1 for CPU emulation
// Keep cadence aligned with main-thread frame signaling for responsive video.
#if VIDEO_DIRTY_MARK_NOOP
// Full-frame refreshes move far more pixels than sparse tile updates. Keep the
// diagnostic cadence configurable so benchmarks can establish the upper bound
// obtained by removing all guest-write bookkeeping without overwhelming DSI.
#ifndef VIDEO_NOOP_REFRESH_MS
#define VIDEO_NOOP_REFRESH_MS 100
#endif
#define VIDEO_MIN_FRAME_INTERVAL_MS VIDEO_NOOP_REFRESH_MS
#else
#define VIDEO_MIN_FRAME_INTERVAL_MS 45
#endif

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
// Advanced once per collection. Core 1 uses this to collapse repeated writes
// to the same tile into one atomic bitmap update per display epoch.
DRAM_ATTR static volatile uint32 dirty_collection_epoch = 1;

// Per-tile render lock bitmap - set while video task is snapshotting a tile
// If CPU tries to write while this is set, the tile is re-marked dirty for next frame
// This prevents torn data from race conditions during snapshot
DRAM_ATTR static uint32 tile_render_active[(TOTAL_TILES + 31) / 32];   // Tiles currently being rendered

// Lookup tables for fast 8-bit dirty-tile mapping.
// Avoids repeated /40 and /640 math on the framebuffer write hot path.
DRAM_ATTR static uint8 tile_col_lut[MAC_SCREEN_WIDTH];
DRAM_ATTR static uint8 tile_row_base_lut[MAC_SCREEN_HEIGHT];
static bool tile_lut_initialized = false;

// The old "streaming full-frame render" path with its 2x 20 KB DRAM_ATTR
// row buffers has been removed. DIRTY_THRESHOLD_PERCENT=101 means we are
// always in tile-render mode, so those 40 KB of BSS were dead at runtime
// and just prevented cpufunctbl (256 KB) from landing in internal SRAM.
// The one-shot VideoInit clear now goes through BoardDisplay_ClearScreen.

static volatile bool force_full_update = true;               // Force full update on first frame or palette change
static int dirty_tile_count = 0;                             // Count of dirty tiles for threshold check

// When true, the video task suppresses its own frame pushes (even a
// force_full_update one) until Mac OS writes something real into the Mac
// frame buffer. That keeps the classic-Mac pre-boot splash (the tiled
// checkerboard + Happy Mac painted by MacSplash) on screen right up to
// the moment the emulator actually starts drawing, instead of flashing
// solid gray in between. Cleared by the task itself on first real write.
static volatile bool preserve_splash_until_first_write = false;

// Timestamp (millis()) when the preserve-splash gate was armed. Used as a
// safety ceiling so a pathological ROM that keeps writing uniform frames
// can't pin the splash forever - after this window elapses we hand off
// unconditionally. In practice Mac OS paints the 50% gray desktop stipple
// within a few hundred ms, so 5 s is plenty of headroom.
static volatile uint32 preserve_splash_armed_ms = 0;
#define PRESERVE_SPLASH_MAX_MS 5000

// PSRAM strip buffer for row-span merging optimization.
// Holds one full-width row: 1280 pixels × 80 display-rows × 2 bytes = 204,800 bytes.
// Adjacent dirty tiles in the same row are composed here and pushed with a single
// setAddrWindow + DMA call, reducing DSI command overhead proportionally.
#define STRIP_BUF_PIXELS (DISPLAY_WIDTH * TILE_HEIGHT * PIXEL_SCALE)
static uint16 *strip_buffer = nullptr;

// Display dimensions (from BoardDisplay HAL)
static int display_width = 0;
static int display_height = 0;

// Video mode info
static video_mode current_mode;

// Current video state cache - updated on mode switch for fast access during rendering
// These are used by the render loops and dirty tracking to handle different bit depths
static volatile video_depth current_depth = MAC_SCREEN_DEPTH;
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
#define PERF_REPORT_INTERVAL_MS 30000               // Report every 30 seconds

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
 *  Convert RGB888 to the native little-endian RGB565 layout that
 *  esp_lcd_panel_draw_bitmap() expects (R:5 G:6 B:5 packing). Both
 *  boards now drive their MIPI-DSI panels through the same IDF API,
 *  so a single conversion works everywhere.
 */
static inline uint16 rgb888_to_rgb565(uint8 r, uint8 g, uint8 b)
{
    return (uint16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
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

static void initTileLuts(void)
{
    if (tile_lut_initialized) {
        return;
    }
    for (int x = 0; x < MAC_SCREEN_WIDTH; x++) {
        tile_col_lut[x] = (uint8)(x / TILE_WIDTH);
    }
    for (int y = 0; y < MAC_SCREEN_HEIGHT; y++) {
        tile_row_base_lut[y] = (uint8)((y / TILE_HEIGHT) * TILES_X);
    }
    tile_lut_initialized = true;
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
    /* Keep Serial.println OUT of the critical section. The spinlock is
     * taken on the calling core AND blocks the opposite core when it
     * attempts to read the palette in the video render path. If we
     * printed from inside the critical section, a UART TX stall would
     * starve the other core's interrupt watchdog and panic CPU0.
     * We only log which path we took after the critical section exits. */
    const char *log_msg = NULL;

    portENTER_CRITICAL(&frame_spinlock);

    switch (depth) {
        case VDEPTH_1BIT:
            // 1-bit: Black and white
            // Index 0 = white, Index 1 = black
            palette_rgb565[0] = rgb888_to_rgb565(255, 255, 255);  // White
            palette_rgb565[1] = rgb888_to_rgb565(0, 0, 0);        // Black
            log_msg = "[VIDEO] Initialized 1-bit B&W palette";
            break;

        case VDEPTH_2BIT:
            // 2-bit: 4 levels of gray
            // Index 0 = white, Index 3 = black
            palette_rgb565[0] = rgb888_to_rgb565(255, 255, 255);  // White
            palette_rgb565[1] = rgb888_to_rgb565(170, 170, 170);  // Light gray
            palette_rgb565[2] = rgb888_to_rgb565(85, 85, 85);     // Dark gray
            palette_rgb565[3] = rgb888_to_rgb565(0, 0, 0);        // Black
            log_msg = "[VIDEO] Initialized 2-bit grayscale palette";
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
            log_msg = "[VIDEO] Initialized 4-bit 16-color palette";
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
            log_msg = "[VIDEO] Initialized 8-bit 256-color palette";
            break;
    }

    portEXIT_CRITICAL(&frame_spinlock);

    if (log_msg) {
        Serial.println(log_msg);
    }

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

static inline void markTileDirtyBit(int tile_idx)
{
    static uint32 cached_epoch = 0;
    static int cached_tile = -1;
    const uint32 epoch = dirty_collection_epoch;
    if (likely(cached_epoch == epoch && cached_tile == tile_idx)) {
        return;
    }
    __atomic_or_fetch(&write_dirty_tiles[tile_idx / 32], (1u << (tile_idx % 32)), __ATOMIC_RELAXED);
    cached_epoch = epoch;
    cached_tile = tile_idx;
}

// Fast 8-bit path helper: convert framebuffer byte offset to tile index.
static inline int fastTileIndex8Bit(uint32 offset)
{
    if (offset >= (uint32)(MAC_SCREEN_WIDTH * MAC_SCREEN_HEIGHT)) return -1;

    // Writes are typically clustered on nearby rows; cache last resolved row.
    static uint32 cached_row_base = 0xFFFFFFFFu;
    static uint16 cached_row = 0;

    uint32 row_base = cached_row_base;
    uint32 y;
    if (likely(row_base != 0xFFFFFFFFu && offset >= row_base && offset < (row_base + MAC_SCREEN_WIDTH))) {
        y = cached_row;
    } else {
        y = offset / MAC_SCREEN_WIDTH;
        row_base = y * MAC_SCREEN_WIDTH;
        cached_row_base = row_base;
        cached_row = (uint16)y;
    }

    uint32 x = offset - row_base;
    return (int)(tile_row_base_lut[y] + tile_col_lut[x]);
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
#if VIDEO_DIRTY_MARK_NOOP
    UNUSED(offset);
    return;
#else
    if (offset >= frame_buffer_size) return;

    // Hot path: 8-bit mode (default mode for this port).
    if (likely(current_depth == VDEPTH_8BIT && current_bytes_per_row == MAC_SCREEN_WIDTH)) {
        int tile_idx = fastTileIndex8Bit(offset);
        if (tile_idx >= 0) {
            markTileDirtyBit(tile_idx);
        }
        return;
    }
    
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
            markTileDirtyBit(tile_idx);
        }
    }
#endif
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
#if VIDEO_DIRTY_MARK_NOOP
    UNUSED(offset);
    UNUSED(size);
    return;
#else
    if (size == 0 || offset >= frame_buffer_size) return;

    // Clamp size to framebuffer bounds
    if (offset + size > frame_buffer_size) {
        size = frame_buffer_size - offset;
    }

    // Hot path: 8-bit mode with 2/4-byte writes.
    if (size <= 4 && likely(current_depth == VDEPTH_8BIT && current_bytes_per_row == MAC_SCREEN_WIDTH)) {
        int first_tile = fastTileIndex8Bit(offset);
        if (first_tile >= 0) {
            markTileDirtyBit(first_tile);
        }

        if (size > 1) {
            uint32 end_offset = offset + size - 1;
            int last_tile = fastTileIndex8Bit(end_offset);
            if (last_tile >= 0 && last_tile != first_tile) {
                markTileDirtyBit(last_tile);
            }
        }
        return;
    }

    // Hot path: emulator memory writes are 2 or 4 bytes.
    // Marking first and last touched bytes is sufficient here and avoids
    // the expensive multi-row/full-row fallback path.
    if (size <= 4) {
        VideoMarkDirtyOffset(offset);
        if (size > 1) {
            VideoMarkDirtyOffset(offset + size - 1);
        }
        return;
    }

    // Generic fallback for larger ranges (currently unused in this port).
    VideoMarkDirtyOffset(offset);
    VideoMarkDirtyOffset(offset + size - 1);
#endif
}

/*
 *  Mark every tile that intersects a physical-display rectangle dirty.
 *  Used by external code (touch overlay) to force the video task to
 *  redraw a region even when the Mac framebuffer hasn't changed. Takes
 *  physical-display-pixel coordinates and converts them to Mac-tile
 *  indices via the known PIXEL_SCALE.
 */
extern "C" void VideoMarkTilesDirtyRect(int px, int py, int pw, int ph)
{
    if (pw <= 0 || ph <= 0) return;

    /* Physical -> Mac coords (PIXEL_SCALE = 2 on both supported boards). */
    int mx0 = px / PIXEL_SCALE;
    int my0 = py / PIXEL_SCALE;
    int mx1 = (px + pw + PIXEL_SCALE - 1) / PIXEL_SCALE;
    int my1 = (py + ph + PIXEL_SCALE - 1) / PIXEL_SCALE;
    if (mx0 < 0) mx0 = 0;
    if (my0 < 0) my0 = 0;
    if (mx1 > MAC_SCREEN_WIDTH)  mx1 = MAC_SCREEN_WIDTH;
    if (my1 > MAC_SCREEN_HEIGHT) my1 = MAC_SCREEN_HEIGHT;
    if (mx0 >= mx1 || my0 >= my1) return;

    int tx0 = mx0 / TILE_WIDTH;
    int ty0 = my0 / TILE_HEIGHT;
    int tx1 = (mx1 + TILE_WIDTH  - 1) / TILE_WIDTH;
    int ty1 = (my1 + TILE_HEIGHT - 1) / TILE_HEIGHT;
    if (tx1 > TILES_X) tx1 = TILES_X;
    if (ty1 > TILES_Y) ty1 = TILES_Y;

    for (int ty = ty0; ty < ty1; ++ty) {
        for (int tx = tx0; tx < tx1; ++tx) {
            markTileDirtyBit(ty * TILES_X + tx);
        }
    }

    /* Wake the video task so the redraw happens promptly. */
    if (video_task_handle != NULL) {
        xTaskNotifyGive(video_task_handle);
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
        count += __builtin_popcount(bits);
    }
    __atomic_add_fetch(&dirty_collection_epoch, 1u, __ATOMIC_RELAXED);
    
    return count;
}

/*
 *  Cheap "is the Mac framebuffer all one value?" check used by the splash
 *  handoff gate. Mac OS's first few framebuffer writes are typically a
 *  uniform clear (all 0x00 / 0xFF / similar); blitting those over our
 *  pre-boot checkerboard looks like a black flash, so we keep the splash
 *  up until the frame actually has content variation.
 *
 *  We sample the corners + center of every currently-dirty tile (7 bytes
 *  per tile). If all samples match across every dirty tile, the frame is
 *  considered uniform. Short-circuits as soon as a mismatch is found.
 */
static bool frame_is_uniform(uint8 *src_buffer)
{
    if (!src_buffer) return true;

    video_depth depth = current_depth;
    uint32 bpr = current_bytes_per_row;

    // Pick the reference byte from the first dirty tile we find.
    bool have_ref = false;
    uint8 ref = 0;

    for (int ty = 0; ty < TILES_Y; ty++) {
        for (int tx = 0; tx < TILES_X; tx++) {
            int tile_idx = ty * TILES_X + tx;
            int word = tile_idx >> 5;
            int bit  = tile_idx & 31;
            if ((dirty_tiles[word] & (1u << bit)) == 0) continue;

            int tile_x_px = tx * TILE_WIDTH;
            int tile_y_px = ty * TILE_HEIGHT;

            // Convert pixel x into a byte offset within the row. This is
            // approximate for packed modes (we only use it for sampling),
            // and it's correct for the default 8-bit path Mac OS boots in.
            int byte_x_left;
            int byte_x_right;
            if (depth == VDEPTH_8BIT) {
                byte_x_left  = tile_x_px;
                byte_x_right = tile_x_px + TILE_WIDTH - 1;
            } else {
                int ppb = current_pixels_per_byte;
                if (ppb <= 0) ppb = 1;
                byte_x_left  = tile_x_px / ppb;
                byte_x_right = (tile_x_px + TILE_WIDTH - 1) / ppb;
            }

            int y_top    = tile_y_px;
            int y_bot    = tile_y_px + TILE_HEIGHT - 1;
            int y_mid    = tile_y_px + TILE_HEIGHT / 2;
            int byte_x_mid = (byte_x_left + byte_x_right) / 2;

            const int sample_count = 7;
            const struct { int x; int y; } samples[sample_count] = {
                { byte_x_left,  y_top },
                { byte_x_right, y_top },
                { byte_x_left,  y_bot },
                { byte_x_right, y_bot },
                { byte_x_mid,   y_mid },
                { byte_x_left,  y_mid },
                { byte_x_right, y_mid },
            };

            for (int s = 0; s < sample_count; s++) {
                int sx = samples[s].x;
                int sy = samples[s].y;
                if (sy < 0 || sy >= MAC_SCREEN_HEIGHT) continue;
                if (sx < 0 || sx >= (int)bpr) continue;
                uint8 v = src_buffer[(uint32)sy * bpr + (uint32)sx];
                if (!have_ref) {
                    ref = v;
                    have_ref = true;
                } else if (v != ref) {
                    return false;
                }
            }
        }
    }

    // If we didn't sample anything (no dirty tiles), treat as uniform so the
    // caller keeps the splash up; the main gate already handles the
    // dirty_tile_count==0 case before us.
    return true;
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
    
    BoardDisplay_BeginTiles();

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

            // STEP 4.5: Composite any active touch overlay (on-screen
            // keyboard / gaming pad) onto this tile before pushing.
            // Cheap no-op when the overlay is hidden.
            int dst_start_x = tx * tile_pixel_width;
            int dst_start_y = ty * tile_pixel_height;
            TouchOverlay_CompositeTile(dst_start_x, dst_start_y,
                                       tile_pixel_width, tile_pixel_height,
                                       current_buffer);

            // STEP 5: Wait for any pending DMA before using its buffer
            if (dma_pending) {
                BoardDisplay_WaitPush();
                dma_pending = false;
            }

            // STEP 6: Push to display using async DMA
            BoardDisplay_PushTile(dst_start_x, dst_start_y,
                                  tile_pixel_width, tile_pixel_height,
                                  current_buffer);
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
        BoardDisplay_WaitPush();
    }

    BoardDisplay_EndTiles();
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
        if (total_frames > 0 && !AutomationSerialCaptureActive()) {
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
    
    // Minimum frame interval follows CPU-side signal cadence (~10 FPS by default).
    // This keeps Core 0 and PSRAM bandwidth available for CPU emulation.
    const TickType_t min_frame_ticks = pdMS_TO_TICKS(VIDEO_MIN_FRAME_INTERVAL_MS);
    TickType_t last_frame_ticks = xTaskGetTickCount();
    
    while (video_task_running) {
        // Note: Watchdog is configured with 10s timeout and no panic,
        // so we don't need to reset it frequently
        
        // Event-driven: wait for frame signal with timeout.
        // Timeout only exists as a safety net; normal rendering is signal-driven.
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
        
        // Skip unsignaled wakeups unless we need to force a redraw.
        if (!should_render && !force_full_update) {
            continue;
        }

        // The logical framebuffer snapshot is the source of truth for host
        // automation. Avoid competing with its USB compression/transfer task
        // for Core 0 and PSRAM bandwidth; the panel catches up immediately
        // after the short capture lease closes.
        if (AutomationSerialCaptureActive()) {
            continue;
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
        
        // Collect dirty tiles from write-time tracking. In full-frame mode,
        // guest writes carry zero tracking overhead; the existing periodic
        // signal makes every tile visible on the next asynchronous refresh.
        t0 = micros();
#if VIDEO_DIRTY_MARK_NOOP
        for (int i = 0; i < (TOTAL_TILES + 31) / 32; i++) {
            dirty_tiles[i] = 0xFFFFFFFFu;
        }
        dirty_tile_count = should_render ? TOTAL_TILES : 0;
#else
        dirty_tile_count = collectWriteDirtyTiles();
#endif
        t1 = micros();
        perf_detect_us += (t1 - t0);

        // Keep the pre-boot splash on screen until Mac OS actually writes
        // something with real content to the frame buffer. If we're still
        // in that window and nothing has been dirtied yet, skip the push
        // entirely.
        //
        // Mac OS's first few framebuffer writes are typically a uniform
        // clear (all 0x00 or 0xFF) - pushing those looks like a black
        // flash between our checkerboard splash and the Mac boot desktop.
        // We keep the splash up until the frame has content variation
        // (desktop stipple / menu bar pixels). A 5-second safety timeout
        // prevents pinning the splash if something weird happens.
        if (preserve_splash_until_first_write) {
            if (dirty_tile_count == 0) {
                perf_skip_count++;
                continue;
            }

            uint32 splash_elapsed = millis() - preserve_splash_armed_ms;
            bool   timed_out      = splash_elapsed >= PRESERVE_SPLASH_MAX_MS;

            if (!timed_out && frame_is_uniform(mac_frame_buffer)) {
                // Still in clear-screen phase: throw the dirty bits away
                // so we don't accumulate, and wait for a richer frame.
                for (int i = 0; i < (TOTAL_TILES + 31) / 32; i++) {
                    dirty_tiles[i] = 0;
                }
                dirty_tile_count = 0;
                perf_skip_count++;
                continue;
            }

            preserve_splash_until_first_write = false;
            Serial.printf("[VIDEO] First Mac OS content detected (%s), handing off from splash\n",
                          timed_out ? "safety-timeout" : "non-uniform");
        }

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
    
    // Get display dimensions (post-HAL rotation if any)
    display_width  = BoardDisplay_Width();
    display_height = BoardDisplay_Height();
    Serial.printf("[VIDEO] Display size: %dx%d\n", display_width, display_height);
    
    // Verify display size matches our expectations
    if (display_width != DISPLAY_WIDTH || display_height != DISPLAY_HEIGHT) {
        Serial.printf("[VIDEO] WARNING: Expected %dx%d display, got %dx%d\n", 
                      DISPLAY_WIDTH, DISPLAY_HEIGHT, display_width, display_height);
    }
    
    // Allocate Mac frame buffer in PSRAM
    // For 640x360 @ 8-bit = 230,400 bytes
    frame_buffer_size = MAC_SCREEN_WIDTH * MAC_SCREEN_HEIGHT;
    initTileLuts();
    
    mac_frame_buffer = (uint8 *)ps_malloc(frame_buffer_size);
    if (!mac_frame_buffer) {
        Serial.println("[VIDEO] ERROR: Failed to allocate Mac frame buffer in PSRAM!");
        return false;
    }
    
    Serial.printf("[VIDEO] Mac frame buffer allocated: %p (%d bytes)\n", mac_frame_buffer, frame_buffer_size);

    // Allocate the PSRAM strip buffer used by the row-span merging optimization
    strip_buffer = (uint16 *)ps_malloc(STRIP_BUF_PIXELS * sizeof(uint16));
    if (!strip_buffer) {
        Serial.println("[VIDEO] WARNING: Failed to allocate strip buffer in PSRAM - span merging disabled");
    } else {
        Serial.printf("[VIDEO] Strip buffer allocated: %p (%d bytes)\n", strip_buffer, (int)(STRIP_BUF_PIXELS * sizeof(uint16)));
    }
    
    // Clear frame buffer to gray
    memset(mac_frame_buffer, 0x80, frame_buffer_size);
    
    // Initialize dirty tracking and render lock
    memset(dirty_tiles, 0, sizeof(dirty_tiles));
    memset(write_dirty_tiles, 0, sizeof(write_dirty_tiles));
    memset(tile_render_active, 0, sizeof(tile_render_active));
    force_full_update = true;  // Force full update on first real Mac OS write

    // Don't clear the panel here. The MacSplash checkerboard painted at
    // pre-boot stays visible until the 68k actually starts drawing, at
    // which point the video task (below) will push real tiles over it.
    // preserve_splash_until_first_write gates that handoff.
    preserve_splash_until_first_write = true;
    preserve_splash_armed_ms = millis();
    Serial.println("[VIDEO] Splash preserved until first Mac OS write");
    
    // Set up Mac frame buffer pointers
    MacFrameBaseHost = mac_frame_buffer;
    MacFrameSize = frame_buffer_size;
    MacFrameLayout = FLAYOUT_DIRECT;
    
    // Initialize the palette for the selected maximum/default depth.
    initDefaultPalette(MAC_SCREEN_DEPTH);
    
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
    
    // Add packed modes up to the configured maximum. Restricting the mode list
    // is intentional: Mac OS otherwise restores a deeper saved mode at boot.
#if VIDEO_MAX_DEPTH_BITS >= 2
    // Add 2-bit mode (4 colors)
    mode.depth = VDEPTH_2BIT;
    mode.bytes_per_row = TrivialBytesPerRow(MAC_SCREEN_WIDTH, VDEPTH_2BIT);  // 160 bytes
    modes.push_back(mode);
    Serial.printf("[VIDEO] Added mode: 2-bit, %d bytes/row\n", mode.bytes_per_row);
#endif

#if VIDEO_MAX_DEPTH_BITS >= 4
    // Add 4-bit mode (16 colors)
    mode.depth = VDEPTH_4BIT;
    mode.bytes_per_row = TrivialBytesPerRow(MAC_SCREEN_WIDTH, VDEPTH_4BIT);  // 320 bytes
    modes.push_back(mode);
    Serial.printf("[VIDEO] Added mode: 4-bit, %d bytes/row\n", mode.bytes_per_row);
#endif

#if VIDEO_MAX_DEPTH_BITS >= 8
    // Add 8-bit mode (256 colors)
    mode.depth = VDEPTH_8BIT;
    mode.bytes_per_row = TrivialBytesPerRow(MAC_SCREEN_WIDTH, VDEPTH_8BIT);  // 640 bytes
    modes.push_back(mode);
    Serial.printf("[VIDEO] Added mode: 8-bit, %d bytes/row\n", mode.bytes_per_row);
#endif

    // The last advertised mode is the configured default/max depth.
    current_mode = mode;

    // Initialize the renderer's hot state for the selected packed layout.
    updateVideoStateCache(MAC_SCREEN_DEPTH, mode.bytes_per_row);

    // Advertise the selected depth as the monitor default.
    the_monitor = new ESP32_monitor_desc(modes, MAC_SCREEN_DEPTH, 0x80);
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
    // Input events now signal ADB directly from the input producers.
    // Keep this hook for compatibility but avoid generating synthetic ADB IRQs
    // at 60Hz, which creates extra interrupt churn.
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

/*
 * Take a best-effort, logical-resolution snapshot for host automation.
 *
 * Palette/mode state is copied under the video spinlock. The framebuffer is
 * deliberately not held under a global critical section: copying up to 256 KB
 * from PSRAM with interrupts disabled would disturb USB, audio, and the 60 Hz
 * timer. A guest write that lands during the short row copy can affect that
 * one capture, just as it can on a physical display scanout; the next capture
 * is clean. Packed 1/2/4-bit modes are expanded to palette indices so the host
 * always receives the same simple format.
 */
bool VideoCaptureFrame(uint8 *pixels, uint32 pixel_capacity,
                       uint16 *palette, uint16 *width, uint16 *height)
{
    if (pixels == NULL || palette == NULL || width == NULL || height == NULL ||
        mac_frame_buffer == NULL) {
        return false;
    }

    const uint16 capture_width = MAC_SCREEN_WIDTH;
    const uint16 capture_height = MAC_SCREEN_HEIGHT;
    const uint32 required = (uint32)capture_width * capture_height;
    if (pixel_capacity < required) {
        return false;
    }

    video_depth depth;
    uint32 bytes_per_row;
    portENTER_CRITICAL(&frame_spinlock);
    depth = current_depth;
    bytes_per_row = current_bytes_per_row;
    memcpy(palette, palette_rgb565, sizeof(palette_rgb565));
    portEXIT_CRITICAL(&frame_spinlock);

    for (uint16 y = 0; y < capture_height; ++y) {
        const uint8 *src = mac_frame_buffer + (uint32)y * bytes_per_row;
        uint8 *dst = pixels + (uint32)y * capture_width;
        decodePackedRow(src, dst, capture_width, depth);
        if ((y & 0x1f) == 0x1f) {
            taskYIELD();
        }
    }

    *width = capture_width;
    *height = capture_height;
    return true;
}
