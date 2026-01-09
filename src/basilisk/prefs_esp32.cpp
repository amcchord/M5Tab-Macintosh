/*
 *  prefs_esp32.cpp - Preferences handling for ESP32
 *
 *  BasiliskII ESP32 Port
 */

#include "sysdeps.h"
#include "prefs.h"
#include "boot_gui.h"

#define DEBUG 0
#include "debug.h"

// Platform-specific preferences items
prefs_desc platform_prefs_items[] = {
    {NULL, TYPE_END, false, NULL}  // End marker
};

/*
 *  Load preferences from settings file
 */
void LoadPrefs(const char *vmdir)
{
    UNUSED(vmdir);
    
    Serial.println("[PREFS] Loading preferences...");
    
    // Set ROM file path
    PrefsReplaceString("rom", "/Q650.ROM");
    
    // Set model ID to Quadra 900 (14) for MacOS 8 compatibility
    // Quadra 650 is similar architecture
    PrefsReplaceInt32("modelid", 14);
    
    // Set CPU type to 68040
    PrefsReplaceInt32("cpu", 4);
    
    // Disable FPU (not implemented on ESP32)
    PrefsReplaceBool("fpu", false);
    
    // Get RAM size from Boot GUI selection
    uint32_t ram_size = BootGUI_GetRAMSize();
    if (ram_size == 0) {
        ram_size = 8 * 1024 * 1024;  // Default 8MB if GUI not initialized
    }
    PrefsReplaceInt32("ramsize", ram_size);
    Serial.printf("[PREFS] RAM: %d MB\n", ram_size / (1024 * 1024));
    
    // Set screen configuration
    PrefsReplaceString("screen", "win/640/480");
    
    // Get hard disk path from Boot GUI selection
    const char* disk_path = BootGUI_GetDiskPath();
    if (disk_path && strlen(disk_path) > 0) {
        PrefsReplaceString("disk", disk_path);
        Serial.printf("[PREFS] Disk: %s (read-write)\n", disk_path);
    } else {
        // Fallback to default if no disk selected
        PrefsReplaceString("disk", "/Macintosh8.dsk");
        Serial.println("[PREFS] Disk: /Macintosh8.dsk (default, read-write)");
    }
    
    // Disable sound (for now)
    PrefsReplaceBool("nosound", true);
    
    // Get CD-ROM path from Boot GUI selection
    const char* cdrom_path = BootGUI_GetCDROMPath();
    if (cdrom_path && strlen(cdrom_path) > 0) {
        PrefsReplaceBool("nocdrom", false);
        PrefsReplaceString("cdrom", cdrom_path);
        Serial.printf("[PREFS] CD-ROM: %s\n", cdrom_path);
    } else {
        PrefsReplaceBool("nocdrom", true);
        Serial.println("[PREFS] CD-ROM: None");
    }
    
    // No GUI
    PrefsReplaceBool("nogui", true);
    
    // Boot from first bootable volume
    PrefsReplaceInt32("bootdrive", 0);
    PrefsReplaceInt32("bootdriver", 0);
    
    // Frame skip (lower = smoother but slower)
    PrefsReplaceInt32("frameskip", 4);
    
    Serial.println("[PREFS] Preferences loaded");
    
    // Debug: Print loaded prefs
    D(bug("  ROM: %s\n", PrefsFindString("rom")));
    D(bug("  Model ID: %d\n", PrefsFindInt32("modelid")));
    D(bug("  CPU: %d\n", PrefsFindInt32("cpu")));
    D(bug("  RAM: %d bytes\n", PrefsFindInt32("ramsize")));
}

/*
 *  Save preferences to settings file (no-op on ESP32)
 */
void SavePrefs(void)
{
    // Preferences are hardcoded, no saving needed
}

/*
 *  Add default preferences items
 */
void AddPlatformPrefsDefaults(void)
{
    // Defaults are set in LoadPrefs
}
