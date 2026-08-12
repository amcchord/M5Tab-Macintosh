/**
 * @file main.cpp
 * BasiliskII ESP32 - Macintosh Emulator
 *
 * Bring up the board + display, show the classic-Mac themed splash
 * (tiled BgTile background + centered Happy Mac). A tap during the first
 * 2 seconds opens the settings UI; otherwise we quietly transition into
 * the emulator handoff.
 */

#include <Arduino.h>

#include "board.h"
#include "board_config.h"
#include "board_display.h"
#include "board_sd.h"          /* pulls in SD_FS (SD on Tab5, SD_MMC on Waveshare) */

#include "boot_gui.h"
#include "automation.h"
#include "mac_splash.h"

#include "esp_system.h"        /* esp_register_shutdown_handler */

/* Forward declarations for BasiliskII functions */
extern void basilisk_setup(void);
extern void basilisk_loop(void);
extern bool basilisk_is_running(void);

/* Sys_flush_now lives in src/basilisk/sys_esp32.cpp; declared in sys.h
 * but pulling that header here drags in the BasiliskII type macros, so
 * forward-declare instead. ABI-compatible (extern "C" not needed - the
 * symbol is C++-mangled identically since it has C-style signature). */
extern void Sys_flush_now(void);

/* ESP-IDF shutdown hook: drain any dirty disk-image writes to the SD
 * card on programmatic reset (esp_restart), panic, or watchdog. Does
 * not catch a hard power pull, but does catch every other path that
 * goes through the IDF shutdown chain. */
static void on_system_shutdown(void)
{
    Sys_flush_now();
}

/* ============================================================================
 * Window during which a tap anywhere opens the configuration menu. The
 * splash is supposed to feel seamless, not like a boot menu, but the
 * window has to be long enough that a user who watches the Happy Mac
 * appear and decides to tap actually has time to react. With the
 * MacSplash::Begin() warmup + start-touch-task flow finishing in ~150 ms,
 * 4 seconds gives a comfortable reaction window without making "no
 * tap" boots feel sluggish.
 * ==========================================================================*/
static const uint32_t SPLASH_TAP_WINDOW_MS = 4000;

/* ============================================================================
 * SD card bring-up + ROM existence check
 * ==========================================================================*/

static bool initSDCard(void)
{
    Serial.println("[MAIN] Initializing SD card via board HAL...");
    if (!BoardSD_Init()) {
        Serial.println("[MAIN] ERROR: SD card initialization failed");
        Serial.println("[MAIN] Ensure SD card is inserted and formatted as FAT32");
        return false;
    }

    Serial.printf("[MAIN] SD card initialized: %llu MB\n",
                  (unsigned long long)BoardSD_CardSizeMB());

    /* SD_FS is SD on Tab5 (SPI) and SD_MMC on Waveshare (native SDMMC). */
    const bool hasROM   = SD_FS.exists("/Q650.ROM");
    const bool hasDisk  = SD_FS.exists("/Macintosh.dsk");
    const bool hasFloppy = SD_FS.exists("/DiskTools1.img");

    Serial.printf("[MAIN] Q650.ROM: %s\n",      hasROM    ? "found" : "MISSING");
    Serial.printf("[MAIN] Macintosh.dsk: %s\n", hasDisk   ? "found" : "MISSING");
    Serial.printf("[MAIN] DiskTools1.img: %s\n", hasFloppy ? "found" : "MISSING");

    if (!hasROM) {
        Serial.println("[MAIN] ERROR: Q650.ROM not found on SD card!");
        return false;
    }
    return true;
}

static void haltWith(const char *msg)
{
    Serial.printf("[MAIN] FATAL: %s\n", msg);
    MacSplash::ShowErrorOverlay(msg);
    while (true) { delay(1000); }
}

/* ============================================================================
 * Setup
 * ==========================================================================*/

void setup(void)
{
    Serial.begin(115200);
    delay(500);

    /* We cannot reach 400 MHz on this Waveshare P4 10.1 in practice.
     * pioarduino's "postv3" chip variant (which would unlock 400 MHz)
     * ships a bootloader built for silicon revision 3.01+, and our
     * chip (ESP-ROM "esp32p4-eco2") immediately panics with
     * "Illegal instruction" at the bootloader entry. Verified on
     * hardware: the stock pre-v3 prebuilt IDF is the only one that
     * runs, and it clamps the CPU clock to 360 MHz. Leave the PMU
     * alone - setCpuFrequencyMhz(400) would just log "failed" and
     * add noise to the boot log.                                     */

    Serial.println("\n\n========================================");
    Serial.println("  BasiliskII ESP32 - Macintosh Emulator");
    Serial.printf ("  Board: %s\n", BOARD_NAME);
    Serial.println("========================================\n");

    if (!Board_Init()) {
        Serial.println("[MAIN] ERROR: Board_Init failed - halting");
        while (true) { delay(1000); }
    }
    if (!BoardDisplay_Init()) {
        Serial.println("[MAIN] ERROR: BoardDisplay_Init failed - halting");
        while (true) { delay(1000); }
    }

    Serial.printf("[MAIN] Display: %dx%d\n", BoardDisplay_Width(), BoardDisplay_Height());
    Serial.printf("[MAIN] Free heap: %d bytes\n",     ESP.getFreeHeap());
    Serial.printf("[MAIN] Free PSRAM: %d bytes\n",    ESP.getFreePsram());
    Serial.printf("[MAIN] Total PSRAM: %d bytes\n",   ESP.getPsramSize());
    Serial.printf("[MAIN] CPU Freq: %d MHz\n",        ESP.getCpuFreqMHz());

    /* Paint the classic-Mac splash first thing, then listen for a tap. */
    MacSplash::Begin();
    const bool openSettings = MacSplash::WaitForTapOrTimeout(SPLASH_TAP_WINDOW_MS);

    /* SD comes up after the splash; practical init time fits comfortably
     * inside the 2s tap window, so the icon stays visible throughout. */
    if (!initSDCard()) {
        haltWith("SD card or ROM file not found");
    }

    /* Register the SD-flush shutdown hook as soon as the card is up.
     * Doing it after initSDCard() keeps the hook from firing on a
     * pre-mount halt (where there is nothing to flush yet) and means
     * any later esp_restart() / panic still drains pending writes. */
    esp_err_t hook_err = esp_register_shutdown_handler(&on_system_shutdown);
    if (hook_err != ESP_OK) {
        Serial.printf("[MAIN] WARN: shutdown hook register failed (0x%x)\n", hook_err);
    } else {
        Serial.println("[MAIN] SD flush shutdown hook registered");
    }

    // Subscribe before BootGUI starts its asynchronous WiFi association so
    // automation never has to poll the hosted C6 transport for link state.
    AutomationPrepareNetwork();
    if (!BootGUI_Init()) {
        haltWith("Boot GUI initialization failed");
    }

    if (openSettings) {
        BootGUI_RunSettingsOnly();
    } else {
        BootGUI_FinishWithoutUI();
    }

    /* Apply the user's rotation preference (boot GUI default is 180,
     * matching v4.0 behaviour). The HAL gates this internally on the
     * board: Tab5 flips the MiniGfx output and the touch transform,
     * Waveshare ignores the call since its panel orientation is fixed
     * by the ribbon. We do this before TransitionToEmulator so the
     * splash and the Mac framebuffer agree on which way is up. */
    bool flip = (BootGUI_GetRotation() == 180);
    BoardDisplay_SetFlip180(flip);

    MacSplash::TransitionToEmulator();

    Serial.println("[MAIN] Starting BasiliskII emulator...");
    basilisk_setup();

    Serial.println("[MAIN] Emulator exited");

    /* Final sync: emulator just stopped, no more writes are coming.
     * Push anything still dirty to the card before the user sees the
     * "safe to power off" splash so the next boot is clean even on a
     * hard power-cycle from this state. */
    Sys_flush_now();

    /* Paint a classic "it is now safe to switch off your computer"
     * screen so the user isn't left with the stale last Mac framebuffer
     * (which previously showed as a cyan smear). loop() below keeps the
     * device idle so nothing repaints over this. */
    MacSplash::ShowSafeToPowerOff();
}

/* ============================================================================
 * Main Loop
 * ==========================================================================*/

void loop(void)
{
    Board_Update();
    /* Emulator owns its own tasks after basilisk_setup(); we only get here
     * if it exits. */
    delay(100);
}
