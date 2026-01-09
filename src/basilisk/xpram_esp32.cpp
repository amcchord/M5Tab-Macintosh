/*
 *  xpram_esp32.cpp - XPRAM handling for ESP32
 *
 *  BasiliskII ESP32 Port
 */

#include "sysdeps.h"
#include "xpram.h"

#include <SD.h>

#define DEBUG 1
#include "debug.h"

// XPRAM file on SD card
const char XPRAM_FILE_PATH[] = "/BasiliskII_XPRAM";

// XPRAM buffer (defined in xpram.cpp)
extern uint8 XPRAM[XPRAM_SIZE];

/*
 *  Load XPRAM from SD card
 */
void LoadXPRAM(const char *vmdir)
{
    UNUSED(vmdir);
    
    Serial.println("[XPRAM] Loading XPRAM...");
    
    // Clear XPRAM first
    memset(XPRAM, 0, XPRAM_SIZE);
    
    // Try to load from SD card
    File f = SD.open(XPRAM_FILE_PATH, FILE_READ);
    if (f) {
        size_t bytes_read = f.read(XPRAM, XPRAM_SIZE);
        f.close();
        Serial.printf("[XPRAM] Loaded %d bytes from %s\n", bytes_read, XPRAM_FILE_PATH);
    } else {
        Serial.println("[XPRAM] No saved XPRAM found, using defaults");
    }
}

/*
 *  Save XPRAM to SD card
 */
void SaveXPRAM(void)
{
    Serial.println("[XPRAM] Saving XPRAM...");
    
    File f = SD.open(XPRAM_FILE_PATH, FILE_WRITE);
    if (f) {
        size_t bytes_written = f.write(XPRAM, XPRAM_SIZE);
        f.close();
        Serial.printf("[XPRAM] Saved %d bytes to %s\n", bytes_written, XPRAM_FILE_PATH);
    } else {
        Serial.printf("[XPRAM] ERROR: Cannot write to %s\n", XPRAM_FILE_PATH);
    }
}

/*
 *  Delete XPRAM file
 */
void ZapPRAM(void)
{
    Serial.println("[XPRAM] Zapping PRAM...");
    
    memset(XPRAM, 0, XPRAM_SIZE);
    SD.remove(XPRAM_FILE_PATH);
}
