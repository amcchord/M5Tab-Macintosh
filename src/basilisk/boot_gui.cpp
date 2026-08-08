/*
 *  boot_gui.cpp - Pre-boot configuration GUI
 *
 *  BasiliskII ESP32 Port
 *
 *  Classic Macintosh-style boot configuration screen with:
 *  - 3-second countdown to auto-boot
 *  - Hard disk image selection
 *  - CD-ROM ISO selection
 *  - RAM size selection (4/8/12/16 MB)
 *  - WiFi network configuration
 *  - Settings persistence to SD card
 *
 *  Touch handling runs in a dedicated FreeRTOS task for responsiveness.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <vector>
#include <string>
#include <climits>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "boot_gui.h"
#include "board.h"
#include "board_config.h"
#include "board_display.h"
#include "board_touch.h"
#include "board_wifi.h"
#include "board_sd.h"  /* SD_FS alias */
#include "chicago_font.h"
#include "asset_bg_tile.h"   /* Classic 50% gray desktop stipple pattern */
#include "usb_msc.h"

// Balanced defaults: auto-connect and keep WiFi for emulator networking.
#ifndef BOOTGUI_ENABLE_WIFI_AUTOCONNECT
#define BOOTGUI_ENABLE_WIFI_AUTOCONNECT 1
#endif

#ifndef BOOTGUI_KEEP_WIFI_FOR_EMULATOR
#define BOOTGUI_KEEP_WIFI_FOR_EMULATOR 1
#endif

// ============================================================================
// Touch Task Infrastructure
// ============================================================================

// Touch event structure for queue-based input handling
typedef struct {
    int x;
    int y;
    bool is_pressed;
    bool was_pressed;
    bool was_released;
} TouchEvent;

// Touch task handles
static QueueHandle_t touch_queue = nullptr;
static TaskHandle_t touch_task_handle = nullptr;
static volatile bool touch_task_running = false;

#define TOUCH_TASK_STACK_SIZE 4096
#define TOUCH_TASK_PRIORITY   1
#define TOUCH_POLL_INTERVAL_MS 16  // ~60Hz polling

// Track previous touch state for edge detection
static volatile bool touch_prev_pressed = false;
static volatile bool touch_edge_pressed = false;
static volatile bool touch_edge_released = false;
static portMUX_TYPE touch_spinlock = portMUX_INITIALIZER_UNLOCKED;

// Touch task function - runs on Core 0
static void touchTask(void* param)
{
    Serial.println("[BOOT_GUI] Touch task started");
    
    TouchEvent evt;
    bool local_prev_pressed = false;
    // Last-known press coordinates. The Waveshare touch HAL zeros (x,y)
    // the instant the finger lifts, so the raw release sample carries
    // useless coordinates. Cache the most recent pressed position here
    // and re-use it on release so the UI can hit-test where the finger
    // actually lifted (keyboard keys, Done/Cancel buttons, list rows).
    int16_t last_press_x = 0;
    int16_t last_press_y = 0;
    
    while (touch_task_running) {
        // Refresh the board's cached touch state
        Board_Update();

        // Push any pending framebuffer updates to the panel. Both
        // boards flush the MiniGfx PSRAM framebuffer to the MIPI-DSI
        // back buffer here so the user sees fresh redraws.
        BoardDisplay_Present();

        BoardTouchDetail touch = BoardTouch_GetDetail();
        bool current_pressed = touch.pressed;

        // Detect edges ourselves (don't rely on M5's edge detection)
        bool just_pressed = current_pressed && !local_prev_pressed;
        bool just_released = !current_pressed && local_prev_pressed;

        // Fill event structure. While the finger is down, pass the live
        // coordinates and remember them. Once it lifts, re-use the last
        // cached position so the release-edge consumer sees the spot the
        // user was actually pointing at, not (0,0).
        if (current_pressed) {
            last_press_x = (int16_t)touch.x;
            last_press_y = (int16_t)touch.y;
            evt.x = touch.x;
            evt.y = touch.y;
        } else {
            evt.x = last_press_x;
            evt.y = last_press_y;
        }
        evt.is_pressed = current_pressed;
        
        // Use spinlock to safely set edge flags
        portENTER_CRITICAL(&touch_spinlock);
        if (just_pressed) {
            touch_edge_pressed = true;
        }
        if (just_released) {
            touch_edge_released = true;
        }
        evt.was_pressed = touch_edge_pressed;
        evt.was_released = touch_edge_released;
        portEXIT_CRITICAL(&touch_spinlock);
        
        // Overwrite queue with latest touch state (non-blocking)
        xQueueOverwrite(touch_queue, &evt);
        
        local_prev_pressed = current_pressed;
        
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_INTERVAL_MS));
    }
    
    Serial.println("[BOOT_GUI] Touch task stopped");
    vTaskDelete(NULL);
}

// Start the touch task (also exposed publicly as BootGUI_StartTouchTask so
// that MacSplash can reuse the same polling task before BootGUI_Init runs).
static bool startTouchTask(void)
{
    if (touch_task_running) {
        return true;  // Already running
    }
    
    // Create queue for single touch event (overwrite mode)
    touch_queue = xQueueCreate(1, sizeof(TouchEvent));
    if (!touch_queue) {
        Serial.println("[BOOT_GUI] ERROR: Failed to create touch queue");
        return false;
    }
    
    // Reset edge detection state
    touch_prev_pressed = false;
    touch_edge_pressed = false;
    touch_edge_released = false;
    
    // Initialize queue with empty event
    TouchEvent empty_evt = {0, 0, false, false, false};
    xQueueOverwrite(touch_queue, &empty_evt);
    
    touch_task_running = true;
    
    // Create touch task on Core 0
    BaseType_t result = xTaskCreatePinnedToCore(
        touchTask,
        "BootGUI_Touch",
        TOUCH_TASK_STACK_SIZE,
        NULL,
        TOUCH_TASK_PRIORITY,
        &touch_task_handle,
        0  // Core 0
    );
    
    if (result != pdPASS) {
        Serial.println("[BOOT_GUI] ERROR: Failed to create touch task");
        touch_task_running = false;
        vQueueDelete(touch_queue);
        touch_queue = nullptr;
        return false;
    }
    
    Serial.println("[BOOT_GUI] Touch task created successfully");
    return true;
}

// Stop the touch task
static void stopTouchTask(void)
{
    if (!touch_task_running) {
        return;
    }
    
    Serial.println("[BOOT_GUI] Stopping touch task...");
    
    // Signal task to stop
    touch_task_running = false;
    
    // Wait for task to notice the flag and exit its loop
    // The task has a 16ms delay, so wait a bit longer
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Explicitly delete the task if it's still running
    if (touch_task_handle != nullptr) {
        // Check if task still exists before deleting
        eTaskState state = eTaskGetState(touch_task_handle);
        if (state != eDeleted && state != eInvalid) {
            vTaskDelete(touch_task_handle);
            Serial.println("[BOOT_GUI] Touch task explicitly deleted");
        }
        touch_task_handle = nullptr;
    }
    
    // Now safe to delete the queue since task is definitely gone
    if (touch_queue) {
        vQueueDelete(touch_queue);
        touch_queue = nullptr;
    }
    
    Serial.println("[BOOT_GUI] Touch task cleanup complete");
}

// Get the latest touch event from the queue.
//
// The queue is written with xQueueOverwrite every ~16ms and polled here
// every ~1ms, so the same queued event is visible to many callers in a
// row. If we trusted the queued `was_pressed` / `was_released` flags we
// would re-fire the same rising/falling edge up to ~16 times per tap,
// which would (among other things) type 16 copies of every keyboard
// keystroke. Instead, ignore the queued edge bits entirely: take the
// authoritative edge state directly from the spinlock-guarded globals
// and clear them in the same atomic block so the edge is consumed
// exactly once no matter how often this function is called.
static bool getTouchEvent(TouchEvent* evt)
{
    if (!touch_queue || !evt) {
        return false;
    }
    
    // Peek at the queue (non-blocking, doesn't remove item)
    if (xQueuePeek(touch_queue, evt, 0) != pdTRUE) {
        return false;
    }
    
    // Override the queued edge flags with (and then clear) the live
    // globals. Using one critical section for both the read and the
    // clear means we can't race the touch task and drop a newly-set
    // edge.
    portENTER_CRITICAL(&touch_spinlock);
    evt->was_pressed  = touch_edge_pressed;
    evt->was_released = touch_edge_released;
    touch_edge_pressed  = false;
    touch_edge_released = false;
    portEXIT_CRITICAL(&touch_spinlock);
    
    return true;
}

// ============================================================================
// Classic Mac Color Palette
// ============================================================================

#define MAC_WHITE       0xFFFF
#define MAC_BLACK       0x0000
#define MAC_LIGHT_GRAY  0xC618  // #C0C0C0
#define MAC_DARK_GRAY   0x8410  // #808080
#define MAC_DESKTOP     0xA514  // Classic Mac desktop gray pattern base

// ============================================================================
// Typography
// ============================================================================
// Classic Mac OS used a single 12pt Chicago for menu bar, buttons, dialog
// titles and body text. Chicago_* renders at the font's native pixel size
// (23 px line height) - no scaling; that's what keeps the glyphs crisp on
// a 1280-wide display.

// Menu bar has to be tall enough to comfortably fit a 23px line height
// with a little breathing room below the baseline.
#define MENU_BAR_HEIGHT 28
#define CLOSE_BOX_SIZE  11

// ============================================================================
// UI Layout Constants - Touch-friendly for 5" 1280x720 display
// ============================================================================

// Display dimensions (will be set at runtime)
static int SCREEN_WIDTH = 1280;
static int SCREEN_HEIGHT = 720;

// Full-screen layout with minimal margins
#define SCREEN_MARGIN   20
#define TITLE_BAR_HEIGHT 50
#define CONTENT_PADDING 15

// Button dimensions - large for easy touch
#define BUTTON_HEIGHT   70
#define BUTTON_PADDING  10

// List box dimensions - big items for easy touch
#define LIST_ITEM_HEIGHT 55
#define LIST_MAX_VISIBLE 6

// Radio button dimensions - large touch targets
#define RADIO_SIZE      40
#define RADIO_SPACING   140

// ============================================================================
// Settings Storage
// ============================================================================

static char selected_disk_path[BOOT_GUI_MAX_PATH] = "";
static char selected_cdrom_path[BOOT_GUI_MAX_PATH] = "";
static char selected_extfs_path[BOOT_GUI_MAX_PATH] = "";
static int selected_ram_mb = 8;  // Default 8MB
static bool skip_gui = false;    // If true, skip boot GUI and go straight to emulator

// WiFi settings storage
static char wifi_ssid[64] = "";
static char wifi_password[64] = "";
static bool wifi_auto_connect = false;
static bool wifi_initialized = false;

// Audio settings
static bool audio_enabled = true;  // Default: audio enabled

// "Boot from CD" - when set, LoadPrefs steers bootdrive to the CD-ROM
// driver refnum so the Mac boots from the selected ISO instead of the
// hard disk. Equivalent to holding "C" at startup on a real Mac.
static bool boot_from_cd = false;

// Display rotation in degrees (0 or 180). The Tab5 default is 180 to
// match v4.0 behaviour (USB-C port on the left); 0 puts the USB-C
// port on the right. Waveshare ignores this for now.
static int rotation_degrees = 180;

static const char* SETTINGS_FILE = "/basilisk_settings.txt";

// ============================================================================
// File Lists
// ============================================================================

static std::vector<std::string> disk_files;
static std::vector<std::string> cdrom_files;
static std::vector<std::string> extfs_folders;

static int disk_selection_index = 0;
static int cdrom_selection_index = 0;  // 0 = None
static int extfs_selection_index = 0;  // 0 = None
static int disk_scroll_offset = 0;
static int cdrom_scroll_offset = 0;
static int extfs_scroll_offset = 0;

// ============================================================================
// UI State
// ============================================================================

static bool gui_initialized = false;

// Alias to the board's drawing surface. Both boards now use the
// MiniGfx software framebuffer; it flushes to the panel on the next
// BoardDisplay_Present().
#define gfx BoardDisplay_Gfx()

// ============================================================================
// Forward Declarations
// ============================================================================

static void loadSettings(void);
static void saveSettings(void);
static void scanDiskFiles(void);
static void scanCDROMFiles(void);
static void drawDesktopPattern(void);
static void drawWindow(int x, int y, int w, int h, const char* title);
static void drawButton(int x, int y, int w, int h, const char* label,
                       bool pressed, bool is_default = false);
static void drawListBox(int x, int y, int w, int h, const std::vector<std::string>& items, 
                        int selected, int scroll_offset, bool include_none);
static void drawRadioButton(int x, int y, const char* label, bool selected);
static bool isPointInRect(int px, int py, int rx, int ry, int rw, int rh);
static void runSettingsScreen(void);
static void runWiFiScreen(void);
static void runUsbMscScreen(void);
static void initWiFi(void);
static void drawC6FirmwareUpdateSplash(void);
static void runC6FirmwareUpdateIfNeeded(void);
static void drawKeyboard(int x, int y, int w, int h, bool shift_active, int highlight_key);
static int getKeyboardKey(int touch_x, int touch_y, int kb_x, int kb_y, int kb_w, int kb_h);
static void drawSignalBars(int x, int y, int rssi);
static void drawWifiStatusStrip(int x, int y, int w, int h);

// ============================================================================
// Settings Load/Save
// ============================================================================

static void loadSettings(void)
{
    Serial.println("[BOOT_GUI] Loading settings...");
    
    File file = SD_FS.open(SETTINGS_FILE, FILE_READ);
    if (!file) {
        Serial.println("[BOOT_GUI] No settings file found, using defaults");
        return;
    }
    
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        
        int eq_pos = line.indexOf('=');
        if (eq_pos <= 0) {
            continue;
        }
        
        String key = line.substring(0, eq_pos);
        String value = line.substring(eq_pos + 1);
        key.trim();
        value.trim();
        
        if (key == "disk") {
            strncpy(selected_disk_path, value.c_str(), BOOT_GUI_MAX_PATH - 1);
            Serial.printf("[BOOT_GUI] Loaded disk: %s\n", selected_disk_path);
        } else if (key == "cdrom") {
            strncpy(selected_cdrom_path, value.c_str(), BOOT_GUI_MAX_PATH - 1);
            Serial.printf("[BOOT_GUI] Loaded cdrom: %s\n", selected_cdrom_path);
        } else if (key == "extfs") {
            strncpy(selected_extfs_path, value.c_str(), BOOT_GUI_MAX_PATH - 1);
            Serial.printf("[BOOT_GUI] Loaded shared folder: %s\n", selected_extfs_path);
        } else if (key == "ramsize") {
            selected_ram_mb = value.toInt();
            if (selected_ram_mb != 4 && selected_ram_mb != 8 && 
                selected_ram_mb != 12 && selected_ram_mb != 16) {
                selected_ram_mb = 8;  // Default to 8MB if invalid
            }
            Serial.printf("[BOOT_GUI] Loaded RAM: %d MB\n", selected_ram_mb);
        } else if (key == "skip_gui") {
            skip_gui = (value == "yes" || value == "true" || value == "1");
            Serial.printf("[BOOT_GUI] Loaded skip_gui: %s\n", skip_gui ? "yes" : "no");
        } else if (key == "wifi_ssid") {
            strncpy(wifi_ssid, value.c_str(), sizeof(wifi_ssid) - 1);
            Serial.printf("[BOOT_GUI] Loaded WiFi SSID: %s\n", wifi_ssid);
        } else if (key == "wifi_pass") {
            strncpy(wifi_password, value.c_str(), sizeof(wifi_password) - 1);
            Serial.println("[BOOT_GUI] Loaded WiFi password");
        } else if (key == "wifi_auto") {
            wifi_auto_connect = (value == "yes" || value == "true" || value == "1");
            Serial.printf("[BOOT_GUI] Loaded wifi_auto: %s\n", wifi_auto_connect ? "yes" : "no");
        } else if (key == "audio" || key == "audio_enabled") {
            audio_enabled = (value == "yes" || value == "true" || value == "1");
            Serial.printf("[BOOT_GUI] Loaded audio_enabled: %s\n", audio_enabled ? "yes" : "no");
        } else if (key == "boot_from_cd") {
            boot_from_cd = (value == "yes" || value == "true" || value == "1");
            Serial.printf("[BOOT_GUI] Loaded boot_from_cd: %s\n", boot_from_cd ? "yes" : "no");
        } else if (key == "rotation") {
            int rot = value.toInt();
            // Accept 0 or 180; reject anything else and fall back to 180.
            if (rot == 0 || rot == 180) {
                rotation_degrees = rot;
            } else {
                rotation_degrees = 180;
            }
            Serial.printf("[BOOT_GUI] Loaded rotation: %d\n", rotation_degrees);
        }
    }
    
    file.close();
}

static void saveSettings(void)
{
    Serial.println("[BOOT_GUI] Saving settings...");

    // Rewrite file from scratch to avoid stale duplicate keys across boots.
    if (SD_FS.exists(SETTINGS_FILE)) {
        SD_FS.remove(SETTINGS_FILE);
    }

    File file = SD_FS.open(SETTINGS_FILE, FILE_WRITE);
    if (!file) {
        Serial.println("[BOOT_GUI] ERROR: Cannot open settings file for writing");
        return;
    }
    
    file.printf("disk=%s\n", selected_disk_path);
    file.printf("cdrom=%s\n", selected_cdrom_path);
    file.printf("extfs=%s\n", selected_extfs_path);
    file.printf("ramsize=%d\n", selected_ram_mb);
    file.printf("skip_gui=%s\n", skip_gui ? "yes" : "no");
    
    // Save WiFi settings
    file.printf("wifi_ssid=%s\n", wifi_ssid);
    file.printf("wifi_pass=%s\n", wifi_password);
    file.printf("wifi_auto=%s\n", wifi_auto_connect ? "yes" : "no");
    
    // Save audio settings
    file.printf("audio=%s\n", audio_enabled ? "yes" : "no");

    // v4.5 additions: boot-from-CD and screen rotation.
    file.printf("boot_from_cd=%s\n", boot_from_cd ? "yes" : "no");
    file.printf("rotation=%d\n", rotation_degrees);

    file.close();
    Serial.println("[BOOT_GUI] Settings saved");
}

// ============================================================================
// File Scanning
// ============================================================================

static bool hasExtension(const char* filename, const char* ext)
{
    const char* dot = strrchr(filename, '.');
    if (!dot) {
        return false;
    }
    return strcasecmp(dot, ext) == 0;
}

static void scanDiskFiles(void)
{
    Serial.println("[BOOT_GUI] Scanning for disk images...");
    disk_files.clear();
    
    File root = SD_FS.open("/");
    if (!root) {
        Serial.println("[BOOT_GUI] ERROR: Cannot open SD root");
        return;
    }
    
    while (true) {
        File entry = root.openNextFile();
        if (!entry) {
            break;
        }
        
        if (!entry.isDirectory()) {
            const char* name = entry.name();
            // Skip hidden files (starting with '.')
            if (name[0] == '.') {
                entry.close();
                continue;
            }
            // Accept the common Mac disk-image extensions used in the
            // wild: .dsk, .img (Sony / floppy images), and .hfv (Win
            // Basilisk II / Mini vMac). The disk backend treats any of
            // them as raw sectors, so the file extension only gates
            // whether the picker shows the file.
            if (hasExtension(name, ".dsk") ||
                hasExtension(name, ".img") ||
                hasExtension(name, ".hfv")) {
                std::string path = "/";
                path += name;
                disk_files.push_back(path);
                Serial.printf("[BOOT_GUI] Found disk: %s\n", path.c_str());
            }
        }
        entry.close();
        
        if (disk_files.size() >= BOOT_GUI_MAX_FILES) {
            break;
        }
    }
    root.close();
    
    Serial.printf("[BOOT_GUI] Found %d disk images\n", disk_files.size());
    
    // Find index of currently selected disk
    disk_selection_index = 0;
    for (size_t i = 0; i < disk_files.size(); i++) {
        if (disk_files[i] == selected_disk_path) {
            disk_selection_index = i;
            break;
        }
    }
}

static void scanCDROMFiles(void)
{
    Serial.println("[BOOT_GUI] Scanning for CD-ROM images...");
    cdrom_files.clear();
    
    File root = SD_FS.open("/");
    if (!root) {
        Serial.println("[BOOT_GUI] ERROR: Cannot open SD root");
        return;
    }
    
    while (true) {
        File entry = root.openNextFile();
        if (!entry) {
            break;
        }
        
        if (!entry.isDirectory()) {
            const char* name = entry.name();
            // Skip hidden files (starting with '.')
            if (name[0] == '.') {
                entry.close();
                continue;
            }
            // CD-ROM images: ISO 9660 .iso, Apple Disk Copy .cdr (raw
            // sector dump used by Disk Utility), and Roxio .toast (the
            // header is a thin wrapper but most modern .toast files in
            // the wild are raw ISO data with the wrapper stripped).
            if (hasExtension(name, ".iso") ||
                hasExtension(name, ".cdr") ||
                hasExtension(name, ".toast")) {
                std::string path = "/";
                path += name;
                cdrom_files.push_back(path);
                Serial.printf("[BOOT_GUI] Found CD-ROM: %s\n", path.c_str());
            }
        }
        entry.close();
        
        if (cdrom_files.size() >= BOOT_GUI_MAX_FILES) {
            break;
        }
    }
    root.close();
    
    Serial.printf("[BOOT_GUI] Found %d CD-ROM images\n", cdrom_files.size());
    
    // Find index of currently selected CD-ROM (0 = None)
    cdrom_selection_index = 0;
    if (strlen(selected_cdrom_path) > 0) {
        for (size_t i = 0; i < cdrom_files.size(); i++) {
            if (cdrom_files[i] == selected_cdrom_path) {
                cdrom_selection_index = i + 1;  // +1 because 0 is "None"
                break;
            }
        }
    }
}

// Scan the SD root for top-level directories that can be shared with the
// guest Mac via ExtFS. The picker in the settings UI always leads with a
// synthetic "None" entry, so users can disable the shared volume without
// having to clear a text field.
static void scanExtFSFolders(void)
{
    Serial.println("[BOOT_GUI] Scanning for shared folders...");
    extfs_folders.clear();

    File root = SD_FS.open("/");
    if (!root) {
        Serial.println("[BOOT_GUI] ERROR: Cannot open SD root");
        return;
    }

    while (true) {
        File entry = root.openNextFile();
        if (!entry) {
            break;
        }

        if (entry.isDirectory()) {
            // Arduino's File::name() returns just the basename for
            // directories opened from the root. Normalize to strip any
            // leading slash the underlying driver might include.
            const char* raw = entry.name();
            if (raw == NULL) {
                entry.close();
                continue;
            }
            const char* name = (raw[0] == '/') ? raw + 1 : raw;

            // Hide dotfiles (including our own .finf/.rsrc sidecars that
            // will appear once ExtFS starts writing Finder metadata) and
            // the FAT bookkeeping folder Windows creates automatically.
            bool skip = (name[0] == '.') ||
                        (strcasecmp(name, "System Volume Information") == 0);
            if (!skip) {
                std::string path = "/";
                path += name;
                extfs_folders.push_back(path);
                Serial.printf("[BOOT_GUI] Found shared folder: %s\n",
                              path.c_str());
            }
        }
        entry.close();

        if (extfs_folders.size() >= BOOT_GUI_MAX_FILES) {
            break;
        }
    }
    root.close();

    Serial.printf("[BOOT_GUI] Found %d shared folders\n",
                  (int)extfs_folders.size());

    // Find index of currently selected folder (0 = None).
    extfs_selection_index = 0;
    if (strlen(selected_extfs_path) > 0) {
        for (size_t i = 0; i < extfs_folders.size(); i++) {
            if (extfs_folders[i] == selected_extfs_path) {
                extfs_selection_index = i + 1;  // +1 because 0 is "None"
                break;
            }
        }
        // If the saved folder no longer exists on the card, fall back to
        // "None" rather than silently keeping a dangling path.
        if (extfs_selection_index == 0) {
            Serial.printf("[BOOT_GUI] Saved shared folder %s no longer present, defaulting to None\n",
                          selected_extfs_path);
            selected_extfs_path[0] = '\0';
        }
    }
}

// ============================================================================
// Drawing Functions - Desktop Pattern
// ============================================================================

static void drawDesktopPattern(void)
{
    // Tile the classic 50% gray stipple (same pattern MacSplash uses) at
    // 2x so each source pixel becomes a 2x2 block. On the Waveshare
    // 1280x800 panel this lines up with the Mac OS desktop pattern.
    Gfx_TileBackground(BG_TILE_PIXELS, BG_TILE_W, BG_TILE_H, /*scale=*/2);
}

// Re-tile a rectangle with the desktop stipple so partial redraws can
// erase prior content without leaving a flat-gray patch over the pattern.
// Matches the same 2x block scale drawDesktopPattern() uses.
static void fillWithDesktopStipple(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    const int block = 2;
    int x_end = x + w;
    int y_end = y + h;
    for (int yy = y; yy < y_end; yy += block) {
        int src_y = (yy / block) % BG_TILE_H;
        for (int xx = x; xx < x_end; xx += block) {
            int src_x   = (xx / block) % BG_TILE_W;
            uint16_t c  = BG_TILE_PIXELS[src_y * BG_TILE_W + src_x];
            int bw      = block;
            int bh      = block;
            if (xx + bw > x_end) bw = x_end - xx;
            if (yy + bh > y_end) bh = y_end - yy;
            gfx.fillRect(xx, yy, bw, bh, c);
        }
    }
}

// (drawLabelBadge removed - every label now sits on a real Mac window's
// white body, so plain Chicago_DrawString reads cleanly without a paper
// backing hack.)

// ============================================================================
// Classic Mac menu bar - drawn at the top of every pre-boot screen.
// White background, 1px black separator, an Apple glyph on the left, the
// screen title in Chicago, and a running clock on the right.
// ============================================================================
static void drawAppleGlyph(int x, int y)
{
    // Tiny 10x11 black silhouette that reads as the classic Apple mark at
    // a glance. Not pixel-accurate to the real Mac ROM, but good enough
    // for flavor at this size.
    const uint16_t bits[11] = {
        0b0000011000,
        0b0000110000,
        0b0000110000,
        0b0001110000,
        0b0111111110,
        0b1111111111,
        0b1111111111,
        0b1111111111,
        0b0111111110,
        0b0011111100,
        0b0010100100,
    };
    for (int row = 0; row < 11; ++row) {
        uint16_t bb = bits[row];
        for (int col = 0; col < 10; ++col) {
            if (bb & (1u << (9 - col))) {
                gfx.fillRect(x + col, y + row, 1, 1, MAC_BLACK);
            }
        }
    }
}

// Format a short "H:MM AM"-style clock string into `out` for the right side
// of the menu bar. If a real wall-clock is available (time() is non-zero),
// we use it; otherwise we fall back to "uptime" - minutes-since-boot rendered
// as "00:MM" - so the bar always has something to show without screaming
// about a missing RTC.
static void formatMenuClock(char *out, size_t n)
{
    time_t now = time(nullptr);
    if (now > 100000) {  // anything vaguely sane means we have an RTC / NTP
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        int hour12 = tm_now.tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        snprintf(out, n, "%d:%02d %s",
                 hour12, tm_now.tm_min,
                 tm_now.tm_hour >= 12 ? "PM" : "AM");
        return;
    }

    uint32_t up_s  = millis() / 1000;
    uint32_t up_m  = up_s / 60;
    uint32_t up_h  = up_m / 60;
    snprintf(out, n, "%u:%02u", (unsigned)(up_h % 100), (unsigned)(up_m % 60));
}

static void drawMenuBar(const char *title)
{
    (void)title;  // Screen name is shown as a window title instead.

    // Background fill.
    gfx.fillRect(0, 0, SCREEN_WIDTH, MENU_BAR_HEIGHT, MAC_WHITE);
    // 1px bottom separator.
    gfx.drawFastHLine(0, MENU_BAR_HEIGHT - 1, SCREEN_WIDTH, MAC_BLACK);

    // Apple glyph on the left.
    drawAppleGlyph(10, (MENU_BAR_HEIGHT - 11) / 2);

    // Static menu titles. Purely decorative - these don't open anything.
    int base_x = 40;
    int text_y = (MENU_BAR_HEIGHT - Chicago_LineHeight()) / 2;
    if (text_y < 0) text_y = 0;
    const char *items[] = { "File", "Edit", "View", "Special" };
    for (int i = 0; i < 4; ++i) {
        Chicago_DrawString(items[i], base_x, text_y, MAC_BLACK, TL_DATUM);
        base_x += Chicago_MeasureWidth(items[i]) + 18;
    }

    // Running clock on the right edge, Chicago, right-aligned with 10 px
    // of padding so it doesn't hug the screen edge.
    char clock_buf[16];
    formatMenuClock(clock_buf, sizeof(clock_buf));
    Chicago_DrawString(clock_buf, SCREEN_WIDTH - 10, text_y,
                       MAC_BLACK, TR_DATUM);
}

// Decorative close-box drawn on windows. Matches the classic Mac design:
// 11x11 square with a 3px white inset so it reads as "hollow" at a glance.
static void drawCloseBox(int x, int y)
{
    gfx.drawRect(x, y, CLOSE_BOX_SIZE, CLOSE_BOX_SIZE, MAC_BLACK);
    gfx.fillRect(x + 1, y + 1, CLOSE_BOX_SIZE - 2, CLOSE_BOX_SIZE - 2,
                 MAC_WHITE);
}

// ============================================================================
// Drawing Functions - Window
// ============================================================================
// Classic Mac window chrome:
//   * 1 px black frame, 2 px black drop shadow on the bottom + right edges.
//   * Title bar is the classic 6 horizontal hairlines with a centered
//     white "paper" cutout around the Chicago title. The cutout is only
//     as wide as the text + 12 px so the stripes flank the title crisply.
//   * A decorative close-box sits at the top-left. Sub-panel windows
//     (used for inline group-boxes like the Memory radios) skip the
//     close-box and use a shorter title strip.

static constexpr int WINDOW_TITLE_STRIPES = 6;

static void drawTitleStripes(int left, int top, int width, int height)
{
    // Paint the striped bar background in black, then carve out
    // white gaps so we end up with exactly WINDOW_TITLE_STRIPES
    // horizontal 1 px black lines evenly spread across `height`.
    if (width <= 0 || height <= 0) return;
    gfx.fillRect(left, top, width, height, MAC_WHITE);

    // Centre the 6-stripe block vertically; stripes are spaced every 2 px
    // (1 px black, 1 px white), which matches the original Mac look.
    int stripe_block = WINDOW_TITLE_STRIPES * 2 - 1;  // 11 px tall
    int stripe_top   = top + (height - stripe_block) / 2;
    if (stripe_top < top) stripe_top = top;

    for (int i = 0; i < WINDOW_TITLE_STRIPES; ++i) {
        int ly = stripe_top + i * 2;
        if (ly >= top + height) break;
        gfx.drawFastHLine(left, ly, width, MAC_BLACK);
    }
}

static void drawWindowEx(int x, int y, int w, int h, const char *title,
                         bool is_sub_panel)
{
    // 2 px black drop shadow along bottom and right edges (classic Mac
    // "hard" shadow, not the wide gray block we used to draw).
    gfx.fillRect(x + 2, y + h, w, 2, MAC_BLACK);      // bottom
    gfx.fillRect(x + w, y + 2, 2, h, MAC_BLACK);      // right

    // Window body.
    gfx.fillRect(x, y, w, h, MAC_WHITE);
    gfx.drawRect(x, y, w, h, MAC_BLACK);

    // Title bar geometry. Sub-panels use a shorter bar so the group-box
    // stays compact inside the main window.
    int title_h = is_sub_panel ? TITLE_BAR_HEIGHT / 2 : TITLE_BAR_HEIGHT;
    int title_top = y + 1;

    // Striped title bar (draws over the interior).
    int stripes_left  = x + 1;
    int stripes_width = w - 2;
    drawTitleStripes(stripes_left, title_top, stripes_width, title_h);

    // White "paper" cutout for the title, width = text + 12 px. Chicago
    // sits vertically centred in the title bar.
    if (title && title[0]) {
        int title_paper_w = Chicago_MeasureWidth(title) + 12;
        int title_paper_x = x + (w - title_paper_w) / 2;
        gfx.fillRect(title_paper_x, title_top, title_paper_w, title_h,
                     MAC_WHITE);
        Chicago_DrawString(title, x + w / 2, title_top + title_h / 2,
                           MAC_BLACK, MC_DATUM);
    }

    // Decorative close-box (main windows only).
    if (!is_sub_panel) {
        int cb_y = title_top + (title_h - CLOSE_BOX_SIZE) / 2;
        drawCloseBox(x + 8, cb_y);
    }

    // 1 px divider under the title bar that completes the classic look.
    gfx.drawFastHLine(x + 1, title_top + title_h, w - 2, MAC_BLACK);
}

static void drawWindow(int x, int y, int w, int h, const char *title)
{
    drawWindowEx(x, y, w, h, title, /*is_sub_panel=*/false);
}

static void drawSubPanel(int x, int y, int w, int h, const char *title)
{
    drawWindowEx(x, y, w, h, title, /*is_sub_panel=*/true);
}

// Return the pixel Y offset (relative to window top) at which content
// drawing starts, i.e. just below the title bar + divider line. Used by
// callers laying out the inside of a drawWindow / drawSubPanel.
static int windowContentTop(bool is_sub_panel)
{
    int title_h = is_sub_panel ? TITLE_BAR_HEIGHT / 2 : TITLE_BAR_HEIGHT;
    return 1 + title_h + 1;  // 1 px frame + title bar + 1 px divider
}

// ============================================================================
// Drawing Functions - Button
// ============================================================================

// Classic Mac default button has an outer 1 px black ring, a 3 px gap,
// then the regular 1 px button frame. When painting the halo we need to
// erase the area with the desktop stipple so multiple repaints don't
// leave concentric rectangles layered on top of each other.
static constexpr int DEFAULT_BUTTON_HALO_GAP = 3;

static void drawButton(int x, int y, int w, int h, const char* label,
                       bool pressed, bool is_default)
{
    if (is_default) {
        // Erase the halo region first so a repaint doesn't leave the
        // previous ring behind.
        int halo_pad = DEFAULT_BUTTON_HALO_GAP + 1;
        fillWithDesktopStipple(x - halo_pad, y - halo_pad,
                               w + halo_pad * 2, h + halo_pad * 2);

        // Draw the outer black ring.
        int hx = x - halo_pad;
        int hy = y - halo_pad;
        int hw = w + halo_pad * 2;
        int hh = h + halo_pad * 2;
        gfx.drawRect(hx, hy, hw, hh, MAC_BLACK);
    }

    uint16_t fg;
    if (pressed) {
        // Pressed state - inverted (classic Mac push-button selected state).
        gfx.fillRect(x, y, w, h, MAC_BLACK);
        fg = MAC_WHITE;
    } else {
        // Normal state - white fill with a heavy black border on the
        // bottom/right (the classic "default button" drop shadow), a
        // 1px black outline, and a 1px white highlight on the
        // top/left so the button reads as raised.
        gfx.fillRect(x, y, w, h, MAC_WHITE);

        // 1-px raised highlight (top/left).
        gfx.drawFastHLine(x + 1, y + 1, w - 2, MAC_WHITE);
        gfx.drawFastVLine(x + 1, y + 1, h - 2, MAC_WHITE);

        // Dropshadow (bottom/right).
        gfx.drawFastHLine(x + 1, y + h - 1, w - 2, MAC_DARK_GRAY);
        gfx.drawFastVLine(x + w - 1, y + 1, h - 2, MAC_DARK_GRAY);

        // 1-px border around the whole thing.
        gfx.drawRect(x, y, w, h, MAC_BLACK);

        fg = MAC_BLACK;
    }

    // Chicago button label - one native Chicago, per the classic
    // "single 12 pt Chicago everywhere" rule.
    Chicago_DrawString(label, x + w / 2, y + h / 2, fg,
                       MC_DATUM);
}

// ============================================================================
// Drawing Functions - List Box
// ============================================================================

static void drawListBox(int x, int y, int w, int h, const std::vector<std::string>& items,
                        int selected, int scroll_offset, bool include_none)
{
    // Background
    gfx.fillRect(x, y, w, h, MAC_WHITE);

    // Classic single 1 px black frame plus a 1 px white inset so rows
    // breathe away from the border.
    gfx.drawRect(x, y, w, h, MAC_BLACK);

    // Calculate visible items
    int visible_count = (h - 6) / LIST_ITEM_HEIGHT;
    int total_items = items.size();
    if (include_none) {
        total_items++;
    }
    
    // Reserve a 16px scrollbar rail on the right edge (classic Mac look).
    const int SCROLLBAR_W = 16;
    int content_w = w - SCROLLBAR_W;

    for (int i = 0; i < visible_count && (i + scroll_offset) < total_items; i++) {
        int item_index = i + scroll_offset;
        int item_y = y + 3 + i * LIST_ITEM_HEIGHT;
        
        const char* item_text;
        if (include_none && item_index == 0) {
            item_text = "(None)";
        } else {
            int file_index = item_index;
            if (include_none) {
                file_index--;
            }
            if (file_index >= 0 && file_index < (int)items.size()) {
                // Show just the filename, not the full path
                const char* path = items[file_index].c_str();
                item_text = path;
                if (path[0] == '/') {
                    item_text = path + 1;  // Skip leading slash
                }
            } else {
                continue;
            }
        }

        uint16_t fg = MAC_BLACK;
        if (item_index == selected) {
            // Selected item - inverted with padding (classic List Manager
            // highlight).
            gfx.fillRect(x + 3, item_y, content_w - 6, LIST_ITEM_HEIGHT, MAC_BLACK);
            fg = MAC_WHITE;
        }

        // Truncate text to roughly fit the visible area.
        char truncated[48];
        strncpy(truncated, item_text, sizeof(truncated) - 1);
        truncated[sizeof(truncated) - 1] = '\0';
        // Rough char-budget: we have (content_w - 12) pixels at native Chicago.
        // pixChicago glyphs average ~9px at scale=1 => ~one char per 9px.
        int char_budget = (content_w - 12) / 9;
        if (char_budget > 0 && (int)strlen(truncated) > char_budget) {
            truncated[char_budget - 3] = '\0';
            strcat(truncated, "...");
        }

        Chicago_DrawString(truncated, x + 8, item_y + LIST_ITEM_HEIGHT / 2,
                           fg, ML_DATUM);
    }

    // ------------------------------------------------------------------
    // Scrollbar rail on the right edge. Classic Mac look: white "elevator"
    // rail bounded by black, arrow boxes top and bottom, and a shaded
    // thumb indicating scroll position.
    // ------------------------------------------------------------------
    int sb_x = x + content_w;
    int sb_y = y + 3;
    int sb_h = h - 6;
    // Rail
    gfx.fillRect(sb_x, sb_y, SCROLLBAR_W, sb_h, MAC_WHITE);
    gfx.drawRect(sb_x, sb_y, SCROLLBAR_W, sb_h, MAC_BLACK);
    // Top arrow box
    int arrow_h = 16;
    gfx.drawFastHLine(sb_x, sb_y + arrow_h, SCROLLBAR_W, MAC_BLACK);
    if (scroll_offset > 0) {
        gfx.fillTriangle(sb_x + SCROLLBAR_W / 2, sb_y + 4,
                         sb_x + 3, sb_y + arrow_h - 3,
                         sb_x + SCROLLBAR_W - 3, sb_y + arrow_h - 3,
                         MAC_BLACK);
    }
    // Bottom arrow box
    gfx.drawFastHLine(sb_x, sb_y + sb_h - arrow_h, SCROLLBAR_W, MAC_BLACK);
    if (scroll_offset + visible_count < total_items) {
        gfx.fillTriangle(sb_x + 3, sb_y + sb_h - arrow_h + 3,
                         sb_x + SCROLLBAR_W - 3, sb_y + sb_h - arrow_h + 3,
                         sb_x + SCROLLBAR_W / 2, sb_y + sb_h - 4,
                         MAC_BLACK);
    }
    // Solid-with-border elevator thumb (System 7 look).
    if (total_items > visible_count) {
        int track_h    = sb_h - arrow_h * 2;
        int thumb_h    = (track_h * visible_count) / total_items;
        if (thumb_h < 16) thumb_h = 16;
        int thumb_max  = track_h - thumb_h;
        int thumb_pos  = (scroll_offset * thumb_max) /
                         (total_items - visible_count);
        if (thumb_pos < 0) thumb_pos = 0;
        int thumb_y    = sb_y + arrow_h + thumb_pos;
        gfx.fillRect(sb_x + 2, thumb_y + 1,
                     SCROLLBAR_W - 4, thumb_h - 2, MAC_LIGHT_GRAY);
        gfx.drawRect(sb_x + 2, thumb_y + 1,
                     SCROLLBAR_W - 4, thumb_h - 2, MAC_BLACK);
    }
}

// ============================================================================
// Drawing Functions - Radio Button
// ============================================================================

static void drawRadioButton(int x, int y, const char* label, bool selected)
{
    // Large touch-friendly radio button
    int r = RADIO_SIZE / 2;
    int cx = x + r;
    int cy = y + r;
    
    // White background circle
    gfx.fillCircle(cx, cy, r, MAC_WHITE);
    
    // Outer circle border
    gfx.drawCircle(cx, cy, r, MAC_BLACK);
    gfx.drawCircle(cx, cy, r - 1, MAC_BLACK);
    
    // Fill center if selected
    if (selected) {
        gfx.fillCircle(cx, cy, r - 6, MAC_BLACK);
    }

    // Label in Chicago. Radios now live inside a white window/sub-panel
    // so no badge backing is needed - plain Chicago reads cleanly.
    Chicago_DrawString(label, x + RADIO_SIZE + 10, cy,
                       MAC_BLACK, ML_DATUM);
}

// ============================================================================
// Drawing Functions - Checkbox
// ============================================================================

static void drawCheckbox(int x, int y, int size, const char* label, bool checked)
{
    // White background square
    gfx.fillRect(x, y, size, size, MAC_WHITE);
    
    // Border
    gfx.drawRect(x, y, size, size, MAC_BLACK);
    gfx.drawRect(x + 1, y + 1, size - 2, size - 2, MAC_BLACK);
    
    // Draw checkmark if checked
    if (checked) {
        // Draw a simple X or checkmark
        int margin = 6;
        int x1 = x + margin;
        int y1 = y + margin;
        int x2 = x + size - margin;
        int y2 = y + size - margin;
        
        // Draw thick checkmark lines
        for (int i = -1; i <= 1; i++) {
            gfx.drawLine(x1, y1 + size/4 + i, x + size/2, y2 + i, MAC_BLACK);
            gfx.drawLine(x + size/2, y2 + i, x2, y1 + i, MAC_BLACK);
        }
    }

    // Label in Chicago. Checkbox lives on the main window's white body,
    // so plain Chicago is crisp without a badge backing.
    Chicago_DrawString(label, x + size + 10, y + size / 2,
                       MAC_BLACK, ML_DATUM);
}

// ============================================================================
// WiFi Initialization
// ============================================================================

static void initWiFi(void)
{
    if (wifi_initialized) {
        return;
    }
    
    Serial.println("[BOOT_GUI] Initializing WiFi...");

    // Configure the board-specific WiFi transport:
    //   - Tab5: hard-coded SDIO2 pins to the ESP32-C6 via WiFi.setPins()
    //   - Waveshare: no-op (esp_wifi_remote / esp_hosted auto-configures)
    BoardWifi_Prepare();
    
    // Set WiFi mode to station
    WiFi.mode(WIFI_STA);
    
    // Disable auto-reconnect so failed connections don't keep retrying
    // in the background (which blocks scanning)
    WiFi.setAutoReconnect(false);
    
    // Disconnect from any previous connection
    WiFi.disconnect();
    
    wifi_initialized = true;
    Serial.println("[BOOT_GUI] WiFi initialized");
}

// ============================================================================
// WiFi Co-processor Firmware Update
//
// On boards that talk WiFi via an SDIO-attached co-processor (ESP32-C6 on
// the Waveshare P4 10.1), the slave firmware version is pinned by the
// Arduino core we link against. A mismatch leaves WiFi stuck at
// "Could not get slave firmware version" forever - even once the user
// has selected a network. To keep the device recoverable without
// needing Espressif's HTTPS CDN (chicken-and-egg since WiFi is broken)
// we bake the matching `esp32c6-v<X.Y.Z>.bin` blob into flash via
// scripts/fetch_c6_firmware.py and stream it into the slave over the
// already-up hosted transport. On Tab5 both hooks are no-ops (the Tab5
// stub in board_wifi_firmware_tab5.cpp always reports up-to-date).
// ============================================================================

static void drawC6FirmwareUpdateSplash(void)
{
    // Center a Mac-style alert panel explaining the long blocking op.
    // Matches the USB Disk splash visual language so the device looks
    // consistent while it's frozen for 10-30s streaming the blob.
    drawDesktopPattern();
    drawMenuBar("Basilisk II");

    int panel_w = SCREEN_WIDTH * 3 / 5;
    int panel_h = 260;
    if (panel_w < 480) panel_w = 480;
    int panel_x = (SCREEN_WIDTH  - panel_w) / 2;
    int panel_y = (SCREEN_HEIGHT - panel_h) / 2;
    drawWindow(panel_x, panel_y, panel_w, panel_h, "Updating WiFi");

    int line_h = Chicago_LineHeight() + 4;
    int text_x = panel_x + panel_w / 2;
    int text_y = panel_y + TITLE_BAR_HEIGHT + 40;

    Chicago_DrawString("Updating the WiFi co-processor firmware.",
                       text_x, text_y, MAC_BLACK, TC_DATUM);
    text_y += line_h;
    Chicago_DrawString("This only happens once after a firmware update.",
                       text_x, text_y, MAC_BLACK, TC_DATUM);
    text_y += line_h * 2;

    Chicago_DrawString("Please wait - the device will restart",
                       text_x, text_y, MAC_BLACK, TC_DATUM);
    text_y += line_h;
    Chicago_DrawString("automatically when the update is complete.",
                       text_x, text_y, MAC_BLACK, TC_DATUM);
    text_y += line_h * 2;

    Chicago_DrawString("Do not unplug or power off.",
                       text_x, text_y, MAC_DARK_GRAY, TC_DATUM);

    BoardDisplay_Present();
}

static void runC6FirmwareUpdateIfNeeded(void)
{
    // Tab5 stub short-circuits; on Waveshare this queries the hosted
    // version RPC, so the transport must already be up (we ensured
    // that via initWiFi() in BootGUI_Init before getting here).
    if (!BoardWifi_IsCoprocessorFirmwareOutdated()) {
        return;
    }

    Serial.println("[BOOT_GUI] WiFi co-processor firmware is outdated; applying embedded update...");
    drawC6FirmwareUpdateSplash();

    BoardWifiFirmwareResult r = BoardWifi_ApplyCoprocessorFirmware();
    switch (r) {
        case BOARD_WIFI_FW_UPDATED: {
            // hostedActivateUpdate() has already triggered a C6 reset;
            // the SDIO link is torn down, so the cleanest way to come
            // up on matched firmware is a full host reboot. Give the
            // user a beat to read the splash, then restart.
            Serial.println("[BOOT_GUI] WiFi co-processor updated; rebooting host in 2s...");
            delay(2000);
            ESP.restart();
            // not reached
            break;
        }
        case BOARD_WIFI_FW_FAILED:
            Serial.println("[BOOT_GUI] WiFi co-processor update FAILED; continuing without WiFi");
            // Fall through - boot continues, WiFi will be unusable this
            // session but everything else (SD, emulator) still works.
            break;
        case BOARD_WIFI_FW_UP_TO_DATE:
        case BOARD_WIFI_FW_NOT_SUPPORTED:
        default:
            break;
    }
}

// ============================================================================
// Drawing Functions - Signal Bars
// ============================================================================

static void drawSignalBars(int x, int y, int rssi)
{
    // Convert RSSI to bar count (0-4 bars)
    // Typical RSSI ranges: -30 (excellent) to -90 (unusable)
    int bars = 0;
    if (rssi >= -50) {
        bars = 4;
    } else if (rssi >= -60) {
        bars = 3;
    } else if (rssi >= -70) {
        bars = 2;
    } else if (rssi >= -80) {
        bars = 1;
    }
    
    // Draw 4 bars with increasing height
    int bar_width = 6;
    int bar_gap = 3;
    int max_height = 24;
    
    for (int i = 0; i < 4; i++) {
        int bar_height = (max_height / 4) * (i + 1);
        int bar_x = x + i * (bar_width + bar_gap);
        int bar_y = y + max_height - bar_height;
        
        if (i < bars) {
            // Filled bar
            gfx.fillRect(bar_x, bar_y, bar_width, bar_height, MAC_BLACK);
        } else {
            // Empty bar (outline only)
            gfx.drawRect(bar_x, bar_y, bar_width, bar_height, MAC_DARK_GRAY);
        }
    }
}

// ============================================================================
// WiFi status strip - shown next to the WiFi button on the settings screen
// so the user can see the live connection state without entering the WiFi
// configuration screen.
// ============================================================================
static void drawWifiStatusStrip(int x, int y, int w, int h)
{
    // Paint a white "note card" behind the status strip so the text stays
    // readable against the 50% gray desktop stipple. 1px black border
    // matches the classic Mac dialog accent.
    gfx.fillRect(x, y, w, h, MAC_WHITE);
    gfx.drawRect(x, y, w, h, MAC_BLACK);

    wl_status_t status = WiFi.status();
    bool connected = (status == WL_CONNECTED);

    // Inset everything by a few pixels so text isn't flush against the
    // border, which would otherwise clip against the line.
    int inner_x = x + 6;
    int inner_y = y + 4;

    // Top row: signal bars + status text.
    int bars_x = inner_x;
    int bars_y = inner_y + 2;
    int text_x = inner_x + 36;  // Reserve ~36px for the 4-bar indicator.

    if (connected) {
        int rssi = WiFi.RSSI();
        drawSignalBars(bars_x, bars_y, rssi);
    } else {
        // Empty outline bars to indicate "no signal".
        drawSignalBars(bars_x, bars_y, -200);
    }

    const char *status_label = "Not connected";
    if (connected) {
        status_label = "Connected";
    } else if (status == WL_DISCONNECTED && strlen(wifi_ssid) > 0 && wifi_auto_connect) {
        status_label = "Connecting...";
    } else if (status == WL_IDLE_STATUS && strlen(wifi_ssid) > 0 && wifi_auto_connect) {
        status_label = "Connecting...";
    } else if (status == WL_CONNECT_FAILED) {
        status_label = "Connect failed";
    } else if (status == WL_NO_SSID_AVAIL) {
        status_label = "No network";
    }
    Chicago_DrawString(status_label, text_x, inner_y + 4, MAC_BLACK,
                       TL_DATUM);

    // Second row: SSID (if we have one).
    if (strlen(wifi_ssid) > 0) {
        char ssid_display[20];
        strncpy(ssid_display, wifi_ssid, sizeof(ssid_display) - 1);
        ssid_display[sizeof(ssid_display) - 1] = '\0';
        Chicago_DrawString(ssid_display, inner_x, inner_y + 28, MAC_BLACK,
                           TL_DATUM);
    } else {
        Chicago_DrawString("(no network saved)", inner_x, inner_y + 28, MAC_DARK_GRAY,
                           TL_DATUM);
    }

    // Third row: IP address (if connected).
    if (connected) {
        char ip_display[24];
        IPAddress ip = WiFi.localIP();
        snprintf(ip_display, sizeof(ip_display), "%u.%u.%u.%u",
                 ip[0], ip[1], ip[2], ip[3]);
        Chicago_DrawString(ip_display, inner_x, inner_y + 52, MAC_BLACK,
                           TL_DATUM);
    }
}

// ============================================================================
// On-Screen Keyboard
// ============================================================================

// Keyboard layout - QWERTY with special keys
// Key codes: -1 = none, -2 = shift, -3 = backspace, -4 = enter, -5 = cancel
#define KB_KEY_NONE      -1
#define KB_KEY_SHIFT     -2
#define KB_KEY_BACKSPACE -3
#define KB_KEY_ENTER     -4
#define KB_KEY_CANCEL    -5
#define KB_KEY_SPACE     ' '

// Keyboard rows (lowercase)
static const char* KB_ROW_1 = "1234567890";
static const char* KB_ROW_2 = "qwertyuiop";
static const char* KB_ROW_3 = "asdfghjkl";
static const char* KB_ROW_4 = "zxcvbnm";

// Keyboard rows (uppercase/shifted)
static const char* KB_ROW_1_SHIFT = "!@#$%^&*()";
static const char* KB_ROW_2_SHIFT = "QWERTYUIOP";
static const char* KB_ROW_3_SHIFT = "ASDFGHJKL";
static const char* KB_ROW_4_SHIFT = "ZXCVBNM";

#define KB_ROWS 5
#define KB_KEY_HEIGHT 55
#define KB_KEY_MARGIN 4

static void drawKeyboard(int x, int y, int w, int h, bool shift_active, int highlight_key)
{
    // Calculate key dimensions
    int row_height = KB_KEY_HEIGHT;
    int keys_per_row = 10;
    int key_width = (w - KB_KEY_MARGIN * (keys_per_row + 1)) / keys_per_row;
    
    // Background
    gfx.fillRect(x, y, w, h, MAC_DARK_GRAY);
    gfx.drawRect(x, y, w, h, MAC_BLACK);
    
    // Draw each row
    int current_y = y + KB_KEY_MARGIN;
    int key_index = 0;
    
    const char* rows[] = {
        shift_active ? KB_ROW_1_SHIFT : KB_ROW_1,
        shift_active ? KB_ROW_2_SHIFT : KB_ROW_2,
        shift_active ? KB_ROW_3_SHIFT : KB_ROW_3,
        shift_active ? KB_ROW_4_SHIFT : KB_ROW_4
    };
    
    // Draw letter/number rows
    for (int row = 0; row < 4; row++) {
        const char* row_chars = rows[row];
        int row_len = strlen(row_chars);
        int row_width = row_len * (key_width + KB_KEY_MARGIN) - KB_KEY_MARGIN;
        int start_x = x + (w - row_width) / 2;
        
        for (int col = 0; col < row_len; col++) {
            int key_x = start_x + col * (key_width + KB_KEY_MARGIN);
            bool is_highlighted = (key_index == highlight_key);

            uint16_t fg;
            if (is_highlighted) {
                gfx.fillRect(key_x, current_y, key_width, row_height, MAC_BLACK);
                fg = MAC_WHITE;
            } else {
                gfx.fillRect(key_x, current_y, key_width, row_height, MAC_WHITE);
                gfx.drawRect(key_x, current_y, key_width, row_height, MAC_BLACK);
                fg = MAC_BLACK;
            }

            char label[2] = {row_chars[col], '\0'};
            Chicago_DrawString(label, key_x + key_width / 2,
                               current_y + row_height / 2,
                               fg, MC_DATUM);

            key_index++;
        }
        current_y += row_height + KB_KEY_MARGIN;
    }
    
    // Draw bottom row with special keys: Shift, Space, Backspace, Enter, Cancel
    int bottom_y = current_y;
    int special_key_w = key_width * 2;
    int space_key_w = key_width * 4;
    int total_bottom_w = special_key_w * 2 + space_key_w + key_width * 2 + KB_KEY_MARGIN * 4;
    int bottom_start_x = x + (w - total_bottom_w) / 2;
    int current_x = bottom_start_x;
    
    // Shift key
    bool shift_highlighted = (highlight_key == 100);  // Special index for shift
    uint16_t fg;
    if (shift_highlighted || shift_active) {
        gfx.fillRect(current_x, bottom_y, special_key_w, row_height, MAC_BLACK);
        fg = MAC_WHITE;
    } else {
        gfx.fillRect(current_x, bottom_y, special_key_w, row_height, MAC_WHITE);
        gfx.drawRect(current_x, bottom_y, special_key_w, row_height, MAC_BLACK);
        fg = MAC_BLACK;
    }
    Chicago_DrawString("Shift", current_x + special_key_w / 2,
                       bottom_y + row_height / 2, fg, MC_DATUM);
    current_x += special_key_w + KB_KEY_MARGIN;

    // Space key (no label).
    bool space_highlighted = (highlight_key == 101);
    if (space_highlighted) {
        gfx.fillRect(current_x, bottom_y, space_key_w, row_height, MAC_BLACK);
    } else {
        gfx.fillRect(current_x, bottom_y, space_key_w, row_height, MAC_WHITE);
        gfx.drawRect(current_x, bottom_y, space_key_w, row_height, MAC_BLACK);
    }
    current_x += space_key_w + KB_KEY_MARGIN;

    // Backspace key
    bool bksp_highlighted = (highlight_key == 102);
    if (bksp_highlighted) {
        gfx.fillRect(current_x, bottom_y, special_key_w, row_height, MAC_BLACK);
        fg = MAC_WHITE;
    } else {
        gfx.fillRect(current_x, bottom_y, special_key_w, row_height, MAC_WHITE);
        gfx.drawRect(current_x, bottom_y, special_key_w, row_height, MAC_BLACK);
        fg = MAC_BLACK;
    }
    Chicago_DrawString("<--", current_x + special_key_w / 2,
                       bottom_y + row_height / 2, fg, MC_DATUM);
    current_x += special_key_w + KB_KEY_MARGIN;

    // Enter key
    bool enter_highlighted = (highlight_key == 103);
    if (enter_highlighted) {
        gfx.fillRect(current_x, bottom_y, key_width, row_height, MAC_BLACK);
        fg = MAC_WHITE;
    } else {
        gfx.fillRect(current_x, bottom_y, key_width, row_height, MAC_LIGHT_GRAY);
        gfx.drawRect(current_x, bottom_y, key_width, row_height, MAC_BLACK);
        fg = MAC_BLACK;
    }
    Chicago_DrawString("Done", current_x + key_width / 2,
                       bottom_y + row_height / 2, fg, MC_DATUM);
    current_x += key_width + KB_KEY_MARGIN;

    // Cancel key
    bool cancel_highlighted = (highlight_key == 104);
    if (cancel_highlighted) {
        gfx.fillRect(current_x, bottom_y, key_width, row_height, MAC_BLACK);
        fg = MAC_WHITE;
    } else {
        gfx.fillRect(current_x, bottom_y, key_width, row_height, MAC_WHITE);
        gfx.drawRect(current_x, bottom_y, key_width, row_height, MAC_BLACK);
        fg = MAC_BLACK;
    }
    Chicago_DrawString("Hide", current_x + key_width / 2,
                       bottom_y + row_height / 2, fg, MC_DATUM);
}

// Get key code from touch position
// Returns character code, or special code (KB_KEY_*)
static int getKeyboardKey(int touch_x, int touch_y, int kb_x, int kb_y, int kb_w, int kb_h)
{
    // Check if touch is within keyboard bounds
    if (!isPointInRect(touch_x, touch_y, kb_x, kb_y, kb_w, kb_h)) {
        return KB_KEY_NONE;
    }
    
    int row_height = KB_KEY_HEIGHT;
    int keys_per_row = 10;
    int key_width = (kb_w - KB_KEY_MARGIN * (keys_per_row + 1)) / keys_per_row;
    
    // Determine which row was touched
    int rel_y = touch_y - kb_y - KB_KEY_MARGIN;
    int row = rel_y / (row_height + KB_KEY_MARGIN);
    
    if (row < 0 || row > 4) {
        return KB_KEY_NONE;
    }
    
    // Handle letter/number rows (0-3)
    if (row < 4) {
        const char* rows[] = {KB_ROW_1, KB_ROW_2, KB_ROW_3, KB_ROW_4};
        const char* row_chars = rows[row];
        int row_len = strlen(row_chars);
        int row_width = row_len * (key_width + KB_KEY_MARGIN) - KB_KEY_MARGIN;
        int start_x = kb_x + (kb_w - row_width) / 2;
        
        int rel_x = touch_x - start_x;
        if (rel_x < 0) {
            return KB_KEY_NONE;
        }
        
        int col = rel_x / (key_width + KB_KEY_MARGIN);
        if (col >= 0 && col < row_len) {
            return row_chars[col];
        }
        return KB_KEY_NONE;
    }
    
    // Handle bottom row with special keys
    int bottom_y = kb_y + KB_KEY_MARGIN + 4 * (row_height + KB_KEY_MARGIN);
    int special_key_w = key_width * 2;
    int space_key_w = key_width * 4;
    int total_bottom_w = special_key_w * 2 + space_key_w + key_width * 2 + KB_KEY_MARGIN * 4;
    int bottom_start_x = kb_x + (kb_w - total_bottom_w) / 2;
    int current_x = bottom_start_x;
    
    // Check Shift
    if (isPointInRect(touch_x, touch_y, current_x, bottom_y, special_key_w, row_height)) {
        return KB_KEY_SHIFT;
    }
    current_x += special_key_w + KB_KEY_MARGIN;
    
    // Check Space
    if (isPointInRect(touch_x, touch_y, current_x, bottom_y, space_key_w, row_height)) {
        return KB_KEY_SPACE;
    }
    current_x += space_key_w + KB_KEY_MARGIN;
    
    // Check Backspace
    if (isPointInRect(touch_x, touch_y, current_x, bottom_y, special_key_w, row_height)) {
        return KB_KEY_BACKSPACE;
    }
    current_x += special_key_w + KB_KEY_MARGIN;
    
    // Check Enter
    if (isPointInRect(touch_x, touch_y, current_x, bottom_y, key_width, row_height)) {
        return KB_KEY_ENTER;
    }
    current_x += key_width + KB_KEY_MARGIN;
    
    // Check Cancel
    if (isPointInRect(touch_x, touch_y, current_x, bottom_y, key_width, row_height)) {
        return KB_KEY_CANCEL;
    }
    
    return KB_KEY_NONE;
}

// Get highlight key index from touch position (for visual feedback)
static int getKeyboardHighlight(int touch_x, int touch_y, int kb_x, int kb_y, int kb_w, int kb_h)
{
    if (!isPointInRect(touch_x, touch_y, kb_x, kb_y, kb_w, kb_h)) {
        return -1;
    }
    
    int row_height = KB_KEY_HEIGHT;
    int keys_per_row = 10;
    int key_width = (kb_w - KB_KEY_MARGIN * (keys_per_row + 1)) / keys_per_row;
    
    int rel_y = touch_y - kb_y - KB_KEY_MARGIN;
    int row = rel_y / (row_height + KB_KEY_MARGIN);
    
    if (row < 0 || row > 4) {
        return -1;
    }
    
    // Letter/number rows
    if (row < 4) {
        int row_lengths[] = {10, 10, 9, 7};
        int row_len = row_lengths[row];
        int row_width = row_len * (key_width + KB_KEY_MARGIN) - KB_KEY_MARGIN;
        int start_x = kb_x + (kb_w - row_width) / 2;
        
        int rel_x = touch_x - start_x;
        if (rel_x < 0) {
            return -1;
        }
        
        int col = rel_x / (key_width + KB_KEY_MARGIN);
        if (col >= 0 && col < row_len) {
            int base_index = 0;
            for (int i = 0; i < row; i++) {
                base_index += row_lengths[i];
            }
            return base_index + col;
        }
        return -1;
    }
    
    // Bottom row special keys
    int bottom_y = kb_y + KB_KEY_MARGIN + 4 * (row_height + KB_KEY_MARGIN);
    int special_key_w = key_width * 2;
    int space_key_w = key_width * 4;
    int total_bottom_w = special_key_w * 2 + space_key_w + key_width * 2 + KB_KEY_MARGIN * 4;
    int bottom_start_x = kb_x + (kb_w - total_bottom_w) / 2;
    int current_x = bottom_start_x;
    
    if (isPointInRect(touch_x, touch_y, current_x, bottom_y, special_key_w, row_height)) {
        return 100;  // Shift
    }
    current_x += special_key_w + KB_KEY_MARGIN;
    
    if (isPointInRect(touch_x, touch_y, current_x, bottom_y, space_key_w, row_height)) {
        return 101;  // Space
    }
    current_x += space_key_w + KB_KEY_MARGIN;
    
    if (isPointInRect(touch_x, touch_y, current_x, bottom_y, special_key_w, row_height)) {
        return 102;  // Backspace
    }
    current_x += special_key_w + KB_KEY_MARGIN;
    
    if (isPointInRect(touch_x, touch_y, current_x, bottom_y, key_width, row_height)) {
        return 103;  // Enter
    }
    current_x += key_width + KB_KEY_MARGIN;
    
    if (isPointInRect(touch_x, touch_y, current_x, bottom_y, key_width, row_height)) {
        return 104;  // Cancel
    }
    
    return -1;
}

// ============================================================================
// Hit Testing
// ============================================================================

static bool isPointInRect(int px, int py, int rx, int ry, int rw, int rh)
{
    return (px >= rx && px < rx + rw && py >= ry && py < ry + rh);
}

// ============================================================================
// Settings Screen
// ============================================================================

static void runSettingsScreen(void)
{
    Serial.println("[BOOT_GUI] Showing settings screen...");
    Serial.printf("[BOOT_GUI] Found %d disk files, %d CD-ROM files, %d shared folders\n",
                  (int)disk_files.size(), (int)cdrom_files.size(),
                  (int)extfs_folders.size());
    
    // ----------------------------------------------------------------
    // Layout
    // ----------------------------------------------------------------
    // Everything lives inside one big classic-Mac window centered on the
    // desktop stipple. The Boot button is the only thing outside - it
    // floats in the desktop area below the window, giving us the
    // traditional "action bar" feel without redrawing the window frame
    // every press.

    // Boot button - docked bottom-center on the desktop.
    int boot_btn_w = 320;
    int boot_btn_h = 60;
    int boot_btn_x = (SCREEN_WIDTH - boot_btn_w) / 2;
    int boot_btn_y = SCREEN_HEIGHT - boot_btn_h - SCREEN_MARGIN;

    // Main window geometry.
    int win_x = SCREEN_MARGIN;
    int win_y = MENU_BAR_HEIGHT + 10;
    int win_w = SCREEN_WIDTH - SCREEN_MARGIN * 2;
    int win_h = boot_btn_y - win_y - 15;  // 15 px gap to the Boot halo.

    // Inner content rect of the main window.
    int inner_pad  = 15;
    int content_x  = win_x + inner_pad;
    int content_y  = win_y + windowContentTop(/*is_sub_panel=*/false) + inner_pad;
    int content_w_full = win_w - inner_pad * 2;

    // Right sidebar hosts the WiFi button, status strip, and USB Disk
    // button. The main column (lists + memory + audio) gets the rest.
    int sidebar_w = 200;
    int sidebar_x = content_x + content_w_full - sidebar_w;
    int sidebar_btn_h = 50;
    int main_w    = sidebar_x - content_x - 20;  // 20 px gutter.

    int wifi_btn_x = sidebar_x;
    int wifi_btn_y = content_y;
    int wifi_btn_w = sidebar_w;
    int wifi_btn_h = sidebar_btn_h;

    int wifi_status_x = sidebar_x;
    int wifi_status_y = wifi_btn_y + wifi_btn_h + 10;
    int wifi_status_w = sidebar_w;
    int wifi_status_h = 90;

    int usb_btn_w = sidebar_w;
    int usb_btn_h = sidebar_btn_h;
    int usb_btn_x = sidebar_x;
    int usb_btn_y = wifi_status_y + wifi_status_h + 10;
    bool usb_available = UsbMsc_IsSupported();

    // Three list columns inside the main column. We only show 5 rows
    // (not the max 6) so the Memory sub-panel + Audio checkbox fit
    // cleanly below on a 720 px panel without overflowing the window.
    int list_gap = 30;
    int list_w = (main_w - list_gap * 2) / 3;
    int list_rows = 5;
    int list_h = LIST_ITEM_HEIGHT * list_rows + 4;
    int disk_list_x  = content_x;
    int cdrom_list_x = content_x + (list_w + list_gap) * 1;
    int extfs_list_x = content_x + (list_w + list_gap) * 2;
    int list_y = content_y + 26;  // Room for the "Hard Disk:" label above.

    // Memory group-box: nested sub-panel framing the RAM radios.
    int mem_panel_x = content_x;
    int mem_panel_y = list_y + list_h + 24;
    int mem_panel_w = main_w;
    int mem_panel_h = RADIO_SIZE + windowContentTop(/*is_sub_panel=*/true) + 24;

    // Actual radio placement, inside the sub-panel interior.
    int ram_y = mem_panel_y + windowContentTop(/*is_sub_panel=*/true) + 10;
    int radio_start_x = mem_panel_x + 20;
    int radio_gap = (mem_panel_w - 40) / 4;
    int radio_region_x = radio_start_x - 5;
    int radio_region_y = ram_y - 5;
    int radio_region_w = radio_gap * 4 + 20;
    int radio_region_h = RADIO_SIZE + 30;

    // Three checkboxes share a row below the Memory group-box: Audio,
    // Boot-from-CD, and Rotate 180. Each gets ~1/3 of the main column.
    int audio_y = mem_panel_y + mem_panel_h + 20;
    int audio_x = content_x;
    int audio_checkbox_x = audio_x;
    int audio_checkbox_size = RADIO_SIZE;
    int audio_region_x = audio_x - 5;
    int audio_region_y = audio_y - 5;
    int audio_region_w = main_w / 3;
    int audio_region_h = audio_checkbox_size + 20;

    int bootcd_x = content_x + (main_w / 3);
    int bootcd_y = audio_y;
    int bootcd_checkbox_x = bootcd_x;
    int bootcd_region_w = main_w / 3;

    int rotate_x = content_x + (main_w / 3) * 2;
    int rotate_y = audio_y;
    int rotate_checkbox_x = rotate_x;
    int rotate_region_w = main_w - (main_w / 3) * 2;
    
    // Debug: Print layout info
    Serial.printf("[BOOT_GUI] Layout: list_y=%d, list_h=%d, item_height=%d\n", list_y, list_h, LIST_ITEM_HEIGHT);
    
    bool boot_pressed = false;
    bool prev_boot_pressed = false;
    bool boot_touch_started = false;
    bool wifi_pressed = false;
    bool prev_wifi_pressed = false;
    bool wifi_touch_started = false;
    bool usb_pressed = false;
    bool prev_usb_pressed = false;
    bool usb_touch_started = false;
    bool should_boot = false;
    bool open_wifi = false;
    bool open_usb = false;
    bool first_frame = true;
    
    // Track previous state for change detection
    int prev_disk_selection = disk_selection_index;
    int prev_cdrom_selection = cdrom_selection_index;
    int prev_extfs_selection = extfs_selection_index;
    int prev_ram_mb = selected_ram_mb;
    bool prev_audio_enabled = audio_enabled;
    bool prev_boot_from_cd = boot_from_cd;
    int  prev_rotation_degrees = rotation_degrees;

    // WiFi status polling state. We redraw the strip once per second, or
    // immediately when the underlying connection state changes, so an
    // async auto-connect completing during pre-boot becomes visible
    // without the user leaving the settings screen.
    uint32_t last_wifi_poll_ms = 0;
    wl_status_t prev_wifi_status = (wl_status_t)-1;
    int prev_wifi_rssi = 0;
    
    // Touch state - save position on press for use on release
    int touch_start_x = 0;
    int touch_start_y = 0;
    bool touch_in_disk_list = false;
    bool touch_in_cdrom_list = false;
    bool touch_in_extfs_list = false;
    bool touch_in_boot_btn = false;
    bool touch_in_wifi_btn = false;
    bool touch_in_usb_btn = false;
    bool touch_in_audio_checkbox = false;
    bool touch_in_bootcd_checkbox = false;
    bool touch_in_rotate_checkbox = false;

    TouchEvent touch;
    
    while (!should_boot && !open_wifi && !open_usb) {
        bool disk_changed = false;
        bool cdrom_changed = false;
        bool extfs_changed = false;
        bool ram_changed = false;
        bool audio_changed = false;
        bool bootcd_changed = false;
        bool rotate_changed = false;
        bool boot_btn_changed = false;
        bool wifi_btn_changed = false;
        bool usb_btn_changed  = false;
        
        // Get touch input from queue (non-blocking)
        if (getTouchEvent(&touch)) {
            // Detect new touch start - save position
            if (touch.was_pressed) {
                touch_start_x = touch.x;
                touch_start_y = touch.y;
                touch_in_disk_list = isPointInRect(touch_start_x, touch_start_y, disk_list_x, list_y, list_w, list_h);
                touch_in_cdrom_list = isPointInRect(touch_start_x, touch_start_y, cdrom_list_x, list_y, list_w, list_h);
                touch_in_extfs_list = isPointInRect(touch_start_x, touch_start_y, extfs_list_x, list_y, list_w, list_h);
                touch_in_boot_btn = isPointInRect(touch_start_x, touch_start_y, boot_btn_x, boot_btn_y, boot_btn_w, boot_btn_h);
                touch_in_wifi_btn = isPointInRect(touch_start_x, touch_start_y, wifi_btn_x, wifi_btn_y, wifi_btn_w, wifi_btn_h);
                touch_in_usb_btn = usb_available &&
                    isPointInRect(touch_start_x, touch_start_y, usb_btn_x, usb_btn_y, usb_btn_w, usb_btn_h);
                touch_in_audio_checkbox = isPointInRect(touch_start_x, touch_start_y, audio_x, audio_y, audio_region_w, audio_checkbox_size + 10);
                touch_in_bootcd_checkbox = isPointInRect(touch_start_x, touch_start_y, bootcd_x, bootcd_y, bootcd_region_w, audio_checkbox_size + 10);
                touch_in_rotate_checkbox = isPointInRect(touch_start_x, touch_start_y, rotate_x, rotate_y, rotate_region_w, audio_checkbox_size + 10);
                
                if (touch_in_boot_btn) {
                    boot_touch_started = true;
                    boot_pressed = true;
                }
                if (touch_in_wifi_btn) {
                    wifi_touch_started = true;
                    wifi_pressed = true;
                }
                if (touch_in_usb_btn) {
                    usb_touch_started = true;
                    usb_pressed = true;
                }
                
                Serial.printf("[BOOT_GUI] Touch start at (%d, %d) disk=%d cdrom=%d extfs=%d boot=%d wifi=%d audio=%d\n",
                              touch_start_x, touch_start_y,
                              touch_in_disk_list, touch_in_cdrom_list,
                              touch_in_extfs_list,
                              touch_in_boot_btn, touch_in_wifi_btn,
                              touch_in_audio_checkbox);
            }
            
            // Detect touch release - use saved position for hit testing
            if (touch.was_released) {
                Serial.printf("[BOOT_GUI] Touch released, start was (%d, %d)\n", touch_start_x, touch_start_y);
                
                // Check Boot button
                if (boot_touch_started) {
                    should_boot = true;
                    Serial.println("[BOOT_GUI] Boot button pressed");
                }
                
                // Check WiFi button
                if (wifi_touch_started) {
                    open_wifi = true;
                    Serial.println("[BOOT_GUI] WiFi button pressed");
                }

                // Check USB Disk button
                if (usb_touch_started) {
                    open_usb = true;
                    Serial.println("[BOOT_GUI] USB Disk button pressed");
                }
                
                // Check disk list click (use saved start position)
                if (touch_in_disk_list) {
                    int clicked_item = (touch_start_y - list_y - 2) / LIST_ITEM_HEIGHT + disk_scroll_offset;
                    if (clicked_item >= 0 && clicked_item < (int)disk_files.size()) {
                        disk_selection_index = clicked_item;
                        strncpy(selected_disk_path, disk_files[clicked_item].c_str(), BOOT_GUI_MAX_PATH - 1);
                        Serial.printf("[BOOT_GUI] Selected disk [%d]: %s\n", clicked_item, selected_disk_path);
                    }
                }
                
                // Check CD-ROM list click (use saved start position)
                if (touch_in_cdrom_list) {
                    int clicked_item = (touch_start_y - list_y - 2) / LIST_ITEM_HEIGHT + cdrom_scroll_offset;
                    int total_items = cdrom_files.size() + 1;  // +1 for "None"
                    if (clicked_item >= 0 && clicked_item < total_items) {
                        cdrom_selection_index = clicked_item;
                        if (clicked_item == 0) {
                            selected_cdrom_path[0] = '\0';
                        } else {
                            strncpy(selected_cdrom_path, cdrom_files[clicked_item - 1].c_str(), BOOT_GUI_MAX_PATH - 1);
                        }
                    }
                }

                // Check shared-folder list click (use saved start position).
                // Index 0 is always "None"; indices 1..N map into extfs_folders.
                if (touch_in_extfs_list) {
                    int clicked_item = (touch_start_y - list_y - 2) / LIST_ITEM_HEIGHT + extfs_scroll_offset;
                    int total_items = extfs_folders.size() + 1;  // +1 for "None"
                    if (clicked_item >= 0 && clicked_item < total_items) {
                        extfs_selection_index = clicked_item;
                        if (clicked_item == 0) {
                            selected_extfs_path[0] = '\0';
                            Serial.println("[BOOT_GUI] Selected shared folder: None");
                        } else {
                            strncpy(selected_extfs_path,
                                    extfs_folders[clicked_item - 1].c_str(),
                                    BOOT_GUI_MAX_PATH - 1);
                            selected_extfs_path[BOOT_GUI_MAX_PATH - 1] = '\0';
                            Serial.printf("[BOOT_GUI] Selected shared folder [%d]: %s\n",
                                          clicked_item, selected_extfs_path);
                        }
                    }
                }

                // Check RAM radio buttons
                int radio_y_hit = ram_y;
                int radio_hit_w = radio_gap - 10;
                int radio_hit_h = RADIO_SIZE + 20;
                
                if (isPointInRect(touch_start_x, touch_start_y, radio_start_x, radio_y_hit, radio_hit_w, radio_hit_h)) {
                    selected_ram_mb = 4;
                } else if (isPointInRect(touch_start_x, touch_start_y, radio_start_x + radio_gap, radio_y_hit, radio_hit_w, radio_hit_h)) {
                    selected_ram_mb = 8;
                } else if (isPointInRect(touch_start_x, touch_start_y, radio_start_x + radio_gap * 2, radio_y_hit, radio_hit_w, radio_hit_h)) {
                    selected_ram_mb = 12;
                } else if (isPointInRect(touch_start_x, touch_start_y, radio_start_x + radio_gap * 3, radio_y_hit, radio_hit_w, radio_hit_h)) {
                    selected_ram_mb = 16;
                }
                
                // Check Audio checkbox toggle
                if (touch_in_audio_checkbox) {
                    audio_enabled = !audio_enabled;
                    Serial.printf("[BOOT_GUI] Audio toggled: %s\n", audio_enabled ? "enabled" : "disabled");
                }

                // Boot-from-CD checkbox: only meaningful when a CD-ROM
                // is selected; toggling without a disc is harmless and
                // LoadPrefs will ignore it at boot.
                if (touch_in_bootcd_checkbox) {
                    boot_from_cd = !boot_from_cd;
                    Serial.printf("[BOOT_GUI] Boot from CD toggled: %s\n",
                                  boot_from_cd ? "yes" : "no");
                }

                // Rotate-180 checkbox: 0 <-> 180.
                if (touch_in_rotate_checkbox) {
                    if (rotation_degrees == 180) {
                        rotation_degrees = 0;
                    } else {
                        rotation_degrees = 180;
                    }
                    Serial.printf("[BOOT_GUI] Rotation toggled: %d\n",
                                  rotation_degrees);
                }

                // Reset touch state
                touch_in_disk_list = false;
                touch_in_cdrom_list = false;
                touch_in_extfs_list = false;
                touch_in_boot_btn = false;
                touch_in_wifi_btn = false;
                touch_in_usb_btn = false;
                touch_in_audio_checkbox = false;
                touch_in_bootcd_checkbox = false;
                touch_in_rotate_checkbox = false;
                boot_touch_started = false;
                boot_pressed = false;
                wifi_touch_started = false;
                wifi_pressed = false;
                usb_touch_started = false;
                usb_pressed = false;
            }

            // Update button visuals while held
            if (touch.is_pressed) {
                if (boot_touch_started) {
                    boot_pressed = isPointInRect(touch.x, touch.y, boot_btn_x, boot_btn_y, boot_btn_w, boot_btn_h);
                }
                if (wifi_touch_started) {
                    wifi_pressed = isPointInRect(touch.x, touch.y, wifi_btn_x, wifi_btn_y, wifi_btn_w, wifi_btn_h);
                }
                if (usb_touch_started) {
                    usb_pressed = isPointInRect(touch.x, touch.y, usb_btn_x, usb_btn_y, usb_btn_w, usb_btn_h);
                }
            }
        }
        
        // Check what changed
        disk_changed = (disk_selection_index != prev_disk_selection);
        cdrom_changed = (cdrom_selection_index != prev_cdrom_selection);
        extfs_changed = (extfs_selection_index != prev_extfs_selection);
        ram_changed = (selected_ram_mb != prev_ram_mb);
        audio_changed = (audio_enabled != prev_audio_enabled);
        bootcd_changed = (boot_from_cd != prev_boot_from_cd);
        rotate_changed = (rotation_degrees != prev_rotation_degrees);
        boot_btn_changed = (boot_pressed != prev_boot_pressed);
        wifi_btn_changed = (wifi_pressed != prev_wifi_pressed);
        usb_btn_changed  = (usb_pressed != prev_usb_pressed);
        
        if (first_frame) {
            // First frame - draw the desktop, menu bar, the main window
            // that wraps every control, then every control on top of it.
            drawDesktopPattern();
            drawMenuBar("Boot Settings");
            drawWindow(win_x, win_y, win_w, win_h, "Boot Settings");

            // Field labels sit directly on the window's white body now,
            // so plain Chicago is fine (no badge hack needed).
            Chicago_DrawString("Hard Disk:",    disk_list_x,  content_y,
                               MAC_BLACK, TL_DATUM);
            Chicago_DrawString("CD-ROM:",       cdrom_list_x, content_y,
                               MAC_BLACK, TL_DATUM);
            Chicago_DrawString("Shared Folder:", extfs_list_x, content_y,
                               MAC_BLACK, TL_DATUM);

            // Draw lists. Hard Disk has no "None" (the emulator needs a
            // boot volume), CD-ROM and Shared Folder both lead with "None"
            // so they can be disabled.
            drawListBox(disk_list_x,  list_y, list_w, list_h, disk_files,
                        disk_selection_index, disk_scroll_offset, false);
            drawListBox(cdrom_list_x, list_y, list_w, list_h, cdrom_files,
                        cdrom_selection_index, cdrom_scroll_offset, true);
            drawListBox(extfs_list_x, list_y, list_w, list_h, extfs_folders,
                        extfs_selection_index, extfs_scroll_offset, true);

            // Memory group-box (nested sub-panel) around the RAM radios.
            drawSubPanel(mem_panel_x, mem_panel_y, mem_panel_w, mem_panel_h,
                         "Memory");
            drawRadioButton(radio_start_x, ram_y, "4 MB", selected_ram_mb == 4);
            drawRadioButton(radio_start_x + radio_gap, ram_y, "8 MB", selected_ram_mb == 8);
            drawRadioButton(radio_start_x + radio_gap * 2, ram_y, "12 MB", selected_ram_mb == 12);
            drawRadioButton(radio_start_x + radio_gap * 3, ram_y, "16 MB", selected_ram_mb == 16);

            // Draw Audio / Boot-from-CD / Rotate checkboxes on one row.
            // Rotate is checked when rotation == 180 (the v4.0 default,
            // matches "USB-C port on the left" hold orientation).
            drawCheckbox(audio_checkbox_x, audio_y, audio_checkbox_size, "Audio", audio_enabled);
            drawCheckbox(bootcd_checkbox_x, bootcd_y, audio_checkbox_size, "Boot from CD", boot_from_cd);
            drawCheckbox(rotate_checkbox_x, rotate_y, audio_checkbox_size, "Rotate 180", rotation_degrees == 180);

            // Draw buttons. Boot is the default action so it gets the
            // classic double-border halo treatment.
            drawButton(wifi_btn_x, wifi_btn_y, wifi_btn_w, wifi_btn_h, "WiFi", wifi_pressed);
            drawButton(boot_btn_x, boot_btn_y, boot_btn_w, boot_btn_h, "Boot",
                       boot_pressed, /*is_default=*/true);
            if (usb_available) {
                drawButton(usb_btn_x, usb_btn_y, usb_btn_w, usb_btn_h,
                           "USB Disk", usb_pressed);
            }

            // Initial WiFi status draw (subsequent updates handled below).
            drawWifiStatusStrip(wifi_status_x, wifi_status_y, wifi_status_w, wifi_status_h);
            last_wifi_poll_ms = millis();
            prev_wifi_status  = WiFi.status();
            prev_wifi_rssi    = (prev_wifi_status == WL_CONNECTED) ? WiFi.RSSI() : 0;

            first_frame = false;
        } else {
            // Incremental updates - drawing directly to display
            if (disk_changed) {
                drawListBox(disk_list_x, list_y, list_w, list_h, disk_files, 
                            disk_selection_index, disk_scroll_offset, false);
            }
            
            if (cdrom_changed) {
                drawListBox(cdrom_list_x, list_y, list_w, list_h, cdrom_files,
                            cdrom_selection_index, cdrom_scroll_offset, true);
            }

            if (extfs_changed) {
                drawListBox(extfs_list_x, list_y, list_w, list_h, extfs_folders,
                            extfs_selection_index, extfs_scroll_offset, true);
            }

            if (ram_changed) {
                // Clear and redraw radio region. The radios now live on
                // the Memory sub-panel's white interior, so erase with
                // white rather than the desktop stipple.
                gfx.fillRect(radio_region_x, radio_region_y,
                             radio_region_w, radio_region_h, MAC_WHITE);
                drawRadioButton(radio_start_x, ram_y, "4 MB", selected_ram_mb == 4);
                drawRadioButton(radio_start_x + radio_gap, ram_y, "8 MB", selected_ram_mb == 8);
                drawRadioButton(radio_start_x + radio_gap * 2, ram_y, "12 MB", selected_ram_mb == 12);
                drawRadioButton(radio_start_x + radio_gap * 3, ram_y, "16 MB", selected_ram_mb == 16);
            }

            if (audio_changed) {
                // Audio checkbox now sits on the main window's white body.
                gfx.fillRect(audio_region_x, audio_region_y,
                             audio_region_w, audio_region_h, MAC_WHITE);
                drawCheckbox(audio_checkbox_x, audio_y, audio_checkbox_size, "Audio", audio_enabled);
            }

            if (bootcd_changed) {
                gfx.fillRect(bootcd_x - 5, audio_region_y,
                             bootcd_region_w, audio_region_h, MAC_WHITE);
                drawCheckbox(bootcd_checkbox_x, bootcd_y, audio_checkbox_size,
                             "Boot from CD", boot_from_cd);
            }

            if (rotate_changed) {
                gfx.fillRect(rotate_x - 5, audio_region_y,
                             rotate_region_w, audio_region_h, MAC_WHITE);
                drawCheckbox(rotate_checkbox_x, rotate_y, audio_checkbox_size,
                             "Rotate 180", rotation_degrees == 180);
            }
            
            if (boot_btn_changed) {
                drawButton(boot_btn_x, boot_btn_y, boot_btn_w, boot_btn_h, "Boot",
                           boot_pressed, /*is_default=*/true);
            }
            
            if (wifi_btn_changed) {
                drawButton(wifi_btn_x, wifi_btn_y, wifi_btn_w, wifi_btn_h, "WiFi", wifi_pressed);
            }

            if (usb_btn_changed && usb_available) {
                drawButton(usb_btn_x, usb_btn_y, usb_btn_w, usb_btn_h,
                           "USB Disk", usb_pressed);
            }
        }

        // Update state tracking
        prev_disk_selection = disk_selection_index;
        prev_cdrom_selection = cdrom_selection_index;
        prev_extfs_selection = extfs_selection_index;
        prev_ram_mb = selected_ram_mb;
        prev_audio_enabled = audio_enabled;
        prev_boot_from_cd = boot_from_cd;
        prev_rotation_degrees = rotation_degrees;
        prev_boot_pressed = boot_pressed;
        prev_wifi_pressed = wifi_pressed;
        prev_usb_pressed  = usb_pressed;

        // Poll WiFi status at ~1 Hz and redraw the status strip if anything
        // changed. Handles the async auto-connect landing while we're
        // sitting on this screen.
        if (!first_frame) {
            uint32_t now_ms = millis();
            if (now_ms - last_wifi_poll_ms >= 1000) {
                last_wifi_poll_ms = now_ms;
                wl_status_t cur_status = WiFi.status();
                int cur_rssi = (cur_status == WL_CONNECTED) ? WiFi.RSSI() : 0;
                // RSSI can wiggle by a few dB even when nothing has really
                // changed; only redraw if the bar count would change or the
                // status itself flipped.
                auto rssi_bars = [](int rssi) -> int {
                    if (rssi >= -50) return 4;
                    if (rssi >= -60) return 3;
                    if (rssi >= -70) return 2;
                    if (rssi >= -80) return 1;
                    return 0;
                };
                bool status_changed = (cur_status != prev_wifi_status);
                bool bars_changed   = (rssi_bars(cur_rssi) != rssi_bars(prev_wifi_rssi));
                if (status_changed || bars_changed) {
                    drawWifiStatusStrip(wifi_status_x, wifi_status_y,
                                        wifi_status_w, wifi_status_h);
                    prev_wifi_status = cur_status;
                    prev_wifi_rssi   = cur_rssi;
                }
            }
        }

        delay(1);  // Minimal delay - MIPI-DSI is fast, just yield to other tasks
    }
    
    // Handle WiFi screen navigation
    if (open_wifi) {
        runWiFiScreen();
        // After returning from WiFi screen, continue showing settings
        runSettingsScreen();
        return;
    }

    // Handle USB Disk mode
    if (open_usb) {
        runUsbMscScreen();
        // After returning, re-scan the SD so any newly-dropped disk /
        // ISO images appear in the lists, then re-enter the settings
        // screen (same pattern used for the WiFi screen).
        scanDiskFiles();
        scanCDROMFiles();
        runSettingsScreen();
        return;
    }

    // Save settings before booting
    saveSettings();
}

// ============================================================================
// WiFi Configuration Screen
// ============================================================================

// WiFi network info storage
typedef struct {
    char ssid[33];
    int32_t rssi;
    uint8_t encryption;
} WiFiNetworkInfo;

static std::vector<WiFiNetworkInfo> wifi_networks;
static int wifi_selection_index = -1;
static int wifi_scroll_offset = 0;

static void runWiFiScreen(void)
{
    Serial.println("[BOOT_GUI] Showing WiFi screen...");

    // Immediate "Opening WiFi..." feedback, painted before the slow
    // bits run (initWiFi() can take ~1-2s on cold boot, and the first
    // scan kickoff adds more). We paint a placeholder window so the
    // transition from the settings screen looks intentional.
    drawDesktopPattern();
    drawMenuBar("WiFi Settings");
    {
        int placeholder_w = 420;
        int placeholder_h = 160;
        int placeholder_x = (SCREEN_WIDTH  - placeholder_w) / 2;
        int placeholder_y = (SCREEN_HEIGHT - placeholder_h) / 2;
        drawWindow(placeholder_x, placeholder_y,
                   placeholder_w, placeholder_h,
                   "WiFi Settings");
        Chicago_DrawString("Opening WiFi...",
                           placeholder_x + placeholder_w / 2,
                           placeholder_y + placeholder_h / 2 + 10,
                           MAC_BLACK, MC_DATUM);
    }
    BoardDisplay_Present();

    // Initialize WiFi if not already done
    initWiFi();
    
    // Ensure WiFi is in a clean state for scanning.
    // After a failed auto-connect attempt, the driver may still be associated.
    // Disconnect (without erasing config) and give it time to settle.
    wl_status_t pre_status = WiFi.status();
    Serial.printf("[BOOT_GUI] WiFi pre-scan status: %d\n", pre_status);
    if (pre_status != WL_IDLE_STATUS && pre_status != WL_DISCONNECTED) {
        WiFi.disconnect(false);
        delay(100);
    }
    // Delete any leftover scan results
    WiFi.scanDelete();
    
    // Layout constants
    int content_x = SCREEN_MARGIN;
    int content_y = MENU_BAR_HEIGHT + 40;  // Room for title badge.
    int content_w = SCREEN_WIDTH - SCREEN_MARGIN * 2;
    
    // Button dimensions (defined early so other elements can size against
    // the bottom button row).
    int btn_w = 180;
    int btn_h = 50;
    int btn_gap = 20;
    
    // Scan / Connect / Back buttons along the bottom.
    int scan_btn_x = content_x;
    int scan_btn_y = SCREEN_HEIGHT - btn_h - SCREEN_MARGIN;
    int connect_btn_x = scan_btn_x + btn_w + btn_gap;
    int connect_btn_y = scan_btn_y;
    int back_btn_x = SCREEN_WIDTH - SCREEN_MARGIN - btn_w;
    int back_btn_y = scan_btn_y;
    
    // Password input area - sits just above the action buttons.
    int password_w = 640;
    int password_h = 44;
    int password_x = (SCREEN_WIDTH - password_w) / 2;
    int password_y = scan_btn_y - password_h - 20;
    
    // Status line - one-line badge right under the network list.
    int status_y = password_y - 34;
    
    // Network list - fills the space between the title and the status
    // line; the list height is adaptive so it doesn't collide with the
    // password field or buttons.
    int list_x = content_x;
    int list_y = content_y + 30;  // Room for the "Networks:" label.
    int list_w = content_w;
    int list_max = status_y - list_y - 10;
    int list_rows = list_max / LIST_ITEM_HEIGHT;
    if (list_rows < 3) list_rows = 3;
    if (list_rows > 6) list_rows = 6;
    int list_h = list_rows * LIST_ITEM_HEIGHT + 4;
    int visible_count_actual = list_rows;
    
    // Keyboard dimensions. Key height shrinks a bit so the keyboard +
    // input bar fit without eating into the network list's row area.
    const int KB_ROWS_DRAWN = 5;
    int kb_key_h = KB_KEY_HEIGHT;
    int kb_h = kb_key_h * KB_ROWS_DRAWN + KB_KEY_MARGIN * (KB_ROWS_DRAWN + 1);
    int kb_y = SCREEN_HEIGHT - kb_h - 10;
    int kb_x = 50;
    int kb_w = SCREEN_WIDTH - 100;

    // Input bar above the keyboard: contains the password preview and the
    // big "Done" / "Cancel" buttons so there's always an unmistakable way
    // to dismiss the on-screen keyboard. Sized to match the existing
    // kb_y-60 erase region.
    int ib_y = kb_y - 60;
    int ib_h = 60;
    int ib_btn_w  = 150;
    int ib_btn_h  = 44;
    int ib_btn_y  = ib_y + (ib_h - ib_btn_h) / 2;
    int ib_done_x   = SCREEN_WIDTH - SCREEN_MARGIN - ib_btn_w;
    int ib_cancel_x = ib_done_x - ib_btn_w - 10;
    int ib_label_x  = SCREEN_MARGIN + 20;
    int ib_preview_x = ib_label_x + 130;
    int ib_preview_w = ib_cancel_x - ib_preview_x - 20;
    int ib_preview_h = ib_btn_h;
    int ib_preview_y = ib_btn_y;
    
    // State
    bool scanning = false;
    bool connecting = false;
    bool show_keyboard = false;
    bool shift_active = false;
    char password_buffer[64] = "";
    int password_cursor = 0;
    int kb_highlight = -1;
    
    bool scan_pressed = false;
    bool scan_touch_started = false;
    bool connect_pressed = false;
    bool connect_touch_started = false;
    bool back_pressed = false;
    bool back_touch_started = false;
    bool password_touched = false;

    // Input-bar (keyboard header) button state.
    bool ib_done_pressed       = false;
    bool ib_done_touch_started = false;
    bool ib_cancel_pressed       = false;
    bool ib_cancel_touch_started = false;
    
    bool should_exit = false;
    
    // Touch state
    int touch_start_x = 0;
    int touch_start_y = 0;
    
    TouchEvent touch;
    
    // Copy saved password if we have a saved SSID
    if (strlen(wifi_ssid) > 0 && strlen(wifi_password) > 0) {
        strncpy(password_buffer, wifi_password, sizeof(password_buffer) - 1);
        password_cursor = strlen(password_buffer);
    }
    
    // Initial scan
    Serial.println("[BOOT_GUI] Starting initial WiFi scan...");
    scanning = true;
    int scan_result = WiFi.scanNetworks(true);  // Async scan
    Serial.printf("[BOOT_GUI] Scan initiated, result: %d\n", scan_result);
    
    // State tracking for incremental updates
    bool first_frame = true;
    bool prev_scanning = false;
    bool prev_connecting = false;
    bool prev_show_keyboard = false;
    int prev_wifi_selection = -1;
    int prev_password_len = 0;
    wl_status_t prev_wifi_status = WL_IDLE_STATUS;
    bool prev_scan_pressed = false;
    bool prev_connect_pressed = false;
    bool prev_back_pressed = false;
    int prev_kb_highlight = -1;
    bool prev_shift_active = false;
    size_t prev_network_count = 0;
    bool prev_ib_done_pressed   = false;
    bool prev_ib_cancel_pressed = false;
    
    while (!should_exit) {
        // Check scan completion
        if (scanning) {
            int16_t result = WiFi.scanComplete();
            if (result >= 0) {
                // Scan complete
                Serial.printf("[BOOT_GUI] Scan complete, found %d networks\n", result);
                wifi_networks.clear();
                for (int i = 0; i < result; i++) {
                    WiFiNetworkInfo info;
                    strncpy(info.ssid, WiFi.SSID(i).c_str(), sizeof(info.ssid) - 1);
                    info.ssid[sizeof(info.ssid) - 1] = '\0';
                    info.rssi = WiFi.RSSI(i);
                    info.encryption = WiFi.encryptionType(i);
                    wifi_networks.push_back(info);
                    Serial.printf("[BOOT_GUI]   %s (RSSI: %d)\n", info.ssid, info.rssi);
                }
                WiFi.scanDelete();
                scanning = false;
                
                // Select saved network if found
                if (strlen(wifi_ssid) > 0) {
                    for (size_t i = 0; i < wifi_networks.size(); i++) {
                        if (strcmp(wifi_networks[i].ssid, wifi_ssid) == 0) {
                            wifi_selection_index = i;
                            break;
                        }
                    }
                }
            } else if (result == WIFI_SCAN_FAILED) {
                Serial.println("[BOOT_GUI] Scan failed");
                scanning = false;
            }
            // result == WIFI_SCAN_RUNNING means still scanning
        }
        
        // Check connection status
        if (connecting) {
            wl_status_t status = WiFi.status();
            if (status == WL_CONNECTED) {
                Serial.println("[BOOT_GUI] WiFi connected!");
                Serial.printf("[BOOT_GUI] IP: %s\n", WiFi.localIP().toString().c_str());
                connecting = false;
                
                // Save credentials on successful connection
                if (wifi_selection_index >= 0 && wifi_selection_index < (int)wifi_networks.size()) {
                    strncpy(wifi_ssid, wifi_networks[wifi_selection_index].ssid, sizeof(wifi_ssid) - 1);
                    // For open networks, save empty password
                    bool is_open = (wifi_networks[wifi_selection_index].encryption == WIFI_AUTH_OPEN);
                    if (is_open) {
                        wifi_password[0] = '\0';
                    } else {
                        strncpy(wifi_password, password_buffer, sizeof(wifi_password) - 1);
                    }
                    wifi_auto_connect = true;
                    saveSettings();
                }
            } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
                Serial.println("[BOOT_GUI] Connection failed");
                connecting = false;
            }
        }
        
        // Get touch input
        if (getTouchEvent(&touch)) {
            // Handle keyboard input when visible
            if (show_keyboard) {
                // The input bar above the keyboard owns the "Done" and
                // "Cancel" buttons. We check those first so a tap on the
                // header never leaks into the keyboard grid below.
                bool in_done = isPointInRect(touch.x, touch.y,
                                             ib_done_x, ib_btn_y,
                                             ib_btn_w, ib_btn_h);
                bool in_cancel = isPointInRect(touch.x, touch.y,
                                               ib_cancel_x, ib_btn_y,
                                               ib_btn_w, ib_btn_h);

                if (touch.was_pressed) {
                    if (in_done) {
                        ib_done_touch_started = true;
                        ib_done_pressed       = true;
                    } else if (in_cancel) {
                        ib_cancel_touch_started = true;
                        ib_cancel_pressed       = true;
                    } else {
                        kb_highlight = getKeyboardHighlight(touch.x, touch.y, kb_x, kb_y, kb_w, kb_h);
                    }
                }

                if (touch.is_pressed) {
                    if (ib_done_touch_started) {
                        ib_done_pressed = in_done;
                    } else if (ib_cancel_touch_started) {
                        ib_cancel_pressed = in_cancel;
                    } else {
                        kb_highlight = getKeyboardHighlight(touch.x, touch.y, kb_x, kb_y, kb_w, kb_h);
                    }
                }

                if (touch.was_released) {
                    kb_highlight = -1;

                    // Header-bar buttons take precedence.
                    if (ib_done_touch_started) {
                        if (in_done) {
                            show_keyboard = false;
                        }
                        ib_done_touch_started = false;
                        ib_done_pressed       = false;
                        continue;
                    }
                    if (ib_cancel_touch_started) {
                        if (in_cancel) {
                            show_keyboard = false;
                        }
                        ib_cancel_touch_started = false;
                        ib_cancel_pressed       = false;
                        continue;
                    }

                    int key = getKeyboardKey(touch.x, touch.y, kb_x, kb_y, kb_w, kb_h);

                    if (key == KB_KEY_CANCEL) {
                        show_keyboard = false;
                    } else if (key == KB_KEY_ENTER) {
                        show_keyboard = false;
                    } else if (key == KB_KEY_SHIFT) {
                        shift_active = !shift_active;
                    } else if (key == KB_KEY_BACKSPACE) {
                        if (password_cursor > 0) {
                            password_cursor--;
                            password_buffer[password_cursor] = '\0';
                        }
                    } else if (key == KB_KEY_SPACE) {
                        if (password_cursor < (int)sizeof(password_buffer) - 1) {
                            password_buffer[password_cursor++] = ' ';
                            password_buffer[password_cursor] = '\0';
                        }
                    } else if (key > 0 && key < 128) {
                        // Regular character
                        char c = (char)key;
                        if (shift_active) {
                            // Convert to uppercase or shifted symbol
                            if (c >= 'a' && c <= 'z') {
                                c = c - 'a' + 'A';
                            } else {
                                // Handle number row shift symbols
                                const char* num_row = "1234567890";
                                const char* sym_row = "!@#$%^&*()";
                                for (int i = 0; i < 10; i++) {
                                    if (c == num_row[i]) {
                                        c = sym_row[i];
                                        break;
                                    }
                                }
                            }
                        }
                        if (password_cursor < (int)sizeof(password_buffer) - 1) {
                            password_buffer[password_cursor++] = c;
                            password_buffer[password_cursor] = '\0';
                        }
                    }
                }
            } else {
                // Regular screen interaction
                if (touch.was_pressed) {
                    touch_start_x = touch.x;
                    touch_start_y = touch.y;
                    
                    // Check buttons
                    if (isPointInRect(touch_start_x, touch_start_y, scan_btn_x, scan_btn_y, btn_w, btn_h)) {
                        scan_touch_started = true;
                        scan_pressed = true;
                    }
                    if (isPointInRect(touch_start_x, touch_start_y, connect_btn_x, connect_btn_y, btn_w, btn_h)) {
                        connect_touch_started = true;
                        connect_pressed = true;
                    }
                    if (isPointInRect(touch_start_x, touch_start_y, back_btn_x, back_btn_y, btn_w, btn_h)) {
                        back_touch_started = true;
                        back_pressed = true;
                    }
                    if (isPointInRect(touch_start_x, touch_start_y, password_x, password_y, password_w, password_h)) {
                        password_touched = true;
                    }
                }
                
                if (touch.was_released) {
                    // Handle button releases
                    if (scan_touch_started && !scanning) {
                        Serial.println("[BOOT_GUI] Starting WiFi scan...");
                        scanning = true;
                        WiFi.scanNetworks(true);
                    }
                    
                    if (connect_touch_started && wifi_selection_index >= 0 && !connecting) {
                        const char* selected_ssid = wifi_networks[wifi_selection_index].ssid;
                        bool is_open = (wifi_networks[wifi_selection_index].encryption == WIFI_AUTH_OPEN);
                        Serial.printf("[BOOT_GUI] Connecting to %s (%s)...\n", 
                                      selected_ssid, is_open ? "open" : "encrypted");
                        connecting = true;
                        if (is_open || strlen(password_buffer) == 0) {
                            WiFi.begin(selected_ssid);
                        } else {
                            WiFi.begin(selected_ssid, password_buffer);
                        }
                    }
                    
                    if (back_touch_started) {
                        should_exit = true;
                    }
                    
                    if (password_touched) {
                        show_keyboard = true;
                    }
                    
                    // Check network list selection
                    if (isPointInRect(touch_start_x, touch_start_y, list_x, list_y, list_w, list_h)) {
                        int clicked_item = (touch_start_y - list_y - 2) / LIST_ITEM_HEIGHT + wifi_scroll_offset;
                        if (clicked_item >= 0 && clicked_item < (int)wifi_networks.size()) {
                            wifi_selection_index = clicked_item;
                            Serial.printf("[BOOT_GUI] Selected network: %s\n", wifi_networks[clicked_item].ssid);
                        }
                    }
                    
                    // Reset touch state
                    scan_touch_started = false;
                    scan_pressed = false;
                    connect_touch_started = false;
                    connect_pressed = false;
                    back_touch_started = false;
                    back_pressed = false;
                    password_touched = false;
                }
                
                // Update button visuals while held
                if (touch.is_pressed) {
                    if (scan_touch_started) {
                        scan_pressed = isPointInRect(touch.x, touch.y, scan_btn_x, scan_btn_y, btn_w, btn_h);
                    }
                    if (connect_touch_started) {
                        connect_pressed = isPointInRect(touch.x, touch.y, connect_btn_x, connect_btn_y, btn_w, btn_h);
                    }
                    if (back_touch_started) {
                        back_pressed = isPointInRect(touch.x, touch.y, back_btn_x, back_btn_y, btn_w, btn_h);
                    }
                }
            }
        }
        
        // Detect state changes
        wl_status_t wifi_status = WiFi.status();
        bool scanning_changed = (scanning != prev_scanning);
        bool connecting_changed = (connecting != prev_connecting);
        bool keyboard_changed = (show_keyboard != prev_show_keyboard);
        bool selection_changed = (wifi_selection_index != prev_wifi_selection);
        bool password_changed = ((int)strlen(password_buffer) != prev_password_len);
        bool status_changed = (wifi_status != prev_wifi_status);
        bool scan_btn_changed = (scan_pressed != prev_scan_pressed);
        bool connect_btn_changed = (connect_pressed != prev_connect_pressed);
        bool back_btn_changed = (back_pressed != prev_back_pressed);
        bool kb_highlight_changed = (kb_highlight != prev_kb_highlight);
        bool shift_changed = (shift_active != prev_shift_active);
        bool network_list_changed = (wifi_networks.size() != prev_network_count) || selection_changed || scanning_changed;
        
        // Save first_frame state before clearing it
        bool needs_full_draw = first_frame;
        if (first_frame) {
            // First frame - draw the themed desktop + menu bar + the
            // single settings window that frames everything.
            drawDesktopPattern();
            drawMenuBar("WiFi Settings");

            // Main window spans from just under the menu bar to just
            // above the bottom button row.
            int wifi_win_x = SCREEN_MARGIN - 8;
            int wifi_win_y = MENU_BAR_HEIGHT + 10;
            int wifi_win_w = SCREEN_WIDTH - (SCREEN_MARGIN - 8) * 2;
            int wifi_win_h = (scan_btn_y - 15) - wifi_win_y;
            drawWindow(wifi_win_x, wifi_win_y, wifi_win_w, wifi_win_h,
                       "WiFi Settings");

            // "Networks:" label sits directly on the window's white body.
            Chicago_DrawString("Networks:", list_x, content_y,
                               MAC_BLACK, TL_DATUM);
            first_frame = false;
        }

        // Draw scanning indicator (only when state changes or first frame)
        if (scanning_changed || needs_full_draw) {
            // Clear the scanning area with white (window body).
            gfx.fillRect(SCREEN_WIDTH - SCREEN_MARGIN - 160, content_y - 4,
                         160, 30, MAC_WHITE);
            if (scanning) {
                Chicago_DrawString("Scanning...",
                                   SCREEN_WIDTH - SCREEN_MARGIN, content_y,
                                   MAC_BLACK, TR_DATUM);
            }
        }
        
        // Draw network list (only when needed)
        if (network_list_changed || needs_full_draw) {
            gfx.fillRect(list_x, list_y, list_w, list_h, MAC_WHITE);
            gfx.drawRect(list_x, list_y, list_w, list_h, MAC_BLACK);
            gfx.drawRect(list_x + 1, list_y + 1, list_w - 2, list_h - 2, MAC_BLACK);

            int visible_count = visible_count_actual;

            for (int i = 0; i < visible_count && (i + wifi_scroll_offset) < (int)wifi_networks.size(); i++) {
                int item_index = i + wifi_scroll_offset;
                int item_y = list_y + 3 + i * LIST_ITEM_HEIGHT;
                
                WiFiNetworkInfo& net = wifi_networks[item_index];

                uint16_t fg = MAC_BLACK;
                if (item_index == wifi_selection_index) {
                    gfx.fillRect(list_x + 3, item_y, list_w - 6, LIST_ITEM_HEIGHT, MAC_BLACK);
                    fg = MAC_WHITE;
                }

                // Draw SSID
                char ssid_display[32];
                strncpy(ssid_display, net.ssid, 24);
                ssid_display[24] = '\0';
                if (strlen(net.ssid) > 24) {
                    strcat(ssid_display, "...");
                }
                Chicago_DrawString(ssid_display, list_x + 10,
                                   item_y + LIST_ITEM_HEIGHT / 2,
                                   fg, ML_DATUM);
                
                // Draw signal bars
                int bars_x = list_x + list_w - 60;
                int bars_y = item_y + (LIST_ITEM_HEIGHT - 24) / 2;
                
                if (item_index == wifi_selection_index) {
                    // Invert signal bar colors for selected item
                    int bars = 0;
                    if (net.rssi >= -50) {
                        bars = 4;
                    } else if (net.rssi >= -60) {
                        bars = 3;
                    } else if (net.rssi >= -70) {
                        bars = 2;
                    } else if (net.rssi >= -80) {
                        bars = 1;
                    }
                    
                    int bar_width = 6;
                    int bar_gap = 3;
                    int max_height = 24;
                    
                    for (int b = 0; b < 4; b++) {
                        int bar_height = (max_height / 4) * (b + 1);
                        int bar_x = bars_x + b * (bar_width + bar_gap);
                        int bar_y_pos = bars_y + max_height - bar_height;
                        
                        if (b < bars) {
                            gfx.fillRect(bar_x, bar_y_pos, bar_width, bar_height, MAC_WHITE);
                        } else {
                            gfx.drawRect(bar_x, bar_y_pos, bar_width, bar_height, MAC_DARK_GRAY);
                        }
                    }
                } else {
                    drawSignalBars(bars_x, bars_y, net.rssi);
                }
                
                // Draw lock icon if encrypted
                if (net.encryption != WIFI_AUTH_OPEN) {
                    uint16_t lock_fg = (item_index == wifi_selection_index)
                                         ? MAC_WHITE : MAC_BLACK;
                    Chicago_DrawString("*",
                                       list_x + list_w - 90,
                                       item_y + LIST_ITEM_HEIGHT / 2,
                                       lock_fg, ML_DATUM);
                }
            }
        }
        
        // Draw status area (only when status changes or first frame)
        if (status_changed || connecting_changed || needs_full_draw) {
            // Clear status area with white (window body).
            gfx.fillRect(content_x - 4, status_y - 4,
                         content_w + 8, 34, MAC_WHITE);

            const char* status_text = "Not connected";
            if (wifi_status == WL_CONNECTED) {
                status_text = "Connected";
            } else if (connecting) {
                status_text = "Connecting...";
            } else if (wifi_status == WL_CONNECT_FAILED) {
                status_text = "Connection failed";
            } else if (wifi_status == WL_NO_SSID_AVAIL) {
                status_text = "Network not found";
            }

            char status_line[128];
            sprintf(status_line, "Status: %s", status_text);
            Chicago_DrawString(status_line, content_x, status_y,
                               MAC_BLACK, TL_DATUM);

            // Show IP if connected
            if (wifi_status == WL_CONNECTED) {
                sprintf(status_line, "IP: %s", WiFi.localIP().toString().c_str());
                Chicago_DrawString(status_line, content_x + 300, status_y,
                                   MAC_BLACK, TL_DATUM);
            }
        }
        
        // Helper lambdas to keep the redraw logic tidy. `drawPasswordField`
        // paints the boxed entry on the settings page with an asterisked
        // preview of what's been typed. `drawInputBar` paints the keyboard
        // header strip (password preview + big Done / Cancel buttons).
        auto drawPasswordField = [&]() {
            gfx.fillRect(password_x, password_y, password_w, password_h, MAC_WHITE);
            gfx.drawRect(password_x, password_y, password_w, password_h, MAC_BLACK);
            gfx.drawRect(password_x + 1, password_y + 1,
                         password_w - 2, password_h - 2, MAC_BLACK);

            int pw_len = strlen(password_buffer);
            if (pw_len > 0) {
                char password_display[65];
                for (int i = 0; i < pw_len && i < 64; i++) {
                    password_display[i] = '*';
                }
                password_display[pw_len] = '\0';
                Chicago_DrawString(password_display,
                                   password_x + 10, password_y + password_h / 2,
                                   MAC_BLACK, ML_DATUM);
            } else {
                Chicago_DrawString("Tap to enter password",
                                   password_x + 10, password_y + password_h / 2,
                                   MAC_DARK_GRAY, ML_DATUM);
            }
        };

        auto drawInputBar = [&]() {
            // Gray header strip matching the keyboard frame.
            gfx.fillRect(0, ib_y, SCREEN_WIDTH, ib_h, MAC_LIGHT_GRAY);
            gfx.drawFastHLine(0, ib_y, SCREEN_WIDTH, MAC_BLACK);

            // "Password:" label on the left.
            Chicago_DrawString("Password:", ib_label_x, ib_y + ib_h / 2,
                               MAC_BLACK, ML_DATUM);

            // White-backed preview of what's been typed so the user can
            // verify characters without the password field being visible.
            gfx.fillRect(ib_preview_x, ib_preview_y,
                         ib_preview_w, ib_preview_h, MAC_WHITE);
            gfx.drawRect(ib_preview_x, ib_preview_y,
                         ib_preview_w, ib_preview_h, MAC_BLACK);
            gfx.drawRect(ib_preview_x + 1, ib_preview_y + 1,
                         ib_preview_w - 2, ib_preview_h - 2, MAC_BLACK);

            int pw_len = strlen(password_buffer);
            if (pw_len > 0) {
                Chicago_DrawString(password_buffer,
                                   ib_preview_x + 8,
                                   ib_preview_y + ib_preview_h / 2,
                                   MAC_BLACK, ML_DATUM);
            } else {
                Chicago_DrawString("(type your password)",
                                   ib_preview_x + 8,
                                   ib_preview_y + ib_preview_h / 2,
                                   MAC_DARK_GRAY, ML_DATUM);
            }

            // Big, unmistakable dismiss buttons. Done is the default action
            // so it gets the classic double-border halo.
            drawButton(ib_cancel_x, ib_btn_y, ib_btn_w, ib_btn_h,
                       "Cancel", ib_cancel_pressed);
            drawButton(ib_done_x, ib_btn_y, ib_btn_w, ib_btn_h,
                       "Done", ib_done_pressed, /*is_default=*/true);
        };

        // Draw password field (only when password changes, keyboard visibility changes, or first frame)
        if (password_changed || keyboard_changed || needs_full_draw) {
            // Clear and redraw password area. Password field lives on the
            // WiFi window's white body.
            gfx.fillRect(content_x - 4, password_y - 4,
                         content_w + 8, password_h + 8, MAC_WHITE);

            if (!show_keyboard) {
                // The field shows an asterisked preview and a "Tap to
                // enter password" placeholder, so no separate label is
                // needed on the left.
                drawPasswordField();
            }
            // When the keyboard is visible, the password preview lives on
            // the input bar instead; leaving the field hidden avoids
            // duplicating the same text in two places.
        }

        // Draw keyboard or buttons depending on mode
        if (keyboard_changed || needs_full_draw) {
            if (show_keyboard) {
                // Hide the WiFi window / status / network list while the
                // keyboard is up. We paint the desktop stipple over the
                // entire area between the window-top and the input bar
                // so no window content peeks through.
                fillWithDesktopStipple(0, MENU_BAR_HEIGHT, SCREEN_WIDTH,
                                       ib_y - MENU_BAR_HEIGHT);

                // Draw input bar + keyboard overlay. The input bar owns
                // the Done/Cancel buttons that always dismiss the
                // keyboard; the keys below still accept Enter/Cancel as
                // secondary shortcuts.
                drawInputBar();
                drawKeyboard(kb_x, kb_y, kb_w, kb_h, shift_active, kb_highlight);
            } else {
                // Keyboard just dismissed. Restore the stipple for the
                // desktop gutter (below the window) and repaint the
                // WiFi window + all controls from scratch.
                fillWithDesktopStipple(0, MENU_BAR_HEIGHT, SCREEN_WIDTH,
                                       SCREEN_HEIGHT - MENU_BAR_HEIGHT);

                int wifi_win_x = SCREEN_MARGIN - 8;
                int wifi_win_y = MENU_BAR_HEIGHT + 10;
                int wifi_win_w = SCREEN_WIDTH - (SCREEN_MARGIN - 8) * 2;
                int wifi_win_h = (scan_btn_y - 15) - wifi_win_y;
                drawWindow(wifi_win_x, wifi_win_y, wifi_win_w, wifi_win_h,
                           "WiFi Settings");

                Chicago_DrawString("Networks:", list_x, content_y,
                                   MAC_BLACK, TL_DATUM);

                // Force a full list + status repaint on the next iteration
                // so the network list comes back into view.
                prev_network_count = (size_t)-1;
                prev_wifi_selection = INT_MIN;
                prev_wifi_status = (wl_status_t)-1;

                drawPasswordField();

                drawButton(scan_btn_x, scan_btn_y, btn_w, btn_h, "Scan", scan_pressed);
                drawButton(connect_btn_x, connect_btn_y, btn_w, btn_h, "Connect",
                           connect_pressed, /*is_default=*/true);
                drawButton(back_btn_x, back_btn_y, btn_w, btn_h, "Back", back_pressed);
            }
        } else if (show_keyboard) {
            // Keyboard visible - update only if something changed.
            bool ib_btn_changed = (ib_done_pressed != prev_ib_done_pressed) ||
                                   (ib_cancel_pressed != prev_ib_cancel_pressed);
            if (password_changed || ib_btn_changed) {
                drawInputBar();
            }
            if (kb_highlight_changed || shift_changed) {
                drawKeyboard(kb_x, kb_y, kb_w, kb_h, shift_active, kb_highlight);
            }
        } else {
            // Buttons visible - update only if pressed state changed
            if (scan_btn_changed) {
                drawButton(scan_btn_x, scan_btn_y, btn_w, btn_h, "Scan", scan_pressed);
            }
            if (connect_btn_changed) {
                drawButton(connect_btn_x, connect_btn_y, btn_w, btn_h, "Connect",
                           connect_pressed, /*is_default=*/true);
            }
            if (back_btn_changed) {
                drawButton(back_btn_x, back_btn_y, btn_w, btn_h, "Back", back_pressed);
            }
        }
        
        // Update previous state for next iteration
        prev_scanning = scanning;
        prev_connecting = connecting;
        prev_show_keyboard = show_keyboard;
        prev_wifi_selection = wifi_selection_index;
        prev_password_len = strlen(password_buffer);
        prev_wifi_status = wifi_status;
        prev_scan_pressed = scan_pressed;
        prev_connect_pressed = connect_pressed;
        prev_back_pressed = back_pressed;
        prev_kb_highlight = kb_highlight;
        prev_shift_active = shift_active;
        prev_network_count = wifi_networks.size();
        prev_ib_done_pressed   = ib_done_pressed;
        prev_ib_cancel_pressed = ib_cancel_pressed;
        
        delay(1);  // Minimal delay - just yield to other tasks
    }
    
    Serial.println("[BOOT_GUI] Exiting WiFi screen");
}

// ============================================================================
// USB Disk Mode Screen
// ============================================================================
//
// Paints a full-screen classic-Mac "dialog" explaining that the SD card is
// now available over USB, then runs the MSC stack on a dedicated FreeRTOS
// task so our touch loop stays responsive. "Done" tears the stack down and
// returns to the settings screen, which will re-scan the SD to pick up any
// newly-copied disk/ISO images.

// Shared context between runUsbMscScreen and its background task. Kept in
// module scope because the task runs while the touch loop is blocked on
// touch polling, and passing a pointer to a stack-local struct would be
// a lifetime hazard if the UI task ever spawns a retry.
typedef struct {
    const char       *err;
    volatile bool     finished;
} UsbMscTaskCtx;

static void usbMscTask(void *arg)
{
    UsbMscTaskCtx *ctx = static_cast<UsbMscTaskCtx *>(arg);
    UsbMsc_Enter(&ctx->err);
    // When UsbMsc_Enter returns, SD is remounted and this task is done.
    ctx->finished = true;
    vTaskDelete(NULL);
}

static void runUsbMscScreen(void)
{
    Serial.println("[BOOT_GUI] Entering USB Disk screen...");

    if (!UsbMsc_IsSupported()) {
        // Shouldn't happen - button is hidden - but fail gracefully.
        Serial.println("[BOOT_GUI] USB MSC not supported in this build");
        return;
    }

    // Spin up MSC in a background task so the touch loop keeps polling.
    UsbMscTaskCtx ctx = { nullptr, false };
    TaskHandle_t msc_task = nullptr;
    BaseType_t rc = xTaskCreatePinnedToCore(
        usbMscTask, "usb_msc", 6144,
        &ctx, 2, &msc_task, 0);
    if (rc != pdPASS) {
        Serial.println("[BOOT_GUI] Failed to create USB MSC task");
        return;
    }

    // Layout: tiled desktop background with a centered "alert" panel.
    drawDesktopPattern();
    drawMenuBar("USB Disk Mode");

    int line_h = Chicago_LineHeight() + 4;

    // Vertical layout constants. The panel auto-sizes to its content so
    // boards with extra instructional copy (Tab5's USB-A cable note) grow
    // the window instead of tucking text under the Done button.
    const int top_pad    = 30;
    const int bottom_pad = 20;
    const int text_gap   = line_h;  // breathing room between text and status
    const int status_row = 30;
    const int btn_gap    = 20;
    const int btn_w      = 160;
    const int btn_h      = 60;

    // Text block: each paragraph is N lines; paragraphs are separated by
    // a line_h gap. Intro + action are universal; Tab5 adds a cable hint.
    int intro_lines  = 2;  // "Your SD card is now available..." / "...USB drive."
    int action_lines = 2;  // "Copy or remove disk images..." / "...boot menu."
    int note_lines   = 0;
#if defined(BOARD_M5STACK_TAB5)
    note_lines = 2;        // "Use a USB-A to USB-C cable..." / "...not USB-C."
#endif
    int n_paragraphs = 2 + (note_lines > 0 ? 1 : 0);
    int total_lines  = intro_lines + action_lines + note_lines;
    int text_block_h = total_lines * line_h + (n_paragraphs - 1) * line_h;

    int panel_w = SCREEN_WIDTH * 3 / 5;
    int panel_h = TITLE_BAR_HEIGHT + top_pad + text_block_h
                + text_gap + status_row + btn_gap + btn_h + bottom_pad;
    int panel_x = (SCREEN_WIDTH - panel_w) / 2;
    int panel_y = (SCREEN_HEIGHT - panel_h) / 2;
    drawWindow(panel_x, panel_y, panel_w, panel_h, "USB Disk");

    int text_x = panel_x + 30;
    int text_y = panel_y + TITLE_BAR_HEIGHT + top_pad;

    Chicago_DrawString("Your SD card is now available on your",
                       text_x, text_y, MAC_BLACK, TL_DATUM);
    text_y += line_h;
    Chicago_DrawString("computer as a USB drive.",
                       text_x, text_y, MAC_BLACK, TL_DATUM);
    text_y += line_h * 2;

    Chicago_DrawString("Copy or remove disk images, then tap",
                       text_x, text_y, MAC_BLACK, TL_DATUM);
    text_y += line_h;
    Chicago_DrawString("\"Done\" to return to the boot menu.",
                       text_x, text_y, MAC_BLACK, TL_DATUM);

#if defined(BOARD_M5STACK_TAB5)
    text_y += line_h * 2;
    Chicago_DrawString("Use a USB-A to USB-C cable, and plug the",
                       text_x, text_y, MAC_DARK_GRAY, TL_DATUM);
    text_y += line_h;
    Chicago_DrawString("USB-A end into the USB-A port (not USB-C).",
                       text_x, text_y, MAC_DARK_GRAY, TL_DATUM);
#endif

    // "Done" button at panel bottom, status row just above it.
    int btn_x = panel_x + (panel_w - btn_w) / 2;
    int btn_y = panel_y + panel_h - btn_h - bottom_pad;

    // Status area: host mount state, error if any.
    int status_y = btn_y - btn_gap - (status_row / 2);
    auto redrawStatus = [&](bool host_mounted, const char *error) {
        // Status sits inside the window's white body, so just wipe
        // with white before re-rendering the line.
        gfx.fillRect(panel_x + 4, status_y - 8,
                     panel_w - 8, 24, MAC_WHITE);
        if (error) {
            Chicago_DrawString(error, panel_x + panel_w / 2, status_y,
                               MAC_BLACK, MC_DATUM);
        } else if (host_mounted) {
            Chicago_DrawString("Host connected - safe to copy files",
                               panel_x + panel_w / 2, status_y,
                               MAC_BLACK, MC_DATUM);
        } else {
            Chicago_DrawString("Waiting for host computer...",
                               panel_x + panel_w / 2, status_y,
                               MAC_DARK_GRAY, MC_DATUM);
        }
    };

    bool done_pressed = false;
    bool prev_done_pressed = false;
    bool done_touch_started = false;
    bool prev_host_mounted = !UsbMsc_HostMounted();  // force initial paint
    const char *prev_err = nullptr;
    bool        status_drawn = false;

    drawButton(btn_x, btn_y, btn_w, btn_h, "Done", done_pressed,
               /*is_default=*/true);

    bool should_exit = false;
    TouchEvent touch;

    while (!should_exit) {
        // Touch handling.
        if (getTouchEvent(&touch)) {
            if (touch.was_pressed) {
                if (isPointInRect(touch.x, touch.y, btn_x, btn_y, btn_w, btn_h)) {
                    done_touch_started = true;
                    done_pressed = true;
                }
            }
            if (touch.is_pressed && done_touch_started) {
                done_pressed = isPointInRect(touch.x, touch.y,
                                             btn_x, btn_y, btn_w, btn_h);
            }
            if (touch.was_released) {
                if (done_touch_started && done_pressed) {
                    Serial.println("[BOOT_GUI] USB Disk: Done pressed");
                    should_exit = true;
                }
                done_touch_started = false;
                done_pressed = false;
            }
        }

        // Redraw button on press-state change.
        if (done_pressed != prev_done_pressed) {
            drawButton(btn_x, btn_y, btn_w, btn_h, "Done", done_pressed,
                       /*is_default=*/true);
            prev_done_pressed = done_pressed;
        }

        // Update status line if host mount or error changed.
        bool host = UsbMsc_HostMounted();
        if (!status_drawn || host != prev_host_mounted || ctx.err != prev_err) {
            redrawStatus(host, ctx.err);
            prev_host_mounted = host;
            prev_err = ctx.err;
            status_drawn = true;
        }

        // If the MSC task finished without user intervention (init failure),
        // keep any error visible briefly, then bail out.
        if (ctx.finished && !should_exit) {
            delay(2000);
            should_exit = true;
        }

        delay(1);
    }

    // Tell the MSC task to wind down, then wait for it to exit.
    UsbMsc_RequestExit();
    // Poll ctx.finished rather than the task handle: once the task calls
    // vTaskDelete(NULL) the handle is no longer safe to inspect.
    for (int i = 0; i < 200 && !ctx.finished; ++i) {
        delay(25);
    }
    (void)msc_task;  // only used for creation

    Serial.println("[BOOT_GUI] Exiting USB Disk screen");
}

// ============================================================================
// Public API
// ============================================================================

bool BootGUI_Init(void)
{
    Serial.println("[BOOT_GUI] Initializing...");

    // If MacSplash already warmed up the touch panel and started the touch
    // task, skip the warm-up loop - it just adds ~1s of stall for no gain.
    if (!touch_task_running) {
        Serial.println("[BOOT_GUI] Warming up touch panel...");
        for (int i = 0; i < 20; i++) {
            Board_Update();
            delay(50);
        }
        Serial.println("[BOOT_GUI] Touch panel ready");

        if (!startTouchTask()) {
            Serial.println("[BOOT_GUI] WARNING: Failed to start touch task, falling back to sync mode");
        }
    } else {
        Serial.println("[BOOT_GUI] Touch task already running, reusing existing instance");
    }

    // Get display dimensions from the board HAL (rotation-aware)
    SCREEN_WIDTH  = BoardDisplay_Width();
    SCREEN_HEIGHT = BoardDisplay_Height();
    Serial.printf("[BOOT_GUI] Display size: %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);

    // Load saved settings
    loadSettings();

    // Scan for disk files
    scanDiskFiles();
    scanCDROMFiles();
    scanExtFSFolders();

    // If no disk is selected but we found some, select the first one
    if (strlen(selected_disk_path) == 0 && disk_files.size() > 0) {
        strncpy(selected_disk_path, disk_files[0].c_str(), BOOT_GUI_MAX_PATH - 1);
        disk_selection_index = 0;
    }

    // Bring up the hosted WiFi transport unconditionally on boards
    // that have a co-processor (Waveshare) so we can query the slave
    // firmware version before any autoconnect attempts. If it's out
    // of sync with the linked host library, stream the embedded
    // `esp32c6-v<X.Y.Z>.bin` blob into the slave and reboot so WiFi
    // comes up working. On Tab5 BoardWifi_IsCoprocessorFirmwareOutdated
    // is a no-op stub so the whole block resolves to a cheap skip.
#if defined(BOARD_WAVESHARE_P4_101)
    initWiFi();
    runC6FirmwareUpdateIfNeeded();
#endif

    // Kick off WiFi auto-connect in the background if configured. The
    // previous countdown screen used to block on this; now we just fire
    // it off and let it land whenever - the emulator doesn't need it up
    // at boot, and the settings screen can always show the live status.
    if (BOOTGUI_ENABLE_WIFI_AUTOCONNECT && wifi_auto_connect && strlen(wifi_ssid) > 0) {
        Serial.printf("[BOOT_GUI] Auto-connecting to WiFi: %s\n", wifi_ssid);
        initWiFi();
        if (strlen(wifi_password) > 0) {
            WiFi.begin(wifi_ssid, wifi_password);
        } else {
            WiFi.begin(wifi_ssid);
        }
    } else if (wifi_auto_connect && strlen(wifi_ssid) > 0) {
        Serial.println("[BOOT_GUI] WiFi auto-connect disabled by build flag");
    }

    gui_initialized = true;
    Serial.println("[BOOT_GUI] Initialization complete");

    return true;
}

// Shared WiFi cleanup path - used at the end of either settings or the
// silent (no-tap) boot flow. Leaves WiFi up if the user configured it and
// it actually connected; otherwise powers it down to free runtime state.
static void bootGuiCleanupWifi(void)
{
    if (!wifi_initialized) {
        return;
    }
    WiFi.scanDelete();
    wl_status_t status = WiFi.status();
    if (status != WL_CONNECTED || !BOOTGUI_KEEP_WIFI_FOR_EMULATOR) {
        // IMPORTANT: On the Waveshare P4 the WiFi co-processor (ESP32-C6)
        // talks to us over SDIO via esp_wifi_remote / esp_hosted, and
        // esp_hosted.a internally uses the SAME esp_driver_sdmmc host
        // context (s_host_ctx / event_queue in sdmmc_host.c) that slot 0
        // - the microSD card - depends on. Calling WiFi.mode(WIFI_OFF)
        // stops esp_wifi_remote, which tears down esp_hosted, which
        // calls sdmmc_host_deinit() and destroys the shared event queue.
        // The next SD_MMC.open() on slot 0 then asserts inside
        // xQueueReceive with a NULL queue handle and the device reboots.
        //
        // This only manifested once the C6 firmware fell out of date
        // ("Version on Host is NEWER than version on co-processor"):
        // WiFi then never reaches WL_CONNECTED and the non-connected
        // branch below runs. When WiFi does connect we keep it up, which
        // is why this bug was silent for a long time.
        //
        // The right answer is to simply leave WiFi running. We still
        // call WiFi.disconnect(false) so any AP association is dropped
        // and we don't waste RF time, but we never flip the mode to OFF.
        // The emulator's ether_esp32 init reuses the existing WiFi state
        // and either picks up the live connection or treats it as "no
        // network" gracefully. The small extra memory cost is well worth
        // not bricking SD right before the emulator boots.
        Serial.println("[BOOT_GUI] Releasing WiFi association before emulator start "
                       "(leaving stack up to preserve shared SDMMC controller)...");
        WiFi.disconnect(false);
        delay(50);
        Serial.println("[BOOT_GUI] WiFi cleanup complete");
    } else {
        Serial.printf("[BOOT_GUI] WiFi connected, keeping connection (IP: %s)\n",
                     WiFi.localIP().toString().c_str());
    }
}

void BootGUI_RunSettingsOnly(void)
{
    if (!gui_initialized) {
        Serial.println("[BOOT_GUI] ERROR: GUI not initialized");
        return;
    }

    // /basilisk_settings.txt "skip_gui=yes" still honored - just short-
    // circuit the settings UI and clean up WiFi / touch task.
    if (skip_gui) {
        Serial.println("[BOOT_GUI] skip_gui=yes, skipping settings screen");
        Serial.printf("[BOOT_GUI] Using saved settings: disk=%s, ram=%dMB\n",
                      selected_disk_path, selected_ram_mb);
        stopTouchTask();
        bootGuiCleanupWifi();
        return;
    }

    Serial.println("[BOOT_GUI] Running settings screen...");
    runSettingsScreen();

    // Stop the touch task before returning to emulator (emulator has its own input task)
    stopTouchTask();
    bootGuiCleanupWifi();

    Serial.println("[BOOT_GUI] Settings complete, proceeding to emulator");
}

void BootGUI_FinishWithoutUI(void)
{
    Serial.println("[BOOT_GUI] Finishing without settings UI");
    stopTouchTask();
    bootGuiCleanupWifi();
}

// --------------------------------------------------------------------------
// Public touch-task hooks - thin wrappers around the static helpers so
// MacSplash can reuse the same 60 Hz polling task without duplicating it.
// --------------------------------------------------------------------------

bool BootGUI_StartTouchTask(void)
{
    return startTouchTask();
}

void BootGUI_StopTouchTask(void)
{
    stopTouchTask();
}

bool BootGUI_PollTouch(BootGUITouch *out)
{
    if (!out) return false;
    TouchEvent evt;
    if (!getTouchEvent(&evt)) return false;
    out->x            = evt.x;
    out->y            = evt.y;
    out->is_pressed   = evt.is_pressed;
    out->was_pressed  = evt.was_pressed;
    out->was_released = evt.was_released;
    return true;
}

const char* BootGUI_GetDiskPath(void)
{
    return selected_disk_path;
}

const char* BootGUI_GetCDROMPath(void)
{
    return selected_cdrom_path;
}

const char* BootGUI_GetExtFSPath(void)
{
    return selected_extfs_path;
}

uint32_t BootGUI_GetRAMSize(void)
{
    return (uint32_t)selected_ram_mb * 1024 * 1024;
}

int BootGUI_GetRAMSizeMB(void)
{
    return selected_ram_mb;
}

const char* BootGUI_GetWiFiSSID(void)
{
    return wifi_ssid;
}

const char* BootGUI_GetWiFiPassword(void)
{
    return wifi_password;
}

bool BootGUI_GetWiFiAutoConnect(void)
{
    return wifi_auto_connect;
}

bool BootGUI_GetBootFromCD(void)
{
    return boot_from_cd;
}

int BootGUI_GetRotation(void)
{
    return rotation_degrees;
}

bool BootGUI_GetAudioEnabled(void)
{
    return audio_enabled;
}

bool BootGUI_IsWiFiConnected(void)
{
    return WiFi.status() == WL_CONNECTED;
}

uint32_t BootGUI_GetWiFiIP(void)
{
    if (WiFi.status() != WL_CONNECTED) {
        return 0;
    }
    // Return IP in host byte order (ESP32 gives it in network byte order)
    IPAddress ip = WiFi.localIP();
    return (uint32_t(ip[0]) << 24) | (uint32_t(ip[1]) << 16) | 
           (uint32_t(ip[2]) << 8) | uint32_t(ip[3]);
}
