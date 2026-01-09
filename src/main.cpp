/**
 * @file main.cpp
 * BasiliskII ESP32 - Macintosh Emulator for M5Stack Tab5
 * 
 * This file initializes the hardware and launches the BasiliskII emulator.
 */

#include <Arduino.h>
#include <M5Unified.h>
#include <M5GFX.h>
#include <SD.h>

// Forward declarations for BasiliskII functions
extern void basilisk_setup(void);
extern void basilisk_loop(void);
extern bool basilisk_is_running(void);

// ============================================================================
// Display Functions
// ============================================================================

void showStartupScreen() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(2);
    
    int centerX = M5.Display.width() / 2;
    int centerY = M5.Display.height() / 2;
    
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.drawString("BasiliskII ESP32", centerX, centerY - 60);
    M5.Display.drawString("Macintosh Emulator", centerX, centerY - 20);
    
    M5.Display.setTextSize(1);
    M5.Display.drawString("Initializing...", centerX, centerY + 40);
}

void showErrorScreen(const char* error) {
    M5.Display.fillScreen(TFT_MAROON);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(2);
    
    int centerX = M5.Display.width() / 2;
    
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.drawString("ERROR", centerX, 100);
    M5.Display.setTextSize(1);
    M5.Display.drawString(error, centerX, 160);
}

// ============================================================================
// SD Card Initialization
// ============================================================================

bool initSDCard() {
    Serial.println("[MAIN] Initializing SD card...");
    
    // Try to initialize SD card using default pins
    if (!SD.begin()) {
        Serial.println("[MAIN] ERROR: SD card initialization failed!");
        return false;
    }
    
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("[MAIN] SD card initialized: %lluMB\n", cardSize);
    
    // Check for required files
    bool hasROM = SD.exists("/Q650.ROM");
    bool hasDisk = SD.exists("/Macintosh.dsk");
    bool hasFloppy = SD.exists("/DiskTools1.img");
    
    Serial.printf("[MAIN] Q650.ROM: %s\n", hasROM ? "found" : "MISSING");
    Serial.printf("[MAIN] Macintosh.dsk: %s\n", hasDisk ? "found" : "MISSING");
    Serial.printf("[MAIN] DiskTools1.img: %s\n", hasFloppy ? "found" : "MISSING");
    
    if (!hasROM) {
        Serial.println("[MAIN] ERROR: Q650.ROM not found on SD card!");
        return false;
    }
    
    return true;
}

// ============================================================================
// Setup
// ============================================================================

void setup() {
    // Initialize M5Stack Tab5
    auto cfg = M5.config();
    M5.begin(cfg);
    
    // Initialize serial
    Serial.begin(115200);
    delay(500);
    
    Serial.println("\n\n========================================");
    Serial.println("  BasiliskII ESP32 - Macintosh Emulator");
    Serial.println("  M5Stack Tab5 Edition");
    Serial.println("========================================\n");
    
    // Configure display orientation (landscape)
    M5.Display.setRotation(3);
    
    // Show startup screen
    showStartupScreen();
    
    // Print system info
    Serial.printf("[MAIN] Display: %dx%d\n", M5.Display.width(), M5.Display.height());
    Serial.printf("[MAIN] Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("[MAIN] Free PSRAM: %d bytes\n", ESP.getFreePsram());
    Serial.printf("[MAIN] Total PSRAM: %d bytes\n", ESP.getPsramSize());
    Serial.printf("[MAIN] CPU Freq: %d MHz\n", ESP.getCpuFreqMHz());
    
    // Initialize SD card
    if (!initSDCard()) {
        showErrorScreen("SD card or ROM file not found");
        Serial.println("[MAIN] Halting - SD card initialization failed");
        while (1) {
            delay(1000);
        }
    }
    
    // Launch BasiliskII emulator
    Serial.println("[MAIN] Starting BasiliskII emulator...");
    basilisk_setup();
    
    // If we get here, emulator has exited
    Serial.println("[MAIN] Emulator exited");
}

// ============================================================================
// Main Loop
// ============================================================================

void loop() {
    // Update M5Stack (handles touch, buttons)
    M5.update();
    
    // The emulator runs its own loop in basilisk_setup()
    // This loop is reached after emulator exits
    delay(100);
}
