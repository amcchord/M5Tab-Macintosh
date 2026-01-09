/*
 *  main_esp32.cpp - Main program entry point for ESP32
 *
 *  BasiliskII ESP32 Port
 */

#include "sysdeps.h"

#include <M5Unified.h>
#include <M5GFX.h>
#include <SD.h>

#include "cpu_emulation.h"
#include "sys.h"
#include "rom_patches.h"
#include "xpram.h"
#include "timer.h"
#include "video.h"
#include "prefs.h"
#include "prefs_items.h"
#include "main.h"
#include "macos_util.h"
#include "user_strings.h"

#define DEBUG 1
#include "debug.h"

// ROM file size limits
const uint32 ROM_MIN_SIZE = 64 * 1024;    // 64KB minimum
const uint32 ROM_MAX_SIZE = 1024 * 1024;  // 1MB maximum

// Memory pointers (declared in basilisk_glue.cpp)
extern uint32 RAMBaseMac;
extern uint8 *RAMBaseHost;
extern uint32 RAMSize;
extern uint32 ROMBaseMac;
extern uint8 *ROMBaseHost;
extern uint32 ROMSize;

// Frame buffer pointers (defined in basilisk_glue.cpp)
extern uint8 *MacFrameBaseHost;
extern uint32 MacFrameSize;
extern int MacFrameLayout;

// CPU and FPU type
int CPUType = 4;           // 68040
bool CPUIs68060 = false;
int FPUType = 1;           // 68881
bool TwentyFourBitAddressing = false;

// Interrupt flags
uint32 InterruptFlags = 0;

// Forward declaration
void basilisk_loop(void);

// CPU tick counter for timing (used by newcpu.cpp)
int32 emulated_ticks = 1000;
static int32 emulated_ticks_quantum = 1000;

/*
 *  CPU tick check - called periodically during emulation
 */
void cpu_do_check_ticks(void)
{
    // Call basilisk_loop to handle periodic tasks
    basilisk_loop();
    
    // Reset tick counter
    emulated_ticks = emulated_ticks_quantum;
}

// Global emulator state
static bool emulator_running = false;
static uint32 last_tick_time = 0;
static uint32 last_second_time = 0;
static uint32 last_video_refresh = 0;

// Video refresh interval (ms)
#define VIDEO_REFRESH_INTERVAL 33  // ~30 FPS

// 60Hz tick interval (ms)
#define TICK_60HZ_INTERVAL 16  // ~60 Hz

/*
 *  Set/clear interrupt flags
 */
void SetInterruptFlag(uint32 flag)
{
    InterruptFlags |= flag;
}

void ClearInterruptFlag(uint32 flag)
{
    InterruptFlags &= ~flag;
}

/*
 *  Mutex functions (single-threaded, stubs)
 */
B2_mutex *B2_create_mutex(void)
{
    return new B2_mutex;
}

void B2_lock_mutex(B2_mutex *mutex)
{
    UNUSED(mutex);
}

void B2_unlock_mutex(B2_mutex *mutex)
{
    UNUSED(mutex);
}

void B2_delete_mutex(B2_mutex *mutex)
{
    delete mutex;
}

/*
 *  Flush code cache (no-op for interpreted emulation)
 */
void FlushCodeCache(void *start, uint32 size)
{
    UNUSED(start);
    UNUSED(size);
}

/*
 *  Display error alert
 */
void ErrorAlert(const char *text)
{
    Serial.printf("[ERROR] %s\n", text);
    
    // Also display on screen if possible
    M5.Display.fillScreen(TFT_RED);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 10);
    M5.Display.println("BasiliskII Error:");
    M5.Display.println(text);
}

/*
 *  Display warning alert
 */
void WarningAlert(const char *text)
{
    Serial.printf("[WARNING] %s\n", text);
}

/*
 *  Display choice alert (always returns true on ESP32)
 */
bool ChoiceAlert(const char *text, const char *pos, const char *neg)
{
    Serial.printf("[CHOICE] %s (%s/%s)\n", text, pos, neg);
    return true;
}

/*
 *  Quit emulator
 */
void QuitEmulator(void)
{
    Serial.println("[MAIN] QuitEmulator called");
    emulator_running = false;
}

/*
 *  Load ROM file from SD card
 */
static bool LoadROM(const char *rom_path)
{
    Serial.printf("[MAIN] Loading ROM from: %s\n", rom_path);
    
    File rom_file = SD.open(rom_path, FILE_READ);
    if (!rom_file) {
        Serial.printf("[MAIN] ERROR: Cannot open ROM file: %s\n", rom_path);
        return false;
    }
    
    size_t rom_size = rom_file.size();
    Serial.printf("[MAIN] ROM file size: %d bytes\n", rom_size);
    
    if (rom_size < ROM_MIN_SIZE || rom_size > ROM_MAX_SIZE) {
        Serial.printf("[MAIN] ERROR: Invalid ROM size (expected %d-%d bytes)\n", 
                      ROM_MIN_SIZE, ROM_MAX_SIZE);
        rom_file.close();
        return false;
    }
    
    // Round up to nearest 64KB
    ROMSize = (rom_size + 0xFFFF) & ~0xFFFF;
    
    // Allocate ROM buffer in PSRAM
    ROMBaseHost = (uint8 *)ps_malloc(ROMSize);
    if (!ROMBaseHost) {
        Serial.println("[MAIN] ERROR: Cannot allocate ROM buffer in PSRAM!");
        rom_file.close();
        return false;
    }
    
    // Clear buffer
    memset(ROMBaseHost, 0, ROMSize);
    
    // Read ROM file
    size_t bytes_read = rom_file.read(ROMBaseHost, rom_size);
    rom_file.close();
    
    if (bytes_read != rom_size) {
        Serial.printf("[MAIN] ERROR: ROM read failed (got %d, expected %d)\n", 
                      bytes_read, rom_size);
        free(ROMBaseHost);
        ROMBaseHost = NULL;
        return false;
    }
    
    Serial.printf("[MAIN] ROM loaded successfully at %p (%d bytes)\n", 
                  ROMBaseHost, ROMSize);
    
    // Print first 16 bytes for debugging
    Serial.print("[MAIN] ROM header: ");
    for (int i = 0; i < 16; i++) {
        Serial.printf("%02X ", ROMBaseHost[i]);
    }
    Serial.println();
    
    return true;
}

/*
 *  Allocate Mac RAM
 */
static bool AllocateRAM(void)
{
    // Get RAM size from preferences
    RAMSize = PrefsFindInt32("ramsize");
    if (RAMSize < 1024 * 1024) {
        RAMSize = 8 * 1024 * 1024;  // Default 8MB
    }
    
    Serial.printf("[MAIN] Allocating %d bytes for Mac RAM...\n", RAMSize);
    
    // Allocate RAM in PSRAM
    RAMBaseHost = (uint8 *)ps_malloc(RAMSize);
    if (!RAMBaseHost) {
        Serial.println("[MAIN] ERROR: Cannot allocate Mac RAM in PSRAM!");
        return false;
    }
    
    // Clear RAM
    memset(RAMBaseHost, 0, RAMSize);
    
    Serial.printf("[MAIN] Mac RAM allocated at %p (%d bytes)\n", RAMBaseHost, RAMSize);
    
    return true;
}

/*
 *  60Hz tick handler - called from main loop
 */
static void handle_60hz_tick(void)
{
    // Set 60Hz interrupt flag
    SetInterruptFlag(INTFLAG_60HZ);
    
    // Handle ADB (mouse/keyboard) updates
    // ADBInterrupt is defined in adb.h but we handle ADB directly
    SetInterruptFlag(INTFLAG_ADB);
    
    // Trigger interrupt
    TriggerInterrupt();
}

/*
 *  1Hz tick handler
 */
static void handle_1hz_tick(void)
{
    SetInterruptFlag(INTFLAG_1HZ);
    TriggerInterrupt();
}

/*
 *  Initialize emulator
 */
static bool InitEmulator(void)
{
    Serial.println("\n========================================");
    Serial.println("  BasiliskII ESP32 - Macintosh Emulator");
    Serial.println("========================================\n");
    
    // Print memory info
    Serial.printf("[MAIN] Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("[MAIN] Free PSRAM: %d bytes\n", ESP.getFreePsram());
    Serial.printf("[MAIN] Total PSRAM: %d bytes\n", ESP.getPsramSize());
    
    // Initialize preferences
    // PrefsInit expects references for argc/argv, but we don't have command line args
    int dummy_argc = 0;
    char *dummy_argv_data[] = { NULL };
    char **dummy_argv = dummy_argv_data;
    PrefsInit(NULL, dummy_argc, dummy_argv);
    
    // Load preferences (hardcoded for ESP32)
    LoadPrefs(NULL);
    
    // Initialize system I/O (SD card)
    SysInit();
    
    // Allocate Mac RAM
    if (!AllocateRAM()) {
        ErrorAlert("Failed to allocate Mac RAM");
        return false;
    }
    
    // Load ROM file
    const char *rom_path = PrefsFindString("rom");
    if (!rom_path) {
        rom_path = "/Q650.ROM";
    }
    
    if (!LoadROM(rom_path)) {
        ErrorAlert("Failed to load ROM file");
        return false;
    }
    
    // Initialize all emulator subsystems
    Serial.println("[MAIN] Calling InitAll()...");
    if (!InitAll(NULL)) {
        ErrorAlert("InitAll() failed");
        return false;
    }
    
    Serial.println("[MAIN] Emulator initialized successfully!");
    
    // Print memory status after init
    Serial.printf("[MAIN] Free heap after init: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("[MAIN] Free PSRAM after init: %d bytes\n", ESP.getFreePsram());
    
    return true;
}

/*
 *  Run emulator main loop
 */
static void RunEmulator(void)
{
    Serial.println("[MAIN] Starting 68k CPU emulation...");
    
    emulator_running = true;
    last_tick_time = millis();
    last_second_time = millis();
    last_video_refresh = millis();
    
    // Start the 68k CPU - this function runs the emulation loop
    // It will return when QuitEmulator() is called
    Start680x0();
    
    Serial.println("[MAIN] 68k CPU emulation ended");
}

/*
 *  Arduino setup function
 */
void basilisk_setup(void)
{
    // Note: M5.begin() and Serial should already be initialized by main.cpp
    
    Serial.println("[MAIN] BasiliskII setup starting...");
    
    // Initialize emulator
    if (!InitEmulator()) {
        Serial.println("[MAIN] Emulator initialization failed!");
        
        // Display error and halt
        while (1) {
            delay(1000);
        }
    }
    
    // Run emulator
    RunEmulator();
    
    // Cleanup
    ExitAll();
    SysExit();
    PrefsExit();
    
    Serial.println("[MAIN] BasiliskII shutdown complete");
}

/*
 *  Arduino loop function - called periodically during emulation
 *  This is called from the CPU emulator's main loop to handle periodic tasks
 */
void basilisk_loop(void)
{
    uint32 current_time = millis();
    
    // Handle 60Hz tick
    if (current_time - last_tick_time >= TICK_60HZ_INTERVAL) {
        last_tick_time = current_time;
        handle_60hz_tick();
    }
    
    // Handle 1Hz tick
    if (current_time - last_second_time >= 1000) {
        last_second_time = current_time;
        handle_1hz_tick();
    }
    
    // Video refresh
    if (current_time - last_video_refresh >= VIDEO_REFRESH_INTERVAL) {
        last_video_refresh = current_time;
        VideoRefresh();
    }
    
    // Yield to allow other tasks
    yield();
}

/*
 *  Check if emulator is running
 */
bool basilisk_is_running(void)
{
    return emulator_running;
}
