/*
 *  boot_gui.cpp - Pre-boot configuration GUI
 *
 *  BasiliskII ESP32 Port
 *
 *  Mac OS 8 style boot configuration screens (Platinum appearance, Charcoal
 *  font, drawn at the emulated Mac's resolution and pixel-doubled):
 *  - Hard disk image selection
 *  - CD-ROM ISO selection
 *  - RAM size selection (8/16/20/24 MB)
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
#include <atomic>
#include <time.h>
#include "esp_system.h"
#include "mac_clock.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "boot_gui.h"
#include "firmware_update.h"
#include "boot_control_values.h"
#include "boot_media.h"
#include "board.h"
#include "board_config.h"
#include "board_display.h"
#include "board_touch.h"
#include "board_wifi.h"
#include "board_sd.h"  /* SD_FS alias */
#include "preboot_screens.h"  /* Mac OS 8 Platinum screens on a 2x canvas */
#include "usb_msc.h"
#include "starter_download.h"

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
static SemaphoreHandle_t touch_task_stopped = nullptr;
static volatile bool touch_task_running = false;

#define TOUCH_TASK_STACK_SIZE 4096
#define TOUCH_TASK_PRIORITY   1
#define TOUCH_POLL_INTERVAL_MS 16  // ~60Hz polling

// Track previous touch state for edge detection
static volatile bool touch_prev_pressed = false;
static volatile bool touch_edge_pressed = false;
static volatile bool touch_edge_released = false;
static portMUX_TYPE touch_spinlock = portMUX_INITIALIZER_UNLOCKED;

// Serial automation can stand in for the touch panel (TOUCH commands), so
// preboot screens can be exercised and captured without a finger.
static bool touch_injected = false;
static bool touch_injected_pressed = false;
static int16_t touch_injected_x = 0;
static int16_t touch_injected_y = 0;

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
        portENTER_CRITICAL(&touch_spinlock);
        if (touch_injected) {
            touch.pressed = touch_injected_pressed;
            touch.x = touch_injected_x;
            touch.y = touch_injected_y;
        }
        portEXIT_CRITICAL(&touch_spinlock);
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
    touch_task_handle = nullptr;
    xSemaphoreGive(touch_task_stopped);
    vTaskDelete(NULL);
}

// Start the touch task (also exposed publicly as BootGUI_StartTouchTask so
// that MacSplash can reuse the same polling task before BootGUI_Init runs).
static bool startTouchTask(void)
{
    if (touch_task_running) {
        return true;  // Already running
    }
    if (touch_queue != nullptr) {
        // A previous stop timed out. Do not replace resources a task may use.
        return false;
    }
    
    // Create queue for single touch event (overwrite mode)
    touch_queue = xQueueCreate(1, sizeof(TouchEvent));
    if (!touch_queue) {
        Serial.println("[BOOT_GUI] ERROR: Failed to create touch queue");
        return false;
    }
    touch_task_stopped = xSemaphoreCreateBinary();
    if (!touch_task_stopped) {
        vQueueDelete(touch_queue);
        touch_queue = nullptr;
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
        vSemaphoreDelete(touch_task_stopped);
        touch_task_stopped = nullptr;
        return false;
    }
    
    Serial.println("[BOOT_GUI] Touch task created successfully");
    return true;
}

// Stop the touch task
static void stopTouchTask(void)
{
    if (touch_queue == nullptr) {
        return;
    }
    
    Serial.println("[BOOT_GUI] Stopping touch task...");
    
    // Signal task to stop
    touch_task_running = false;
    
    // The worker deletes itself. Its TCB may already be freed/reused after a
    // fixed delay, so neither eTaskGetState nor vTaskDelete(handle) is safe.
    // Wait for a positive signal before freeing the queue it last touched.
    if (xSemaphoreTake(touch_task_stopped, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("[BOOT_GUI] ERROR: Touch stop timed out; resources retained");
        return;
    }
    vSemaphoreDelete(touch_task_stopped);
    touch_task_stopped = nullptr;
    
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
// Settings Storage
// ============================================================================

static char selected_disk_path[BOOT_GUI_MAX_PATH] = "";
static char selected_cdrom_path[BOOT_GUI_MAX_PATH] = "";
static char selected_extfs_path[BOOT_GUI_MAX_PATH] = "";
static int selected_ram_mb = kBootRamDefaultMB;
static bool skip_gui = false;    // If true, skip boot GUI and go straight to emulator

// WiFi settings storage
static char wifi_ssid[64] = "";
static char wifi_password[64] = "";
static bool wifi_auto_connect = false;
static bool wifi_initialized = false;

// Audio settings
static bool audio_enabled = true;  // Default: audio enabled

// Power Manager emulation (battery, sleep) on boards with a battery.
// Settings file and serial only; there is no preboot checkbox.
static bool power_manager_enabled = true;

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

static int disk_selection_index = -1;
static int cdrom_selection_index = 0;  // 0 = None
static int extfs_selection_index = 0;  // 0 = None
static int disk_scroll_offset = 0;
static int cdrom_scroll_offset = 0;
static int extfs_scroll_offset = 0;

// ============================================================================
// UI State
// ============================================================================

static bool gui_initialized = false;
static const char *control_phase = "initializing";
static const char *control_screen = "none";
static bool control_boot_requested = false;
static bool control_hold = false;
static bool starter_requested = false;
static bool control_leave_starter = false;
static bool starter_system7 = false;
static bool starter_job_system7 = false;
static bool starter_cancel_pending = false;
static char starter_notice[128] = {};
static bool control_leave_wifi = false;
static bool control_wifi_changed = false;
static uint32_t control_restart_at = 0;
static RTC_NOINIT_ATTR uint32_t control_boot_cookie;
static const uint32_t CONTROL_BOOT_COOKIE = 0xB205E771;
struct BootControlRequest {
    uint32_t id, expires;
    char command[600];
};
struct BootControlReply {
    uint32_t id;
    char text[600];
};
bool boot_control_pending = false;
static QueueHandle_t control_requests = nullptr;
static QueueHandle_t control_replies = nullptr;

// ============================================================================
// Forward Declarations
// ============================================================================

static void loadSettings(void);
static bool saveSettings(void);
static void scanDiskFiles(void);
static void scanCDROMFiles(void);
static void scanExtFSFolders(void);
static void runSettingsScreen(void);
static void runWiFiScreen(void);
static void runUsbMscScreen(void);
static void initWiFi(void);
static void drawC6FirmwareUpdateSplash(void);
static void runC6FirmwareUpdateIfNeeded(void);

// ============================================================================
// Settings Load/Save
// ============================================================================

static void loadSettings(void)
{
    Serial.println("[BOOT_GUI] Loading settings...");
    
    // Recover the previous complete file if power was lost between renames.
    if (!SD_FS.exists(SETTINGS_FILE) && SD_FS.exists("/basilisk_settings.bak")) {
        SD_FS.rename("/basilisk_settings.bak", SETTINGS_FILE);
    }
    File file = SD_FS.open(SETTINGS_FILE, FILE_READ);
    if (!file) {
        Serial.println("[BOOT_GUI] No settings file found, using defaults");
        return;
    }
    
    while (file.available()) {
        String line = file.readStringUntil('\n');
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        
        int eq_pos = line.indexOf('=');
        if (eq_pos <= 0) {
            continue;
        }
        
        String key = line.substring(0, eq_pos);
        String value = line.substring(eq_pos + 1);
        key.trim();

        
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
            selected_ram_mb = BootControlSavedRam(value.toInt());
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
        } else if (key == "power_manager") {
            power_manager_enabled = (value == "yes" || value == "true" || value == "1");
            Serial.printf("[BOOT_GUI] Loaded power_manager: %s\n", power_manager_enabled ? "yes" : "no");
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

static bool saveSettings(void)
{
    // Write and verify a complete replacement before moving the old file.
    // FAT rename is not an atomic overwrite, so retain a recoverable backup.
    String text;
    text.reserve(1200);
    text += "disk="; text += selected_disk_path; text += '\n';
    text += "cdrom="; text += selected_cdrom_path; text += '\n';
    text += "extfs="; text += selected_extfs_path; text += '\n';
    text += "ramsize="; text += selected_ram_mb; text += '\n';
    text += "skip_gui="; text += skip_gui ? "yes\n" : "no\n";
    text += "wifi_ssid="; text += wifi_ssid; text += '\n';
    text += "wifi_pass="; text += wifi_password; text += '\n';
    text += "wifi_auto="; text += wifi_auto_connect ? "yes\n" : "no\n";
    text += "audio="; text += audio_enabled ? "yes\n" : "no\n";
    text += "boot_from_cd="; text += boot_from_cd ? "yes\n" : "no\n";
    text += "rotation="; text += rotation_degrees; text += '\n';
    text += "power_manager="; text += power_manager_enabled ? "yes\n" : "no\n";
    const char *temp = "/basilisk_settings.tmp";
    const char *backup = "/basilisk_settings.bak";
    if (SD_FS.exists(temp) && !SD_FS.remove(temp)) return false;
    File file = SD_FS.open(temp, FILE_WRITE);
    if (!file) return false;
    size_t written = file.print(text);
    file.flush();
    file.close();
    file = SD_FS.open(temp, FILE_READ);
    bool verified = file && written == text.length() && file.size() == text.length();
    for (size_t i = 0; verified && i < text.length(); ++i) {
        verified = file.read() == (uint8_t)text[i];
    }
    file.close();
    if (!verified) return false;
    if (SD_FS.exists(backup) && !SD_FS.remove(backup)) return false;
    bool had_previous = SD_FS.exists(SETTINGS_FILE);
    if (had_previous && !SD_FS.rename(SETTINGS_FILE, backup)) return false;
    if (!SD_FS.rename(temp, SETTINGS_FILE)) {
        if (had_previous) SD_FS.rename(backup, SETTINGS_FILE);
        return false;
    }
    if (had_previous) SD_FS.remove(backup);
    Serial.println("[BOOT_GUI] Settings saved");
    return true;
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
    
    // Select a default only when there is no saved disk. A missing saved
    // disk remains selected by path until the user chooses another image.
    disk_selection_index = BootSelectDisk(disk_files, selected_disk_path, sizeof(selected_disk_path));
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
// Platinum UI glue
// ============================================================================
// Screens are drawn by preboot_screens.cpp on a logical canvas half the panel
// size (the emulated Mac's resolution) and pixel-doubled by Present(). Here we
// translate touches into canvas coordinates and turn live state into views.

namespace UI = PrebootUI;
namespace Screens = PrebootScreens;

struct UiTouch {
    int x, y;            // canvas coordinates
    bool down;           // finger currently on the glass
    bool pressed;        // touch began since the last poll
    bool released;       // touch ended since the last poll
};

static bool pollUiTouch(UiTouch *t)
{
    TouchEvent e;
    if (!getTouchEvent(&e)) return false;
    t->x = e.x / UI::kScale;
    t->y = e.y / UI::kScale;
    t->down = e.is_pressed;
    t->pressed = e.was_pressed;
    t->released = e.was_released;
    return true;
}

// Show the same local clock as the guest, including a saved manual setting.
static void formatMenuClock(char *out, size_t n)
{
    time_t now = static_cast<int64_t>(TimerDateTime()) - 2082844800LL;
    struct tm tm_now;
    gmtime_r(&now, &tm_now);  // Mac seconds already represent local time.
    int hour12 = tm_now.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(out, n, "%d:%02d %s", hour12, tm_now.tm_min,
             tm_now.tm_hour >= 12 ? "PM" : "AM");
}

static int rssiBars(int rssi)
{
    if (rssi >= -50) return 4;
    if (rssi >= -60) return 3;
    if (rssi >= -70) return 2;
    if (rssi >= -80) return 1;
    return 0;
}

static void paintAlert(const Screens::AlertView &view)
{
    Screens::PaintAlert(Screens::LayoutAlert(UI::Width(), UI::Height(), view), view);
    UI::Present();
}

/* Live Wi-Fi summary for the settings screen's Network box. */
struct WifiSummary {
    int bars;
    char status[72];
    char detail[72];
};

static WifiSummary readWifiSummary(void)
{
    WifiSummary s = {};
    const wl_status_t status = wifi_initialized ? WiFi.status() : WL_IDLE_STATUS;
    const bool saved = wifi_ssid[0] != 0;
    if (status == WL_CONNECTED) {
        s.bars = rssiBars(WiFi.RSSI());
        snprintf(s.status, sizeof(s.status), "Wi-Fi: Connected to %s", wifi_ssid);
        IPAddress ip = WiFi.localIP();
        snprintf(s.detail, sizeof(s.detail), "Address %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    } else if (!saved) {
        snprintf(s.status, sizeof(s.status), "Wi-Fi: Not set up");
        snprintf(s.detail, sizeof(s.detail), "Tap Wi-Fi... to choose a network.");
    } else if (status == WL_CONNECT_FAILED) {
        snprintf(s.status, sizeof(s.status), "Wi-Fi: Could not connect to %s", wifi_ssid);
        snprintf(s.detail, sizeof(s.detail), "Check the password in Wi-Fi...");
    } else if (status == WL_NO_SSID_AVAIL) {
        snprintf(s.status, sizeof(s.status), "Wi-Fi: %s is not in range", wifi_ssid);
    } else if (wifi_auto_connect && wifi_initialized) {
        snprintf(s.status, sizeof(s.status), "Wi-Fi: Connecting to %s...", wifi_ssid);
    } else {
        snprintf(s.status, sizeof(s.status), "Wi-Fi: Not connected");
        snprintf(s.detail, sizeof(s.detail), "Saved network: %s", wifi_ssid);
    }
    return s;
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
    // A caution alert explains the 10-30 s pause while the blob streams.
    control_screen = "firmware";
    char clock[16];
    formatMenuClock(clock, sizeof(clock));
    Screens::AlertView view = {};
    view.clock = clock;
    view.icon = UI::kIconCaution;
    view.heading = "Updating the Wi-Fi co-processor.";
    view.body = "This happens once after a firmware update. The device restarts "
                "automatically when the update is complete.";
    view.note = "Do not unplug or switch off.";
    paintAlert(view);
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
// Settings Screen
// ============================================================================

static std::vector<std::string> *mediaList(int column)
{
    return column == 0 ? &disk_files : column == 1 ? &cdrom_files : &extfs_folders;
}

// Rows in a media list; CD-ROM and Shared Folder lead with "None".
static int mediaCount(int column)
{
    return (int)mediaList(column)->size() + (column ? 1 : 0);
}

static int mediaSelection(int column)
{
    return column == 0 ? disk_selection_index
         : column == 1 ? cdrom_selection_index : extfs_selection_index;
}

static void selectMedia(int column, int row)
{
    if (row < 0 || row >= mediaCount(column)) return;
    if (column == 0) {
        disk_selection_index = row;
        snprintf(selected_disk_path, sizeof(selected_disk_path), "%s", disk_files[row].c_str());
        Serial.printf("[BOOT_GUI] Selected disk [%d]: %s\n", row, selected_disk_path);
        return;
    }
    char *path = column == 1 ? selected_cdrom_path : selected_extfs_path;
    (column == 1 ? cdrom_selection_index : extfs_selection_index) = row;
    if (row == 0) path[0] = '\0';
    else snprintf(path, BOOT_GUI_MAX_PATH, "%s", (*mediaList(column))[row - 1].c_str());
    Serial.printf("[BOOT_GUI] Selected %s: %s\n", column == 1 ? "CD-ROM" : "shared folder",
                  row ? path : "None");
}

// The ROM loader accepts 64 KiB through 1 MiB. Existing user-supplied media
// can be selected normally; checksums apply to files installed by the downloader.
static bool usableFile(const char *path, size_t minimum, size_t maximum = SIZE_MAX)
{
    if (!path || !path[0]) return false;
    File f = SD_FS.open(path, FILE_READ);
    return f && !f.isDirectory() && f.size() >= minimum && f.size() <= maximum;
}

static bool bootMediaReady()
{
    return BootHasMedia(boot_from_cd, selected_disk_path, selected_cdrom_path, usableFile);
}

static bool startStarterDownload(bool system7)
{
    if (!wifi_initialized || !BootGUI_IsWiFiConnected()) {
        snprintf(starter_notice, sizeof(starter_notice), "Connect to Wi-Fi before downloading.");
        return false;
    }
    if (!StarterDownloadStart(system7)) {
        snprintf(starter_notice, sizeof(starter_notice), "Could not start the download. Try again.");
        return false;
    }
    starter_system7 = starter_job_system7 = system7;
    starter_cancel_pending = false;
    starter_notice[0] = 0;
    return true;
}

static bool startInstalledSystem()
{
    if (StarterDownloadStatus().state != StarterState::Done || starter_system7 != starter_job_system7)
        return false;
    const char *path = starter_system7 ? "/Macintosh7.dsk" : "/Macintosh8.dsk";
    if (!usableFile("/Q650.ROM", 64 * 1024, 1024 * 1024) || !usableFile(path, 512)) {
        snprintf(starter_notice, sizeof(starter_notice), "Startup files are missing. Check the card in Settings.");
        return false;
    }
    char old_path[sizeof(selected_disk_path)];
    strcpy(old_path, selected_disk_path);
    const bool old_boot_cd = boot_from_cd;
    strcpy(selected_disk_path, path);
    boot_from_cd = false;
    if (!saveSettings()) {
        strcpy(selected_disk_path, old_path);
        boot_from_cd = old_boot_cd;
        snprintf(starter_notice, sizeof(starter_notice), "Could not save the startup disk. Check the card and try again.");
        return false;
    }
    control_boot_requested = true;
    return true;
}

static void runStarterScreen()
{
    const auto L = Screens::LayoutStarter(UI::Width(), UI::Height());
    control_screen = "starter";
    control_leave_starter = false;
    int pressed = Screens::kNoControl;
    bool inside = false, dirty = true, connected = false;
    uint32_t wifi_at = 0, paint_at = 0;
    char shown_clock[16] = {}, shown_status[192] = {};
    bool shown_connected = false, shown_working = false, shown_done = false, shown_failed = false;
    while (!control_boot_requested) {
        BootGUI_ControlPoll();
        if (control_boot_requested || control_leave_starter) return;
        const StarterDownloadInfo info = StarterDownloadStatus();
        const bool working = info.state == StarterState::Working;
        const bool done = info.state == StarterState::Done && starter_job_system7 == starter_system7;
        if ((int32_t)(millis() - wifi_at) >= 0) {
            connected = wifi_initialized && BootGUI_IsWiFiConnected();
            wifi_at = millis() + 1000;
        }
        const bool action_enabled = working ? !starter_cancel_pending : done || connected;
        UiTouch t;
        if (pollUiTouch(&t)) {
            const int hit = Screens::StarterControlAt(L, t.x, t.y);
            if (t.pressed) {
                pressed = (working && hit != Screens::kStarterAction) ||
                    (hit == Screens::kStarterAction && !action_enabled) ? Screens::kNoControl : hit;
                inside = true;
            }
            if (t.down) inside = pressed == hit;
            if (t.released) {
                if (pressed == hit && inside) {
                    if (!working && pressed == Screens::kStarterSettings) {
                        scanDiskFiles();
                        return;
                    }
                    if (!working && pressed == Screens::kStarterWiFi) {
                        runWiFiScreen();
                        control_screen = "starter";
                        wifi_at = 0;
                    } else if (!working && (pressed == Screens::kStarter7 || pressed == Screens::kStarter8)) {
                        starter_system7 = pressed == Screens::kStarter7;
                        starter_notice[0] = 0;
                    } else if (pressed == Screens::kStarterAction && action_enabled) {
                        if (working) { StarterDownloadCancel(); starter_cancel_pending = true; }
                        else if (done) { if (startInstalledSystem()) return; }
                        else startStarterDownload(starter_system7);
                    }
                }
                pressed = Screens::kNoControl;
            }
            dirty = true;
        }
        if (dirty || (int32_t)(millis() - paint_at) >= 0) {
            char clock[16], status[192];
            formatMenuClock(clock, sizeof(clock));
            const bool failed = starter_job_system7 == starter_system7 &&
                (info.state == StarterState::Failed || info.state == StarterState::Cancelled);
            const char *message = starter_notice[0] ? starter_notice :
                working && starter_cancel_pending ? "Cancelling download..." :
                (working || done || failed) && info.message[0] ? info.message :
                connected ? "Choose Download to install the startup files." : "Choose Wi-Fi to connect to a network.";
            int percent = -1;
            if (working && info.total) {
                percent = (int)((uint64_t)info.completed * 100 / info.total);
                snprintf(status, sizeof(status), "%s  %u%%", message, (unsigned)percent);
            } else snprintf(status, sizeof(status), "%s", message);
            const char *action = working ? "Cancel" : done ? "Start Up" : failed ? "Retry" : "Download";
            Screens::StarterView view = {clock, status, connected ? "Wi-Fi connected" : "Wi-Fi disconnected",
                action, inside ? pressed : (int)Screens::kNoControl, percent,
                starter_system7, working, action_enabled};
            if (dirty || connected != shown_connected || working != shown_working || done != shown_done ||
                failed != shown_failed || strcmp(clock, shown_clock) || strcmp(status, shown_status)) {
                Screens::PaintStarter(L, view);
                UI::Present();
                snprintf(shown_clock, sizeof(shown_clock), "%s", clock);
                snprintf(shown_status, sizeof(shown_status), "%s", status);
                shown_connected = connected; shown_working = working; shown_done = done; shown_failed = failed;
            }
            paint_at = millis() + 500;
            dirty = false;
        }
        delay(5);
    }
}

/* Everything the settings screen shows; repaint only when it changes. */
struct SettingsKey {
    int selection[3], first[3], count[3], scroll[3];
    int ram, pressed, bars;
    bool sound, boot_cd, rotate, needs_setup, usb_waits_for_update;
    char clock[16], status[72], detail[72];
};

static void runSettingsScreen(void)
{
    starter_requested |= !control_hold && !bootMediaReady();
    for (;;) {
        if (starter_requested) {
            starter_requested = false;
            runStarterScreen();
            if (control_boot_requested) return;
        }
        control_screen = "settings";
        Serial.println("[BOOT_GUI] Showing settings screen...");
        Serial.printf("[BOOT_GUI] Found %d disk files, %d CD-ROM files, %d shared folders\n",
                      (int)disk_files.size(), (int)cdrom_files.size(), (int)extfs_folders.size());

        const bool usb_available = UsbMsc_IsSupported();
        const Screens::SettingsLayout L =
            Screens::LayoutSettings(UI::Width(), UI::Height(), usb_available);
        int first[3];
        for (int c = 0; c < 3; ++c)
            first[c] = Screens::ListReveal(L.lists[c], 0, mediaCount(c), mediaSelection(c));

        int pressed = Screens::kNoControl;   // control touched down on
        bool inside = false;                 // finger still over it
        int scroll_column = -1, scroll_dir = 0;
        uint32_t repeat_at = 0, refresh_at = 0;
        WifiSummary wifi = {};
        char clock[16] = "";
        SettingsKey shown;
        bool painted = false, update_notice = false, ready = bootMediaReady();
        enum { kStay, kStart, kOpenWiFi, kOpenUsb } next = kStay;

        while (next == kStay) {
            BootGUI_ControlPoll();
            if (control_restart_at) {
                if (BootGUI_RestartPending()) BootGUI_Restart();
                delay(1);
                continue;
            }
            if (FirmwareUpdateBusy()) {
                if (!update_notice) {
                    Screens::AlertView update = {};
                    update.clock = clock;
                    update.icon = UI::kIconMac;
                    update.heading = "Updating firmware";
                    update.body = "Keep the device powered on. Check progress or cancel from the computer connected over USB.";
                    const auto layout = Screens::LayoutAlert(UI::Width(), UI::Height(), update);
                    Screens::PaintAlert(layout, update);
                    UI::Present();
                    update_notice = true;
                    painted = false;
                }
                UiTouch ignored;
                pollUiTouch(&ignored);
                pressed = Screens::kNoControl;
                inside = false;
                scroll_column = -1;
                FirmwareUpdatePoll();
                delay(1);
                continue;
            }
            update_notice = false;
            if (control_boot_requested || starter_requested) break;
            bool usb_waits_for_update = false;
#if defined(BOARD_M5STACK_TAB5)
            // Leaving USB Disk on this board reboots it to release its USB PHY.
            usb_waits_for_update = FirmwareUpdateReady();
#endif

            UiTouch t;
            if (pollUiTouch(&t)) {
                if (t.pressed) {
                    pressed = Screens::SettingsControlAt(L, t.x, t.y);
                    inside = true;
                    if (pressed >= Screens::kDiskList && pressed <= Screens::kFolderList) {
                        // Lists act at touch-down, like the Mac List Manager.
                        const int c = pressed - Screens::kDiskList;
                        const int part = Screens::ListScrollPartAt(L.lists[c], first[c],
                                                                   mediaCount(c), t.x, t.y);
                        if (part == -1 || part == 1) {
                            scroll_column = c;
                            scroll_dir = part;
                            first[c] += part;
                            repeat_at = millis() + 400;
                        } else if (part == -2 || part == 2) {
                            first[c] += part / 2 * L.lists[c].visible;
                        } else {
                            selectMedia(c, Screens::ListRowAt(L.lists[c], first[c],
                                                              mediaCount(c), t.x, t.y));
                        }
                        pressed = Screens::kNoControl;
                    }
                    Serial.printf("[BOOT_GUI] Touch at (%d, %d) control=%d\n", t.x, t.y, pressed);
                }
                if (t.down && pressed != Screens::kNoControl)
                    inside = Screens::SettingsControlAt(L, t.x, t.y) == pressed;
                if (t.released) {
                    if (pressed != Screens::kNoControl && inside) {
                        if (pressed >= Screens::kRamFirst && pressed <= Screens::kRamLast) {
                            selected_ram_mb = kBootRamChoicesMB[pressed - Screens::kRamFirst];
                        } else if (pressed == Screens::kSoundBox) {
                            audio_enabled = !audio_enabled;
                        } else if (pressed == Screens::kBootCDBox) {
                            boot_from_cd = !boot_from_cd;
                        } else if (pressed == Screens::kRotateBox) {
                            rotation_degrees = rotation_degrees == 180 ? 0 : 180;
                        } else if (pressed == Screens::kWiFiButton) {
                            next = kOpenWiFi;
                        } else if (pressed == Screens::kUsbButton) {
                            if (!usb_waits_for_update) next = kOpenUsb;
                        } else if (pressed == Screens::kStartButton) {
                            if (bootMediaReady()) next = kStart;
                            else starter_requested = true;
                        }
                        Serial.printf("[BOOT_GUI] Control %d activated\n", pressed);
                    }
                    pressed = Screens::kNoControl;
                    scroll_column = -1;
                    scroll_dir = 0;
                }
            }

            // Holding a scroll arrow repeats, as on the Mac.
            if (scroll_column >= 0 && (int32_t)(millis() - repeat_at) >= 0) {
                first[scroll_column] += scroll_dir;
                repeat_at = millis() + 100;
            }
            // Serial RESCAN can change list lengths underneath us.
            for (int c = 0; c < 3; ++c)
                first[c] = Screens::ListClampFirst(L.lists[c], first[c], mediaCount(c));

            if ((int32_t)(millis() - refresh_at) >= 0) {
                ready = bootMediaReady();
                wifi = readWifiSummary();
                formatMenuClock(clock, sizeof(clock));
                refresh_at = millis() + 1000;
            }

            Screens::SettingsView v = {};
            static const char *const titles[3] = {"Hard Disk:", "CD-ROM:", "Shared Folder:"};
            static const UI::Icon icons[3] = {UI::kIconHardDisk, UI::kIconCD, UI::kIconFolder};
            for (int c = 0; c < 3; ++c)
                v.columns[c] = {titles[c], mediaList(c), c != 0, mediaSelection(c), first[c],
                                scroll_column == c ? scroll_dir : 0, icons[c]};
            v.clock = clock;
            v.ram_mb = selected_ram_mb;
            v.sound = audio_enabled;
            v.boot_from_cd = boot_from_cd;
            v.rotate180 = rotation_degrees == 180;
            v.usb_available = usb_available;
            v.usb_waits_for_update = usb_waits_for_update;
            v.needs_setup = !ready;
            v.wifi_bars = wifi.bars;
            v.wifi_status = wifi.status;
            v.wifi_detail = wifi.detail;
            v.pressed = inside ? pressed : Screens::kNoControl;

            SettingsKey key;
            memset(&key, 0, sizeof(key));
            for (int c = 0; c < 3; ++c) {
                key.selection[c] = v.columns[c].selected;
                key.first[c] = first[c];
                key.count[c] = mediaCount(c);
                key.scroll[c] = v.columns[c].pressed_scroll;
            }
            key.ram = v.ram_mb;
            key.pressed = v.pressed;
            key.bars = v.wifi_bars;
            key.sound = v.sound;
            key.boot_cd = v.boot_from_cd;
            key.needs_setup = v.needs_setup;
            key.usb_waits_for_update = usb_waits_for_update;
            key.rotate = v.rotate180;
            memcpy(key.clock, clock, sizeof(key.clock));
            memcpy(key.status, wifi.status, sizeof(key.status));
            memcpy(key.detail, wifi.detail, sizeof(key.detail));
            if (!painted || memcmp(&key, &shown, sizeof(key)) != 0) {
                Screens::PaintSettings(L, v);
                UI::Present();
                memcpy(&shown, &key, sizeof(key));  // padding included, for memcmp
                painted = true;
            }
            delay(2);
        }

        if (starter_requested) continue;
        if (next == kOpenWiFi) {
            runWiFiScreen();
            continue;
        }
        if (next == kOpenUsb) {
            runUsbMscScreen();
            // Pick up disk images copied while the card was on USB.
            scanDiskFiles();
            scanCDROMFiles();
            scanExtFSFolders();
            continue;
        }
        break;
    }

    // Serial START already persisted and checked the settings.
    if (!control_boot_requested) saveSettings();
}

// ============================================================================
// WiFi Configuration Screen
// ============================================================================

typedef struct {
    char ssid[33];
    int32_t rssi;
    uint8_t encryption;
} WiFiNetworkInfo;

static std::vector<WiFiNetworkInfo> wifi_networks;

struct WiFiKey {
    int selected, first, count, scroll, pressed, password_length;
    bool scanning, can_join, keyboard, shift;
    char status[96], password[64], clock[16];
};

static void runWiFiScreen(void)
{
    control_screen = "wifi";
    control_leave_wifi = false;
    Serial.println("[BOOT_GUI] Showing WiFi screen...");

    const Screens::WiFiLayout L = Screens::LayoutWiFi(UI::Width(), UI::Height());
    const Screens::KeyboardLayout K = Screens::LayoutKeyboard(UI::Width(), UI::Height());
    std::vector<Screens::NetworkRow> rows;
    char clock[16];
    formatMenuClock(clock, sizeof(clock));

    char password[64] = "";
    char password_backup[64] = "";
    if (wifi_ssid[0] && wifi_password[0]) snprintf(password, sizeof(password), "%s", wifi_password);

    int selected = -1, first = 0, connect_target = -1;
    bool scanning = true, connecting = false, keyboard = false, shift = false;
    int pressed = Screens::kWiFiNoControl, pressed_key = Screens::kKeyNone;
    bool inside = false;
    int scroll_dir = 0;
    uint32_t repeat_at = 0, clock_at = 0;

    auto statusText = [&](char *out, size_t n) {
        const wl_status_t status = WiFi.status();
        if (connecting && connect_target >= 0) {
            snprintf(out, n, "Status: Connecting to %s...", wifi_networks[connect_target].ssid);
        } else if (status == WL_CONNECTED) {
            IPAddress ip = WiFi.localIP();
            snprintf(out, n, "Status: Connected to %s (%u.%u.%u.%u)", WiFi.SSID().c_str(),
                     ip[0], ip[1], ip[2], ip[3]);
        } else if (status == WL_CONNECT_FAILED) {
            snprintf(out, n, "Status: Could not connect. Check the password.");
        } else if (status == WL_NO_SSID_AVAIL) {
            snprintf(out, n, "Status: Network not found");
        } else {
            snprintf(out, n, "Status: Not connected");
        }
    };

    auto paint = [&](const char *status) {
        if (keyboard) {
            const char *network = selected >= 0 ? wifi_networks[selected].ssid : wifi_ssid;
            Screens::KeyboardView kv = {clock, network, password, shift,
                                        inside ? pressed_key : (int)Screens::kKeyNone};
            Screens::PaintKeyboard(K, kv);
        } else {
            Screens::WiFiView wv = {};
            wv.clock = clock;
            wv.networks = &rows;
            wv.selected = selected;
            wv.first = first;
            wv.pressed_scroll = scroll_dir;
            wv.scanning = scanning;
            wv.status = status;
            wv.password_length = (int)strlen(password);
            wv.can_join = selected >= 0 && !connecting;
            wv.pressed = inside ? pressed : Screens::kWiFiNoControl;
            Screens::PaintWiFi(L, wv);
        }
        UI::Present();
    };

    // Paint at once: initWiFi() can take a second or two on a cold boot.
    paint("Status: Starting Wi-Fi...");
    initWiFi();

    // After a failed auto-connect the driver may still be associating.
    // Disconnect (without erasing config) so the scan can run.
    wl_status_t pre_status = WiFi.status();
    Serial.printf("[BOOT_GUI] WiFi pre-scan status: %d\n", pre_status);
    if (pre_status != WL_CONNECTED && pre_status != WL_IDLE_STATUS && pre_status != WL_DISCONNECTED) {
        WiFi.disconnect(false);
        delay(100);
    }
    WiFi.scanDelete();
    Serial.println("[BOOT_GUI] Starting initial WiFi scan...");
    int scan_result = WiFi.scanNetworks(true);  // Async scan
    Serial.printf("[BOOT_GUI] Scan initiated, result: %d\n", scan_result);

    WiFiKey shown;
    bool painted = false, should_exit = false;
    char status[96] = "";
    uint32_t status_at = 0;
    bool was_connecting = false;
    while (!should_exit) {
        BootGUI_ControlPoll();
        if (control_leave_wifi) break;

        if (scanning) {
            int16_t result = WiFi.scanComplete();
            if (result >= 0) {
                Serial.printf("[BOOT_GUI] Scan complete, found %d networks\n", result);
                wifi_networks.clear();
                for (int i = 0; i < result; i++) {
                    WiFiNetworkInfo info;
                    snprintf(info.ssid, sizeof(info.ssid), "%s", WiFi.SSID(i).c_str());
                    if (!info.ssid[0]) continue;   // hidden networks have no name to show
                    info.rssi = WiFi.RSSI(i);
                    info.encryption = WiFi.encryptionType(i);
                    // Several access points can share a name; list it once,
                    // with the strongest signal.
                    bool listed = false;
                    for (WiFiNetworkInfo &known : wifi_networks) {
                        if (strcmp(known.ssid, info.ssid)) continue;
                        if (info.rssi > known.rssi) known = info;
                        listed = true;
                        break;
                    }
                    if (!listed) wifi_networks.push_back(info);
                    Serial.printf("[BOOT_GUI]   %s (RSSI: %d)\n", info.ssid, (int)info.rssi);
                }
                WiFi.scanDelete();
                scanning = false;
                connect_target = -1;
                connecting = false;
                selected = -1;
                rows.clear();
                for (size_t i = 0; i < wifi_networks.size(); ++i) {
                    rows.push_back({wifi_networks[i].ssid, rssiBars(wifi_networks[i].rssi),
                                    wifi_networks[i].encryption != WIFI_AUTH_OPEN});
                    if (wifi_ssid[0] && strcmp(wifi_networks[i].ssid, wifi_ssid) == 0)
                        selected = (int)i;
                }
                first = selected >= 0 ? Screens::ListReveal(L.list, 0, (int)rows.size(), selected) : 0;
            } else if (result == WIFI_SCAN_FAILED) {
                Serial.println("[BOOT_GUI] Scan failed");
                scanning = false;
            }
        }

        if (connecting) {
            wl_status_t status = WiFi.status();
            if (status == WL_CONNECTED && WiFi.SSID() == wifi_networks[connect_target].ssid) {
                Serial.printf("[BOOT_GUI] WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
                connecting = false;
                // Save the credentials that just worked.
                const WiFiNetworkInfo &net = wifi_networks[connect_target];
                snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", net.ssid);
                if (net.encryption == WIFI_AUTH_OPEN) wifi_password[0] = '\0';
                else snprintf(wifi_password, sizeof(wifi_password), "%s", password);
                wifi_auto_connect = true;
                saveSettings();
            } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
                Serial.println("[BOOT_GUI] Connection failed");
                connecting = false;
            }
        }

        UiTouch t;
        if (pollUiTouch(&t)) {
            if (keyboard) {
                if (t.pressed) {
                    pressed_key = Screens::KeyAt(K, t.x, t.y);
                    inside = true;
                }
                if (t.down && pressed_key != Screens::kKeyNone)
                    inside = Screens::KeyAt(K, t.x, t.y) == pressed_key;
                if (t.released) {
                    if (pressed_key != Screens::kKeyNone && inside) {
                        size_t len = strlen(password);
                        if (pressed_key == Screens::kKeyShift) {
                            shift = !shift;
                        } else if (pressed_key == Screens::kKeyDelete) {
                            if (len) password[len - 1] = '\0';
                        } else if (pressed_key == Screens::kKeyOK) {
                            keyboard = false;
                        } else if (pressed_key == Screens::kKeyCancel) {
                            memcpy(password, password_backup, sizeof(password));
                            keyboard = false;
                        } else {
                            for (const auto &key : K.keys) {
                                if (key.normal != pressed_key) continue;
                                if (len + 1 < sizeof(password)) {
                                    password[len] = (char)Screens::KeyCharacter(key, shift);
                                    password[len + 1] = '\0';
                                }
                                break;
                            }
                        }
                        control_screen = keyboard ? "keyboard" : "wifi";
                    }
                    pressed_key = Screens::kKeyNone;
                }
            } else {
                if (t.pressed) {
                    pressed = Screens::WiFiControlAt(L, t.x, t.y);
                    inside = true;
                    if (pressed == Screens::kNetworkList) {
                        const int count = (int)rows.size();
                        const int part = Screens::ListScrollPartAt(L.list, first, count, t.x, t.y);
                        if (part == -1 || part == 1) {
                            scroll_dir = part;
                            first += part;
                            repeat_at = millis() + 400;
                        } else if (part == -2 || part == 2) {
                            first += part / 2 * L.list.visible;
                        } else {
                            const int row = Screens::ListRowAt(L.list, first, count, t.x, t.y);
                            if (row >= 0 && !connecting && row != selected) {
                                const char *saved = !strcmp(wifi_networks[row].ssid, wifi_ssid) ? wifi_password : "";
                                snprintf(password, sizeof(password), "%s", saved);
                                selected = row;
                                Serial.printf("[BOOT_GUI] Selected network: %s\n", wifi_networks[row].ssid);
                            }
                        }
                        pressed = Screens::kWiFiNoControl;
                    }
                }
                if (t.down && pressed != Screens::kWiFiNoControl)
                    inside = Screens::WiFiControlAt(L, t.x, t.y) == pressed;
                if (t.released) {
                    if (pressed != Screens::kWiFiNoControl && inside) {
                        if (pressed == Screens::kScanButton && !scanning) {
                            Serial.println("[BOOT_GUI] Starting WiFi scan...");
                            scanning = true;
                            WiFi.scanNetworks(true);
                        } else if (pressed == Screens::kBackButton) {
                            should_exit = true;
                        } else if (pressed == Screens::kJoinButton && selected >= 0 && !connecting) {
                            const WiFiNetworkInfo &net = wifi_networks[selected];
                            const bool open = net.encryption == WIFI_AUTH_OPEN;
                            Serial.printf("[BOOT_GUI] Connecting to %s (%s)...\n", net.ssid,
                                          open ? "open" : "encrypted");
                            connecting = true;
                            connect_target = selected;
                            if (open || !password[0]) WiFi.begin(net.ssid);
                            else WiFi.begin(net.ssid, password);
                        } else if (pressed == Screens::kPasswordField) {
                            memcpy(password_backup, password, sizeof(password));
                            keyboard = true;
                            shift = false;
                            control_screen = "keyboard";
                        }
                    }
                    pressed = Screens::kWiFiNoControl;
                    scroll_dir = 0;
                }
            }
        }

        if (scroll_dir && (int32_t)(millis() - repeat_at) >= 0) {
            first += scroll_dir;
            repeat_at = millis() + 100;
        }
        first = Screens::ListClampFirst(L.list, first, (int)rows.size());
        if ((int32_t)(millis() - clock_at) >= 0) {
            formatMenuClock(clock, sizeof(clock));
            clock_at = millis() + 1000;
        }
        // The hosted Wi-Fi link is slow to query; refresh 4x a second.
        if (connecting != was_connecting || (int32_t)(millis() - status_at) >= 0) {
            statusText(status, sizeof(status));
            status_at = millis() + 250;
            was_connecting = connecting;
        }

        WiFiKey key;
        memset(&key, 0, sizeof(key));
        key.selected = selected;
        key.first = first;
        key.count = (int)rows.size();
        key.scroll = scroll_dir;
        key.pressed = inside ? (keyboard ? pressed_key : pressed) : -100;
        key.password_length = (int)strlen(password);
        key.scanning = scanning;
        key.can_join = selected >= 0 && !connecting;
        key.keyboard = keyboard;
        key.shift = shift;
        memcpy(key.status, status, sizeof(key.status));
        if (keyboard) memcpy(key.password, password, sizeof(key.password));
        memcpy(key.clock, clock, sizeof(key.clock));
        if (!painted || memcmp(&key, &shown, sizeof(key)) != 0) {
            paint(key.status);
            memcpy(&shown, &key, sizeof(key));  // padding included, for memcmp
            painted = true;
        }
        delay(2);
    }

    Serial.println("[BOOT_GUI] Exiting WiFi screen");
}

// ============================================================================
// USB Disk Mode Screen
// ============================================================================
//
// Shows a Mac alert explaining that the SD card is available over USB, while
// the MSC stack runs on its own FreeRTOS task so the touch loop stays live.
// "Done" tears the stack down and returns to the settings screen, which
// rescans the card for newly copied disk and CD images.

// The UI owns this state until the worker publishes completion.
struct UsbMscTaskCtx {
    const char *error = nullptr;
    std::atomic<bool> finished{false};
};

static UI::Rect s_usb_done_button = {0, 0, 0, 0};   // for BOOT HIT done

static void usbMscTask(void *arg)
{
    UsbMscTaskCtx *ctx = static_cast<UsbMscTaskCtx *>(arg);
    UsbMsc_Enter(&ctx->error);
    ctx->finished.store(true);
    vTaskDelete(NULL);
}

static void runUsbMscScreen(void)
{
    control_screen = "usb";
    Serial.println("[BOOT_GUI] Entering USB Disk screen...");

    if (!UsbMsc_IsSupported()) {
        // Shouldn't happen - button is hidden - but fail gracefully.
        Serial.println("[BOOT_GUI] USB MSC not supported in this build");
        return;
    }

    // Spin up MSC in a background task so the touch loop keeps polling.
    UsbMscTaskCtx ctx;
    TaskHandle_t msc_task = nullptr;
    BaseType_t rc = xTaskCreatePinnedToCore(
        usbMscTask, "usb_msc", 6144,
        &ctx, 2, &msc_task, 0);
    if (rc != pdPASS) {
        Serial.println("[BOOT_GUI] Failed to create USB MSC task");
        return;
    }

    char clock[16];
    formatMenuClock(clock, sizeof(clock));
    Screens::AlertView view = {};
    view.clock = clock;
    view.icon = UI::kIconCard;
    view.heading = "Your SD card is now available as a USB drive.";
    view.body = "Copy files on your computer, eject the drive there, then tap Done "
                "to return to Startup Settings.";
#if defined(BOARD_M5STACK_TAB5)
    view.note = "Use a USB-A to USB-C cable in the Tab5's USB-A port (not USB-C).";
#endif
    view.status = "Waiting for the computer...";
    view.button = "Done";
    const Screens::AlertLayout layout = Screens::LayoutAlert(UI::Width(), UI::Height(), view);
    s_usb_done_button = layout.button;

    bool pressed = false, inside = false, should_exit = false, painted = false;
    const char *shown_status = nullptr;
    bool shown_pressed = false;
    while (!should_exit) {
        BootGUI_ControlPoll();
        UiTouch t;
        if (pollUiTouch(&t)) {
            const bool over = layout.button.inset(-4).contains(t.x, t.y);
            if (t.pressed && over) {
                pressed = true;
                inside = true;
            }
            if (t.down && pressed) inside = over;
            if (t.released) {
                if (pressed && inside) {
                    Serial.println("[BOOT_GUI] USB Disk: Done pressed");
                    should_exit = true;
                }
                pressed = inside = false;
            }
        }

        const bool finished = ctx.finished.load();
        view.status = finished && ctx.error ? ctx.error
                    : UsbMsc_HostMounted() ? "Connected. It is safe to copy files."
                                           : "Waiting for the computer...";
        view.button_pressed = pressed && inside;
        if (!painted || view.status != shown_status || view.button_pressed != shown_pressed) {
            Screens::PaintAlert(layout, view);
            UI::Present();
            shown_status = view.status;
            shown_pressed = view.button_pressed;
            painted = true;
        }

        // If the MSC task finished without user intervention (init failure),
        // keep any error visible briefly, then bail out.
        if (finished && !should_exit) {
            delay(2000);
            should_exit = true;
        }
        delay(2);
    }

    // The worker can still be starting when Done is tapped. Keep asking it
    // to stop until it releases the card or restarts the Tab5. The settings
    // screen must never rescan a card that is still exported to the host.
    while (!ctx.finished.load()) {
        UsbMsc_RequestExit();
        BootGUI_ControlPoll();
        delay(25);
    }
    s_usb_done_button = {0, 0, 0, 0};

    Serial.println("[BOOT_GUI] Exiting USB Disk screen");
}

// ============================================================================
// Preboot serial control (executed only on the main / emulation task)
// ============================================================================

enum SettingType { PathSetting, StringSetting, BoolSetting, RAMSetting, RotationSetting };
struct BootSetting { const char *key; SettingType type; void *value; size_t capacity; };
static BootSetting control_settings[] = {
    {"disk", PathSetting, selected_disk_path, sizeof(selected_disk_path)},
    {"cdrom", PathSetting, selected_cdrom_path, sizeof(selected_cdrom_path)},
    {"extfs", PathSetting, selected_extfs_path, sizeof(selected_extfs_path)},
    {"ramsize", RAMSetting, &selected_ram_mb, 0},
    {"skip_gui", BoolSetting, &skip_gui, 0},
    {"wifi_ssid", StringSetting, wifi_ssid, 33},
    {"wifi_pass", StringSetting, wifi_password, sizeof(wifi_password)},
    {"wifi_auto", BoolSetting, &wifi_auto_connect, 0},
    {"audio", BoolSetting, &audio_enabled, 0},
    {"boot_from_cd", BoolSetting, &boot_from_cd, 0},
    {"rotation", RotationSetting, &rotation_degrees, 0},
    {"power_manager", BoolSetting, &power_manager_enabled, 0},
};

static String controlHex(const char *value)
{
    if (!value[0]) return "-";
    static const char digits[] = "0123456789abcdef";
    String hex;
    hex.reserve(strlen(value) * 2);
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        hex += digits[*p >> 4]; hex += digits[*p & 15];
    }
    return hex;
}

static void controlSyncSelections(void)
{
    disk_selection_index = BootSelectDisk(disk_files, selected_disk_path, sizeof(selected_disk_path));
    cdrom_selection_index = extfs_selection_index = 0;
    for (size_t i = 0; i < cdrom_files.size(); ++i)
        if (cdrom_files[i] == selected_cdrom_path) cdrom_selection_index = i + 1;
    for (size_t i = 0; i < extfs_folders.size(); ++i)
        if (extfs_folders[i] == selected_extfs_path) extfs_selection_index = i + 1;
}

static bool controlPathValid(const char *key, const char *path)
{
    if (!path[0]) return strcmp(key, "disk") != 0;
    if (path[0] != '/' || strstr(path, "/../") || strstr(path, "/./") ||
        strstr(path, "//") || String(path).endsWith("/..") || String(path).endsWith("/.")) return false;
    if (!strcmp(key, "disk") && !(hasExtension(path, ".dsk") || hasExtension(path, ".img") || hasExtension(path, ".hfv"))) return false;
    if (!strcmp(key, "cdrom") && !(hasExtension(path, ".iso") || hasExtension(path, ".cdr") || hasExtension(path, ".toast"))) return false;
    File file = SD_FS.open(path, FILE_READ);
    if (!file) return false;
    bool valid = !strcmp(key, "extfs") ? file.isDirectory() : !file.isDirectory();
    file.close();
    return valid;
}

/* Named touch targets on the current screen, in canvas coordinates, so
 * automation can drive the UI without hard-coded positions. */
static bool controlHitRect(const char *name, UI::Rect *out)
{
    const int w = UI::Width(), h = UI::Height();
    if (!strcmp(control_screen, "starter")) {
        static const char *const names[Screens::kStarterCount] = {"system8", "system7", "settings", "wifi", "action"};
        const auto layout = Screens::LayoutStarter(w, h);
        for (int i = 0; i < Screens::kStarterCount; ++i)
            if (!strcmp(name, names[i])) { *out = layout.controls[i]; return true; }
    } else if (!strcmp(control_screen, "settings")) {
        static const char *const names[Screens::kSettingsControlCount] = {
            "disk", "cdrom", "extfs", "ram4", "ram8", "ram12", "ram16",
            "sound", "bootcd", "rotate", "wifi", "usb", "start"};
        const Screens::SettingsLayout L = Screens::LayoutSettings(w, h, UsbMsc_IsSupported());
        for (int i = 0; i < Screens::kSettingsControlCount; ++i)
            if (!strcmp(name, names[i])) { *out = L.controls[i]; return out->w > 0; }
    } else if (!strcmp(control_screen, "wifi")) {
        static const char *const names[Screens::kWiFiControlCount] = {
            "networks", "password", "scan", "back", "join"};
        const Screens::WiFiLayout L = Screens::LayoutWiFi(w, h);
        for (int i = 0; i < Screens::kWiFiControlCount; ++i)
            if (!strcmp(name, names[i])) { *out = L.controls[i]; return true; }
    } else if (!strcmp(control_screen, "keyboard")) {
        const Screens::KeyboardLayout L = Screens::LayoutKeyboard(w, h);
        if (!strcmp(name, "ok")) { *out = L.ok; return true; }
        if (!strcmp(name, "cancel")) { *out = L.cancel; return true; }
        int code = Screens::kKeyNone;
        if (!strcmp(name, "shift")) code = Screens::kKeyShift;
        else if (!strcmp(name, "delete")) code = Screens::kKeyDelete;
        else if (!strcmp(name, "space")) code = ' ';
        else if (!strncmp(name, "key:", 4) && name[4] && !name[5]) code = name[4];
        for (const auto &key : L.keys)
            if (code != Screens::kKeyNone && (key.normal == code || key.shifted == code)) {
                *out = key.r;
                return true;
            }
    } else if (!strcmp(control_screen, "usb") && !strcmp(name, "done")) {
        *out = s_usb_done_button;
        return out->w > 0;
    }
    return false;
}

static String controlExecute(char *command)
{
    if (!strcmp(command, "STATUS")) {
        return String("OK BOOT STATUS ") + control_phase + " " + control_screen +
            " " + (control_restart_at ? "1" : "0");
    }
    if (!strcmp(command, "UPDATE STATUS")) return FirmwareUpdateStatus();
    if (!strcmp(command, "UPDATE CANCEL")) {
        if (strcmp(control_phase, "preboot") || strcmp(control_screen, "settings") || control_restart_at)
            return "ERR BOOT settings_screen_required";
        return FirmwareUpdateCancel();
    }
    if (FirmwareUpdateBusy()) return "ERR BOOT update_busy";
    if (!strcmp(command, "ENTER")) {
        if (!strcmp(control_screen, "usb")) return "ERR BOOT usb_disk_active";
        if (!strcmp(control_phase, "preboot")) {
            if (StarterDownloadStatus().state == StarterState::Working) return "ERR BOOT download_active";
            control_leave_starter = true;
            control_hold = true;
            control_leave_wifi = true;
        } else if (!strcmp(control_phase, "emulator") || !strcmp(control_phase, "stopped")) {
            if (!control_restart_at) control_restart_at = millis() + 1200;
        } else return "ERR BOOT not_ready";
        return "OK BOOT ENTER";
    }
    if (!gui_initialized) return "ERR BOOT not_ready";
    if (!strncmp(command, "HIT ", 4)) {
        UI::Rect r;
        if (!controlHitRect(command + 4, &r)) return "ERR BOOT unknown_control";
        return String("OK BOOT HIT ") + (command + 4) + " " + r.x + " " + r.y + " " + r.w + " " + r.h;
    }
    if (!strcmp(control_screen, "usb")) return "ERR BOOT usb_disk_active";
    if (!strncmp(command, "GET ", 4)) {
        const char *key = command + 4;
        if (!strcmp(key, "wifi_password_set")) {
            return String("OK BOOT VALUE wifi_password_set ") + controlHex(wifi_password[0] ? "true" : "false");
        }
        for (const auto &setting : control_settings) {
            if (strcmp(key, setting.key)) continue;
            if (!strcmp(key, "wifi_pass")) return "ERR BOOT password_write_only";
            String value;
            if (setting.type == BoolSetting) value = *(bool *)setting.value ? "true" : "false";
            else if (setting.type == RAMSetting || setting.type == RotationSetting) value = *(int *)setting.value;
            else value = (char *)setting.value;
            return String("OK BOOT VALUE ") + key + " " + controlHex(value.c_str());
        }
        return "ERR BOOT unknown_setting";
    }
    if (strcmp(control_phase, "preboot")) return "ERR BOOT enter_preboot_first";
    if (control_restart_at || control_boot_requested) return "ERR BOOT transition_pending";
    if (!strncmp(command, "STARTER ", 8)) {
        const char *action = command + 8;
        const auto info = StarterDownloadStatus();
        if (!strcmp(action, "STATUS")) {
            static const char *const states[] = {"idle", "working", "done", "failed", "cancelled"};
            return String("OK BOOT STARTER ") + states[(unsigned)info.state] + " " + info.completed + " " +
                info.total + " " + controlHex(info.state == StarterState::Idle ? "No installation started." : info.message);
        }
        if (!strcmp(action, "SHOW") && !strcmp(control_screen, "settings")) {
            starter_requested = true;
            return "OK BOOT STARTER SHOW";
        }
        if (strcmp(control_screen, "starter")) return "ERR BOOT starter_screen_required";
        if (!strcmp(action, "SHOW")) return "OK BOOT STARTER SHOW";
        if (!strcmp(action, "CANCEL") && info.state == StarterState::Working) {
            StarterDownloadCancel(); starter_cancel_pending = true;
            return "OK BOOT STARTER CANCEL";
        }
        if (info.state == StarterState::Working) return "ERR BOOT download_active";
        if (!strcmp(action, "7") || !strcmp(action, "8")) {
            return startStarterDownload(*action == '7') ? "OK BOOT STARTER DOWNLOAD" : "ERR BOOT download_unavailable";
        }
        return "ERR BOOT invalid_starter_action";
    }
    if (!strcmp(control_screen, "starter") && !strcmp(command, "START"))
        return startInstalledSystem() ? "OK BOOT START" : "ERR BOOT download_not_ready";
    if (strcmp(control_screen, "settings")) return "ERR BOOT settings_screen_required";
    if (!strncmp(command, "UPDATE START ", 13))
        return FirmwareUpdateStart(command + 13);
    if (!strcmp(command, "UPDATE REBOOT")) {
        if (!FirmwareUpdateReady()) return "ERR BOOT no_verified_update";
        control_restart_at = millis() + 1200;
        return "OK BOOT UPDATE REBOOT";
    }
    if (!strncmp(command, "SET ", 4)) {
        char *key = command + 4;
        char *hex = strchr(key, ' ');
        if (!hex) return "ERR BOOT invalid_set";
        *hex++ = 0;
        char value[BOOT_GUI_MAX_PATH];
        if (!BootControlDecode(hex, value, sizeof(value))) return "ERR BOOT invalid_value";
        for (const auto &setting : control_settings) {
            if (strcmp(key, setting.key)) continue;
            if (setting.type == BoolSetting) {
                bool next;
                if (!BootControlBool(value, &next)) return "ERR BOOT invalid_boolean";
                *(bool *)setting.value = next;
            } else if (setting.type == RAMSetting || setting.type == RotationSetting) {
                int next;
                if (!BootControlChoice(value, setting.type == RAMSetting, &next)) return "ERR BOOT invalid_choice";
                *(int *)setting.value = next;
            } else {
                if (strlen(value) >= setting.capacity) return "ERR BOOT value_too_long";
                if (setting.type == PathSetting && !controlPathValid(key, value)) return "ERR BOOT invalid_path";
                strcpy((char *)setting.value, value);
                controlSyncSelections();
            }
            if (!strncmp(key, "wifi_", 5)) control_wifi_changed = true;
            return String("OK BOOT SET ") + key;
        }
        return "ERR BOOT unknown_setting";
    }
    if (!strncmp(command, "LIST ", 5)) {
        char *kind = command + 5;
        char *index_text = strchr(kind, ' ');
        if (!index_text) return "ERR BOOT invalid_list";
        *index_text++ = 0;
        char *end;
        long index = strtol(index_text, &end, 10);
        if (!index_text[0] || *end || index < 0 || index >= BOOT_GUI_MAX_FILES) return "ERR BOOT invalid_index";
        const std::vector<std::string> *items = !strcmp(kind, "disk") ? &disk_files :
            !strcmp(kind, "cdrom") ? &cdrom_files : !strcmp(kind, "extfs") ? &extfs_folders : nullptr;
        if (!items) return "ERR BOOT invalid_list";
        return String("OK BOOT ITEM ") + (unsigned int)items->size() + " " +
            controlHex((size_t)index < items->size() ? (*items)[index].c_str() : "");
    }
    if (!strcmp(command, "RESCAN")) {
        scanDiskFiles(); scanCDROMFiles(); scanExtFSFolders();
        return "OK BOOT RESCAN";
    }
    if (!strcmp(command, "RELOAD")) {
        if (!SD_FS.exists(SETTINGS_FILE)) return "ERR BOOT no_saved_settings";
        selected_disk_path[0] = selected_cdrom_path[0] = selected_extfs_path[0] = 0;
        wifi_ssid[0] = wifi_password[0] = 0;
        selected_ram_mb = kBootRamDefaultMB; rotation_degrees = 180;
        skip_gui = wifi_auto_connect = boot_from_cd = false; audio_enabled = true;
        power_manager_enabled = true;
        loadSettings(); controlSyncSelections();
        control_wifi_changed = true;
        return "OK BOOT RELOAD";
    }
    if (!strcmp(command, "SAVE")) return saveSettings() ? "OK BOOT SAVE" : "ERR BOOT save_failed";
    if (!strcmp(command, "START")) {
        if (!bootMediaReady() || (!boot_from_cd && !controlPathValid("disk", selected_disk_path)) ||
            !controlPathValid("cdrom", selected_cdrom_path) ||
            !controlPathValid("extfs", selected_extfs_path) ||
            (boot_from_cd && !selected_cdrom_path[0])) return "ERR BOOT invalid_boot_media";
        if (!saveSettings()) return "ERR BOOT save_failed";
        control_boot_requested = true;
        return "OK BOOT START";
    }
    if (!strcmp(command, "WIFI CONNECT")) {
        if (!wifi_ssid[0]) return "ERR BOOT wifi_ssid_required";
        initWiFi();
        WiFi.disconnect(false);
        WiFi.begin(wifi_ssid, wifi_password);
        control_wifi_changed = false;
        return "OK BOOT WIFI CONNECT";
    }
    if (!strcmp(command, "WIFI DISCONNECT")) {
        if (wifi_initialized) WiFi.disconnect(false);
        return "OK BOOT WIFI DISCONNECT";
    }
    return "ERR BOOT unknown_command";
}

bool BootGUI_ControlRequest(const char *command, char *reply, size_t capacity)
{
    if (!control_requests || !control_replies || strlen(command) >= sizeof(BootControlRequest::command)) return false;
    static uint32_t next_id = 0; // Only the serial task calls this API.
    BootControlRequest request = {};
    request.id = ++next_id;
    request.expires = millis() + 3000;
    strcpy(request.command, command);
    if (xQueueSend(control_requests, &request, 0) != pdTRUE) return false;
    __atomic_store_n(&boot_control_pending, true, __ATOMIC_RELEASE);
    BootControlReply result;
    uint32_t deadline = millis() + 4000;
    while ((int32_t)(deadline - millis()) > 0) {
        if (xQueueReceive(control_replies, &result, pdMS_TO_TICKS(50)) == pdTRUE && result.id == request.id) {
            snprintf(reply, capacity, "%s", result.text);
            return true;
        }
    }
    return false;
}

void BootGUI_ControlPoll(void)
{
    if (!control_requests) return;
    __atomic_store_n(&boot_control_pending, control_restart_at != 0, __ATOMIC_RELEASE);
    BootControlRequest request;
    if (xQueueReceive(control_requests, &request, 0) != pdTRUE) return;
    String text = (int32_t)(millis() - request.expires) >= 0 ?
        String("ERR BOOT request_expired") : controlExecute(request.command);
    if (control_restart_at) __atomic_store_n(&boot_control_pending, true, __ATOMIC_RELEASE);
    BootControlReply reply = {};
    reply.id = request.id;
    snprintf(reply.text, sizeof(reply.text), "%s", text.c_str());
    xQueueOverwrite(control_replies, &reply);
}

void BootGUI_ControlSetPhase(const char *phase) { control_phase = phase; }
bool BootGUI_StartInSettings(void) { return control_hold || !bootMediaReady(); }
bool BootGUI_RestartPending(void)
{
    return control_restart_at && (int32_t)(millis() - control_restart_at) >= 0;
}
void BootGUI_Restart(void)
{
    control_boot_cookie = CONTROL_BOOT_COOKIE;
    esp_restart(); // Called on the main task; shutdown hook drains SD writes.
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

    // MacSplash normally created the canvas already; this covers the rest.
    if (!UI::Begin()) {
        Serial.println("[BOOT_GUI] ERROR: no memory for the preboot canvas");
        return false;
    }
    Serial.printf("[BOOT_GUI] Canvas %dx%d, shown at %dx%d\n", UI::Width(), UI::Height(),
                  BoardDisplay_Width(), BoardDisplay_Height());

    // Load saved settings
    loadSettings();

    // Scan for disk files
    scanDiskFiles();
    scanCDROMFiles();
    scanExtFSFolders();

    // Waveshare carries an embedded radio update. Query its ESP-Hosted
    // firmware version before any autoconnect attempts. If it's out
    // of sync with the linked host library, stream the embedded
    // `esp32c6-v<X.Y.Z>.bin` blob into the slave and reboot so WiFi
    // comes up working. Tab5 uses its factory ESP-Hosted radio firmware.
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

    control_requests = xQueueCreate(1, sizeof(BootControlRequest));
    control_replies = xQueueCreate(1, sizeof(BootControlReply));
    if (!control_requests || !control_replies) {
        if (control_requests) vQueueDelete(control_requests);
        if (control_replies) vQueueDelete(control_replies);
        control_requests = control_replies = nullptr;
        Serial.println("[BOOT_GUI] WARNING: Serial settings queues unavailable");
    }
    control_hold = esp_reset_reason() == ESP_RST_SW && control_boot_cookie == CONTROL_BOOT_COOKIE;
    control_boot_cookie = 0; // One boot only; ordinary power cycles keep their behavior.
    control_phase = "preboot";
    gui_initialized = true;
    Serial.println("[BOOT_GUI] Initialization complete");

    return true;
}

// Wait up to ten seconds for an auto-connect to finish, servicing serial
// control meanwhile. Stops early once the network is missing or refused.
static void waitForWiFiAutoConnect(void)
{
    uint32_t start = millis();
    while (millis() - start < 10000) {
        wl_status_t status = WiFi.status();
        if (status == WL_CONNECTED || status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) break;
        BootGUI_ControlPoll();
        delay(20);
    }
}

// Shared WiFi cleanup path - used at the end of either settings or the
// silent (no-tap) boot flow. Leaves WiFi up if the user configured it and
// it actually connected; otherwise powers it down to free runtime state.
static void bootGuiCleanupWifi(void)
{
    // Serial edits take effect for this boot as well as the next power cycle.
    // Keep servicing status while a changed auto-connect configuration settles.
    if (control_wifi_changed) {
        if (wifi_initialized) WiFi.disconnect(false);
        if (BOOTGUI_ENABLE_WIFI_AUTOCONNECT && wifi_auto_connect && wifi_ssid[0]) {
            initWiFi();
            WiFi.begin(wifi_ssid, wifi_password);
            waitForWiFiAutoConnect();
        }
        control_wifi_changed = false;
    } else if (wifi_initialized && BOOTGUI_ENABLE_WIFI_AUTOCONNECT && wifi_auto_connect && wifi_ssid[0]) {
        // The power-on auto-connect runs in the background and is usually
        // still associating when the splash starts the emulator. The
        // emulator's network driver only checks once, at startup, so give
        // it the same time as above instead of dropping the association.
        waitForWiFiAutoConnect();
    }
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

    // This entry point follows an explicit splash tap or crash recovery.
    // A saved skip_gui flag must never lock the user out of settings.

    Serial.println("[BOOT_GUI] Running settings screen...");
    runSettingsScreen();

    // Stop the touch task before returning to emulator (emulator has its own input task)
    control_screen = "none";
    control_phase = "starting";
    stopTouchTask();
    bootGuiCleanupWifi();

    Serial.println("[BOOT_GUI] Settings complete, proceeding to emulator");
}

void BootGUI_FinishWithoutUI(void)
{
    Serial.println("[BOOT_GUI] Finishing without settings UI");
    control_screen = "none";
    control_phase = "starting";
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

bool BootGUI_InjectTouch(int x, int y, bool pressed)
{
    if (!touch_task_running) return false;
    portENTER_CRITICAL(&touch_spinlock);
    touch_injected = true;
    touch_injected_pressed = pressed;
    touch_injected_x = (int16_t)x;
    touch_injected_y = (int16_t)y;
    portEXIT_CRITICAL(&touch_spinlock);
    return true;
}

void BootGUI_EndInjectedTouch(void)
{
    portENTER_CRITICAL(&touch_spinlock);
    touch_injected = false;
    touch_injected_pressed = false;
    portEXIT_CRITICAL(&touch_spinlock);
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

bool BootGUI_GetPowerManagerEnabled(void)
{
    return power_manager_enabled;
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
