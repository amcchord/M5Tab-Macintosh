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
 *  - Settings persistence to SD card
 */

#include <Arduino.h>
#include <M5Unified.h>
#include <M5GFX.h>
#include <SD.h>
#include <vector>
#include <string>

#include "boot_gui.h"

// ============================================================================
// Classic Mac Color Palette
// ============================================================================

#define MAC_WHITE       0xFFFF
#define MAC_BLACK       0x0000
#define MAC_LIGHT_GRAY  0xC618  // #C0C0C0
#define MAC_DARK_GRAY   0x8410  // #808080
#define MAC_DESKTOP     0xA514  // Classic Mac desktop gray pattern base

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
// Happy Mac Icon (32x32 pixel art)
// ============================================================================

static const uint8_t HAPPY_MAC_ICON[] = {
    0x00, 0x0F, 0xF0, 0x00,
    0x00, 0x3F, 0xFC, 0x00,
    0x00, 0x7F, 0xFE, 0x00,
    0x00, 0xFF, 0xFF, 0x00,
    0x01, 0xFF, 0xFF, 0x80,
    0x03, 0xFF, 0xFF, 0xC0,
    0x07, 0xE0, 0x07, 0xE0,
    0x07, 0xC0, 0x03, 0xE0,
    0x0F, 0x9E, 0x79, 0xF0,
    0x0F, 0x9E, 0x79, 0xF0,
    0x0F, 0x80, 0x01, 0xF0,
    0x0F, 0x80, 0x01, 0xF0,
    0x0F, 0x8C, 0x31, 0xF0,
    0x0F, 0x87, 0xE1, 0xF0,
    0x07, 0xC0, 0x03, 0xE0,
    0x07, 0xE0, 0x07, 0xE0,
    0x03, 0xFF, 0xFF, 0xC0,
    0x01, 0xFF, 0xFF, 0x80,
    0x00, 0xFF, 0xFF, 0x00,
    0x00, 0x7F, 0xFE, 0x00,
    0x00, 0x3F, 0xFC, 0x00,
    0x00, 0x0F, 0xF0, 0x00,
    0x00, 0x07, 0xE0, 0x00,
    0x00, 0x1F, 0xF8, 0x00,
    0x00, 0x3F, 0xFC, 0x00,
    0x00, 0x7F, 0xFE, 0x00,
    0x00, 0x7F, 0xFE, 0x00,
    0x00, 0x7F, 0xFE, 0x00,
    0x00, 0x7F, 0xFE, 0x00,
    0x00, 0x3F, 0xFC, 0x00,
    0x00, 0x1F, 0xF8, 0x00,
    0x00, 0x07, 0xE0, 0x00
};

// ============================================================================
// Settings Storage
// ============================================================================

static char selected_disk_path[BOOT_GUI_MAX_PATH] = "";
static char selected_cdrom_path[BOOT_GUI_MAX_PATH] = "";
static int selected_ram_mb = 8;  // Default 8MB

static const char* SETTINGS_FILE = "/basilisk_settings.txt";

// ============================================================================
// File Lists
// ============================================================================

static std::vector<std::string> disk_files;
static std::vector<std::string> cdrom_files;

static int disk_selection_index = 0;
static int cdrom_selection_index = 0;  // 0 = None
static int disk_scroll_offset = 0;
static int cdrom_scroll_offset = 0;

// ============================================================================
// UI State
// ============================================================================

static M5Canvas* canvas = nullptr;
static bool gui_initialized = false;

// ============================================================================
// Forward Declarations
// ============================================================================

static void loadSettings(void);
static void saveSettings(void);
static void scanDiskFiles(void);
static void scanCDROMFiles(void);
static void drawDesktopPattern(void);
static void drawWindow(int x, int y, int w, int h, const char* title);
static void drawButton(int x, int y, int w, int h, const char* label, bool pressed);
static void drawListBox(int x, int y, int w, int h, const std::vector<std::string>& items, 
                        int selected, int scroll_offset, bool include_none);
static void drawRadioButton(int x, int y, const char* label, bool selected);
static void drawHappyMac(int x, int y, int scale);
static bool isPointInRect(int px, int py, int rx, int ry, int rw, int rh);
static void runCountdownScreen(void);
static void runSettingsScreen(void);

// ============================================================================
// Settings Load/Save
// ============================================================================

static void loadSettings(void)
{
    Serial.println("[BOOT_GUI] Loading settings...");
    
    File file = SD.open(SETTINGS_FILE, FILE_READ);
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
        } else if (key == "ramsize") {
            selected_ram_mb = value.toInt();
            if (selected_ram_mb != 4 && selected_ram_mb != 8 && 
                selected_ram_mb != 12 && selected_ram_mb != 16) {
                selected_ram_mb = 8;  // Default to 8MB if invalid
            }
            Serial.printf("[BOOT_GUI] Loaded RAM: %d MB\n", selected_ram_mb);
        }
    }
    
    file.close();
}

static void saveSettings(void)
{
    Serial.println("[BOOT_GUI] Saving settings...");
    
    File file = SD.open(SETTINGS_FILE, FILE_WRITE);
    if (!file) {
        Serial.println("[BOOT_GUI] ERROR: Cannot open settings file for writing");
        return;
    }
    
    file.printf("disk=%s\n", selected_disk_path);
    file.printf("cdrom=%s\n", selected_cdrom_path);
    file.printf("ramsize=%d\n", selected_ram_mb);
    
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
    
    File root = SD.open("/");
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
            if (hasExtension(name, ".dsk") || hasExtension(name, ".img")) {
                // Store with leading slash for full path
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
    
    File root = SD.open("/");
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
            if (hasExtension(name, ".iso")) {
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

// ============================================================================
// Drawing Functions - Desktop Pattern
// ============================================================================

static void drawDesktopPattern(void)
{
    // Classic Mac desktop gray pattern
    canvas->fillScreen(MAC_LIGHT_GRAY);
    
    // Draw subtle checkerboard pattern
    for (int y = 0; y < SCREEN_HEIGHT; y += 2) {
        for (int x = 0; x < SCREEN_WIDTH; x += 2) {
            if ((x + y) % 4 == 0) {
                canvas->drawPixel(x, y, MAC_DESKTOP);
            }
        }
    }
}

// ============================================================================
// Drawing Functions - Window
// ============================================================================

static void drawWindow(int x, int y, int w, int h, const char* title)
{
    // Drop shadow
    canvas->fillRect(x + 4, y + 4, w, h, MAC_DARK_GRAY);
    
    // Window background
    canvas->fillRect(x, y, w, h, MAC_WHITE);
    
    // Window border
    canvas->drawRect(x, y, w, h, MAC_BLACK);
    canvas->drawRect(x + 1, y + 1, w - 2, h - 2, MAC_BLACK);
    
    // Title bar background with horizontal stripes
    canvas->fillRect(x + 2, y + 2, w - 4, TITLE_BAR_HEIGHT, MAC_WHITE);
    for (int ty = y + 4; ty < y + TITLE_BAR_HEIGHT; ty += 2) {
        canvas->drawFastHLine(x + 2, ty, w - 4, MAC_BLACK);
    }
    
    // Title text background (white box in center of title bar)
    int title_width = strlen(title) * 12 + 16;
    int title_x = x + (w - title_width) / 2;
    canvas->fillRect(title_x, y + 2, title_width, TITLE_BAR_HEIGHT, MAC_WHITE);
    
    // Title text
    canvas->setTextColor(MAC_BLACK);
    canvas->setTextSize(2);
    canvas->setTextDatum(MC_DATUM);
    canvas->drawString(title, x + w / 2, y + TITLE_BAR_HEIGHT / 2 + 2);
    
    // Divider line below title bar
    canvas->drawFastHLine(x + 2, y + TITLE_BAR_HEIGHT + 2, w - 4, MAC_BLACK);
}

// ============================================================================
// Drawing Functions - Button
// ============================================================================

static void drawButton(int x, int y, int w, int h, const char* label, bool pressed)
{
    if (pressed) {
        // Pressed state - inverted
        canvas->fillRect(x, y, w, h, MAC_BLACK);
        canvas->setTextColor(MAC_WHITE);
    } else {
        // Normal state - 3D beveled
        canvas->fillRect(x, y, w, h, MAC_WHITE);
        
        // Top and left edges (light)
        canvas->drawFastHLine(x, y, w, MAC_WHITE);
        canvas->drawFastVLine(x, y, h, MAC_WHITE);
        
        // Bottom and right edges (dark)
        canvas->drawFastHLine(x, y + h - 1, w, MAC_BLACK);
        canvas->drawFastHLine(x + 1, y + h - 2, w - 2, MAC_DARK_GRAY);
        canvas->drawFastVLine(x + w - 1, y, h, MAC_BLACK);
        canvas->drawFastVLine(x + w - 2, y + 1, h - 2, MAC_DARK_GRAY);
        
        // Border
        canvas->drawRect(x, y, w, h, MAC_BLACK);
        
        canvas->setTextColor(MAC_BLACK);
    }
    
    // Button label - size based on button height
    int text_size = 2;
    if (h >= 60) {
        text_size = 3;
    }
    if (h >= 80) {
        text_size = 4;
    }
    canvas->setTextSize(text_size);
    canvas->setTextDatum(MC_DATUM);
    canvas->drawString(label, x + w / 2, y + h / 2);
}

// ============================================================================
// Drawing Functions - List Box
// ============================================================================

static void drawListBox(int x, int y, int w, int h, const std::vector<std::string>& items,
                        int selected, int scroll_offset, bool include_none)
{
    // Background
    canvas->fillRect(x, y, w, h, MAC_WHITE);
    
    // Thick border for visibility
    canvas->drawRect(x, y, w, h, MAC_BLACK);
    canvas->drawRect(x + 1, y + 1, w - 2, h - 2, MAC_BLACK);
    canvas->drawRect(x + 2, y + 2, w - 4, h - 4, MAC_BLACK);
    
    // Calculate visible items
    int visible_count = (h - 6) / LIST_ITEM_HEIGHT;
    int total_items = items.size();
    if (include_none) {
        total_items++;
    }
    
    // Draw items - larger text for touch screen
    canvas->setTextSize(2);
    canvas->setTextDatum(ML_DATUM);
    
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
        
        // Check if this item is selected
        if (item_index == selected) {
            // Selected item - inverted with padding
            canvas->fillRect(x + 3, item_y, w - 6, LIST_ITEM_HEIGHT, MAC_BLACK);
            canvas->setTextColor(MAC_WHITE);
        } else {
            canvas->setTextColor(MAC_BLACK);
        }
        
        // Draw text (truncate if too long)
        char truncated[32];
        strncpy(truncated, item_text, 28);
        truncated[28] = '\0';
        if (strlen(item_text) > 28) {
            strcat(truncated, "...");
        }
        
        canvas->drawString(truncated, x + 6, item_y + LIST_ITEM_HEIGHT / 2);
    }
    
    // Draw scroll indicators if needed
    if (scroll_offset > 0) {
        // Up arrow indicator
        canvas->fillTriangle(x + w - 12, y + 8, x + w - 8, y + 4, x + w - 4, y + 8, MAC_BLACK);
    }
    if (scroll_offset + visible_count < total_items) {
        // Down arrow indicator
        canvas->fillTriangle(x + w - 12, h + y - 8, x + w - 8, h + y - 4, x + w - 4, h + y - 8, MAC_BLACK);
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
    canvas->fillCircle(cx, cy, r, MAC_WHITE);
    
    // Outer circle border
    canvas->drawCircle(cx, cy, r, MAC_BLACK);
    canvas->drawCircle(cx, cy, r - 1, MAC_BLACK);
    
    // Fill center if selected
    if (selected) {
        canvas->fillCircle(cx, cy, r - 6, MAC_BLACK);
    }
    
    // Label - larger text
    canvas->setTextColor(MAC_BLACK);
    canvas->setTextSize(2);
    canvas->setTextDatum(ML_DATUM);
    canvas->drawString(label, x + RADIO_SIZE + 10, cy);
}

// ============================================================================
// Drawing Functions - Happy Mac Icon
// ============================================================================

static void drawHappyMac(int x, int y, int scale)
{
    for (int row = 0; row < 32; row++) {
        for (int col = 0; col < 32; col++) {
            int byte_index = row * 4 + col / 8;
            int bit_index = 7 - (col % 8);
            
            if (HAPPY_MAC_ICON[byte_index] & (1 << bit_index)) {
                if (scale == 1) {
                    canvas->drawPixel(x + col, y + row, MAC_BLACK);
                } else {
                    canvas->fillRect(x + col * scale, y + row * scale, scale, scale, MAC_BLACK);
                }
            }
        }
    }
}

// ============================================================================
// Hit Testing
// ============================================================================

static bool isPointInRect(int px, int py, int rx, int ry, int rw, int rh)
{
    return (px >= rx && px < rx + rw && py >= ry && py < ry + rh);
}

// ============================================================================
// Countdown Screen
// ============================================================================

static void runCountdownScreen(void)
{
    Serial.println("[BOOT_GUI] Showing countdown screen...");
    Serial.printf("[BOOT_GUI] Screen size: %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    
    int countdown = 3;
    uint32_t last_second = millis();
    
    // Button dimensions - HUGE and easy to tap, takes up bottom third of screen
    int btn_w = SCREEN_WIDTH - 100;
    int btn_h = 120;
    int btn_x = 50;
    int btn_y = SCREEN_HEIGHT - btn_h - 50;
    
    Serial.printf("[BOOT_GUI] Button rect: x=%d y=%d w=%d h=%d (bottom edge at %d)\n", 
                  btn_x, btn_y, btn_w, btn_h, btn_y + btn_h);
    
    bool button_pressed = false;
    bool button_touch_started = false;  // Track if touch started in button
    bool settings_requested = false;
    
    while (countdown > 0 && !settings_requested) {
        // Handle touch input FIRST (before drawing, so M5.update() is fresh)
        M5.update();
        auto touch = M5.Touch.getDetail();
        
        // Check for new touch start
        if (touch.wasPressed()) {
            Serial.printf("[BOOT_GUI] Touch START at (%d, %d)\n", touch.x, touch.y);
            bool in_button = isPointInRect(touch.x, touch.y, btn_x, btn_y, btn_w, btn_h);
            Serial.printf("[BOOT_GUI] In button: %s (btn_y=%d to %d)\n", 
                          in_button ? "YES" : "NO", btn_y, btn_y + btn_h);
            
            if (in_button) {
                button_touch_started = true;
                button_pressed = true;
                Serial.println("[BOOT_GUI] Button touch started!");
            }
        }
        
        // Check for touch release
        if (touch.wasReleased()) {
            Serial.println("[BOOT_GUI] Touch RELEASED");
            if (button_touch_started) {
                // Touch started in button and was released - trigger action
                settings_requested = true;
                Serial.println("[BOOT_GUI] Opening settings screen!");
            }
            button_touch_started = false;
            button_pressed = false;
        }
        
        // Update button visual state while held
        if (touch.isPressed() && button_touch_started) {
            button_pressed = isPointInRect(touch.x, touch.y, btn_x, btn_y, btn_w, btn_h);
        }
        
        // Draw screen - simple gray background
        canvas->fillScreen(MAC_LIGHT_GRAY);
        
        // Draw title
        canvas->setTextColor(MAC_BLACK);
        canvas->setTextSize(4);
        canvas->setTextDatum(MC_DATUM);
        canvas->drawString("BasiliskII", SCREEN_WIDTH / 2, 100);
        
        // Draw countdown text - large
        char countdown_text[32];
        sprintf(countdown_text, "Starting in %d...", countdown);
        canvas->setTextSize(4);
        canvas->drawString(countdown_text, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - 80);
        
        // Draw settings info
        canvas->setTextSize(2);
        if (strlen(selected_disk_path) > 0) {
            const char* disk_name = selected_disk_path;
            if (disk_name[0] == '/') {
                disk_name++;
            }
            char info[64];
            sprintf(info, "Disk: %s", disk_name);
            canvas->drawString(info, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
            
            sprintf(info, "RAM: %d MB", selected_ram_mb);
            canvas->drawString(info, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 + 40);
        }
        
        // Draw button - huge at bottom
        drawButton(btn_x, btn_y, btn_w, btn_h, "Change Settings", button_pressed);
        
        // Push to display
        canvas->pushSprite(0, 0);
        
        // Update countdown
        if (millis() - last_second >= 1000) {
            countdown--;
            last_second = millis();
        }
        
        delay(16);  // ~60 FPS
    }
    
    if (settings_requested) {
        runSettingsScreen();
    }
}

// ============================================================================
// Settings Screen
// ============================================================================

static void runSettingsScreen(void)
{
    Serial.println("[BOOT_GUI] Showing settings screen...");
    Serial.printf("[BOOT_GUI] Found %d disk files, %d CD-ROM files\n", 
                  (int)disk_files.size(), (int)cdrom_files.size());
    
    // Full-screen layout - no window, just content areas
    int content_x = SCREEN_MARGIN;
    int content_y = SCREEN_MARGIN + TITLE_BAR_HEIGHT;
    int content_w = SCREEN_WIDTH - SCREEN_MARGIN * 2;
    
    // List box dimensions - side by side, using most of screen width
    int list_gap = 30;
    int list_w = (content_w - list_gap) / 2;
    int list_h = LIST_ITEM_HEIGHT * LIST_MAX_VISIBLE + 4;
    int disk_list_x = content_x;
    int cdrom_list_x = content_x + list_w + list_gap;
    int list_y = content_y + 50;  // Space for labels
    
    // RAM radio buttons - below lists
    int ram_y = list_y + list_h + 30;
    int ram_x = content_x;
    
    // Boot button - BIG and at bottom of screen
    int boot_btn_w = 400;
    int boot_btn_h = 80;
    int boot_btn_x = (SCREEN_WIDTH - boot_btn_w) / 2;
    int boot_btn_y = SCREEN_HEIGHT - boot_btn_h - SCREEN_MARGIN;
    
    // Debug: Print layout info
    Serial.printf("[BOOT_GUI] Layout: list_y=%d, list_h=%d, item_height=%d\n", list_y, list_h, LIST_ITEM_HEIGHT);
    Serial.printf("[BOOT_GUI] Disk list: x=%d-%d, y=%d-%d\n", disk_list_x, disk_list_x + list_w, list_y, list_y + list_h);
    Serial.printf("[BOOT_GUI] CDROM list: x=%d-%d, y=%d-%d\n", cdrom_list_x, cdrom_list_x + list_w, list_y, list_y + list_h);
    Serial.printf("[BOOT_GUI] Boot btn: x=%d-%d, y=%d-%d\n", boot_btn_x, boot_btn_x + boot_btn_w, boot_btn_y, boot_btn_y + boot_btn_h);
    Serial.printf("[BOOT_GUI] RAM radios: x=%d, y=%d\n", ram_x, ram_y);
    
    // Calculate valid touch ranges for lists
    int disk_list_valid_h = disk_files.size() * LIST_ITEM_HEIGHT;
    int cdrom_list_valid_h = (cdrom_files.size() + 1) * LIST_ITEM_HEIGHT;  // +1 for None
    Serial.printf("[BOOT_GUI] Valid disk touch: y=%d to %d (%d items)\n", 
                  list_y, list_y + disk_list_valid_h, (int)disk_files.size());
    Serial.printf("[BOOT_GUI] Valid cdrom touch: y=%d to %d (%d items)\n", 
                  list_y, list_y + cdrom_list_valid_h, (int)cdrom_files.size() + 1);
    
    bool boot_pressed = false;
    bool boot_touch_started = false;
    bool should_boot = false;
    
    // Touch state - save position on press for use on release
    int touch_start_x = 0;
    int touch_start_y = 0;
    bool touch_in_disk_list = false;
    bool touch_in_cdrom_list = false;
    bool touch_in_boot_btn = false;
    
    while (!should_boot) {
        // Handle touch input FIRST (before drawing)
        M5.update();
        auto touch = M5.Touch.getDetail();
        
        // Detect new touch start - save position
        if (touch.wasPressed()) {
            touch_start_x = touch.x;
            touch_start_y = touch.y;
            touch_in_disk_list = isPointInRect(touch_start_x, touch_start_y, disk_list_x, list_y, list_w, list_h);
            touch_in_cdrom_list = isPointInRect(touch_start_x, touch_start_y, cdrom_list_x, list_y, list_w, list_h);
            touch_in_boot_btn = isPointInRect(touch_start_x, touch_start_y, boot_btn_x, boot_btn_y, boot_btn_w, boot_btn_h);
            
            if (touch_in_boot_btn) {
                boot_touch_started = true;
                boot_pressed = true;
            }
            
            Serial.printf("[BOOT_GUI] Touch start at (%d, %d) disk=%d cdrom=%d boot=%d\n", 
                          touch_start_x, touch_start_y, touch_in_disk_list, touch_in_cdrom_list, touch_in_boot_btn);
        }
        
        // Detect touch release - use saved position for hit testing
        if (touch.wasReleased()) {
            Serial.printf("[BOOT_GUI] Touch released, start was (%d, %d)\n", touch_start_x, touch_start_y);
            
            // Check Boot button
            if (boot_touch_started) {
                should_boot = true;
                Serial.println("[BOOT_GUI] Boot button pressed");
            }
            
            // Check disk list click (use saved start position)
            if (touch_in_disk_list) {
                int clicked_item = (touch_start_y - list_y - 2) / LIST_ITEM_HEIGHT + disk_scroll_offset;
                Serial.printf("[BOOT_GUI] Disk click: y=%d, list_y=%d, clicked_item=%d, num_files=%d\n", 
                              touch_start_y, list_y, clicked_item, (int)disk_files.size());
                if (clicked_item >= 0 && clicked_item < (int)disk_files.size()) {
                    disk_selection_index = clicked_item;
                    strncpy(selected_disk_path, disk_files[clicked_item].c_str(), BOOT_GUI_MAX_PATH - 1);
                    Serial.printf("[BOOT_GUI] Selected disk [%d]: %s\n", clicked_item, selected_disk_path);
                } else {
                    Serial.printf("[BOOT_GUI] Clicked empty area (item %d doesn't exist)\n", clicked_item);
                }
            }
            
            // Check CD-ROM list click (use saved start position)
            if (touch_in_cdrom_list) {
                int clicked_item = (touch_start_y - list_y - 2) / LIST_ITEM_HEIGHT + cdrom_scroll_offset;
                int total_items = cdrom_files.size() + 1;  // +1 for "None"
                Serial.printf("[BOOT_GUI] CD-ROM click: y=%d, list_y=%d, clicked_item=%d, total_items=%d\n", 
                              touch_start_y, list_y, clicked_item, total_items);
                if (clicked_item >= 0 && clicked_item < total_items) {
                    cdrom_selection_index = clicked_item;
                    if (clicked_item == 0) {
                        selected_cdrom_path[0] = '\0';
                        Serial.println("[BOOT_GUI] Selected CD-ROM: None");
                    } else {
                        strncpy(selected_cdrom_path, cdrom_files[clicked_item - 1].c_str(), BOOT_GUI_MAX_PATH - 1);
                        Serial.printf("[BOOT_GUI] Selected CD-ROM [%d]: %s\n", clicked_item, selected_cdrom_path);
                    }
                } else {
                    Serial.printf("[BOOT_GUI] Clicked empty area (item %d doesn't exist)\n", clicked_item);
                }
            }
            
            // Check RAM radio buttons (use saved start position)
            // Use same layout calculation as drawing
            int radio_start_x = ram_x + 120;
            int radio_gap = (SCREEN_WIDTH - radio_start_x - SCREEN_MARGIN) / 4;
            int radio_y_hit = ram_y;
            int radio_hit_w = radio_gap - 10;  // Hit area width
            int radio_hit_h = RADIO_SIZE + 20;  // Hit area height
            
            if (isPointInRect(touch_start_x, touch_start_y, radio_start_x, radio_y_hit, radio_hit_w, radio_hit_h)) {
                selected_ram_mb = 4;
                Serial.println("[BOOT_GUI] Selected RAM: 4 MB");
            } else if (isPointInRect(touch_start_x, touch_start_y, radio_start_x + radio_gap, radio_y_hit, radio_hit_w, radio_hit_h)) {
                selected_ram_mb = 8;
                Serial.println("[BOOT_GUI] Selected RAM: 8 MB");
            } else if (isPointInRect(touch_start_x, touch_start_y, radio_start_x + radio_gap * 2, radio_y_hit, radio_hit_w, radio_hit_h)) {
                selected_ram_mb = 12;
                Serial.println("[BOOT_GUI] Selected RAM: 12 MB");
            } else if (isPointInRect(touch_start_x, touch_start_y, radio_start_x + radio_gap * 3, radio_y_hit, radio_hit_w, radio_hit_h)) {
                selected_ram_mb = 16;
                Serial.println("[BOOT_GUI] Selected RAM: 16 MB");
            }
            
            // Reset touch state
            touch_in_disk_list = false;
            touch_in_cdrom_list = false;
            touch_in_boot_btn = false;
            boot_touch_started = false;
            boot_pressed = false;
        }
        
        // Update boot button visual while held
        if (touch.isPressed() && boot_touch_started) {
            boot_pressed = isPointInRect(touch.x, touch.y, boot_btn_x, boot_btn_y, boot_btn_w, boot_btn_h);
        }
        
        // Draw screen - simple gray background
        canvas->fillScreen(MAC_LIGHT_GRAY);
        
        // Draw title
        canvas->setTextColor(MAC_BLACK);
        canvas->setTextSize(3);
        canvas->setTextDatum(TC_DATUM);
        canvas->drawString("Boot Settings", SCREEN_WIDTH / 2, SCREEN_MARGIN);
        
        // Draw "Hard Disk:" label
        canvas->setTextSize(2);
        canvas->setTextDatum(TL_DATUM);
        canvas->drawString("Hard Disk:", disk_list_x, content_y);
        
        // Draw disk list
        drawListBox(disk_list_x, list_y, list_w, list_h, disk_files, 
                    disk_selection_index, disk_scroll_offset, false);
        
        // Draw "CD-ROM:" label
        canvas->drawString("CD-ROM:", cdrom_list_x, content_y);
        
        // Draw CD-ROM list
        drawListBox(cdrom_list_x, list_y, list_w, list_h, cdrom_files,
                    cdrom_selection_index, cdrom_scroll_offset, true);
        
        // Draw "Memory:" label - larger for touch screen
        canvas->setTextSize(2);
        canvas->drawString("Memory:", ram_x, ram_y + 10);
        
        // Draw RAM radio buttons - spread across screen for easy touch
        int radio_start_x = ram_x + 120;
        int radio_gap = (SCREEN_WIDTH - radio_start_x - SCREEN_MARGIN) / 4;
        drawRadioButton(radio_start_x, ram_y, "4 MB", selected_ram_mb == 4);
        drawRadioButton(radio_start_x + radio_gap, ram_y, "8 MB", selected_ram_mb == 8);
        drawRadioButton(radio_start_x + radio_gap * 2, ram_y, "12 MB", selected_ram_mb == 12);
        drawRadioButton(radio_start_x + radio_gap * 3, ram_y, "16 MB", selected_ram_mb == 16);
        
        // Draw Boot button
        drawButton(boot_btn_x, boot_btn_y, boot_btn_w, boot_btn_h, "Boot", boot_pressed);
        
        // Push to display
        canvas->pushSprite(0, 0);
        
        delay(16);  // ~60 FPS
    }
    
    // Save settings before booting
    saveSettings();
}

// ============================================================================
// Public API
// ============================================================================

bool BootGUI_Init(void)
{
    Serial.println("[BOOT_GUI] Initializing...");
    
    // Get display dimensions
    SCREEN_WIDTH = M5.Display.width();
    SCREEN_HEIGHT = M5.Display.height();
    Serial.printf("[BOOT_GUI] Display size: %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    
    // Create canvas for double-buffered rendering
    canvas = new M5Canvas(&M5.Display);
    if (!canvas) {
        Serial.println("[BOOT_GUI] ERROR: Failed to create canvas");
        return false;
    }
    
    canvas->setColorDepth(16);
    canvas->createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);
    
    // Load saved settings
    loadSettings();
    
    // Scan for disk files
    scanDiskFiles();
    scanCDROMFiles();
    
    // If no disk is selected but we found some, select the first one
    if (strlen(selected_disk_path) == 0 && disk_files.size() > 0) {
        strncpy(selected_disk_path, disk_files[0].c_str(), BOOT_GUI_MAX_PATH - 1);
        disk_selection_index = 0;
    }
    
    gui_initialized = true;
    Serial.println("[BOOT_GUI] Initialization complete");
    
    return true;
}

void BootGUI_Run(void)
{
    if (!gui_initialized) {
        Serial.println("[BOOT_GUI] ERROR: GUI not initialized");
        return;
    }
    
    Serial.println("[BOOT_GUI] Running boot GUI...");
    
    // Run countdown screen (may transition to settings screen)
    runCountdownScreen();
    
    // Cleanup canvas
    if (canvas) {
        canvas->deleteSprite();
        delete canvas;
        canvas = nullptr;
    }
    
    Serial.println("[BOOT_GUI] Boot GUI complete, proceeding to emulator");
}

const char* BootGUI_GetDiskPath(void)
{
    return selected_disk_path;
}

const char* BootGUI_GetCDROMPath(void)
{
    return selected_cdrom_path;
}

uint32_t BootGUI_GetRAMSize(void)
{
    return (uint32_t)selected_ram_mb * 1024 * 1024;
}

int BootGUI_GetRAMSizeMB(void)
{
    return selected_ram_mb;
}
