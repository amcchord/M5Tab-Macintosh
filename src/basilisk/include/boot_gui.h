/*
 *  boot_gui.h - Pre-boot configuration GUI
 *
 *  BasiliskII ESP32 Port
 *
 *  Provides a classic Macintosh-style boot configuration screen
 *  allowing users to select disk images, CD-ROM, and RAM size.
 */

#ifndef BOOT_GUI_H
#define BOOT_GUI_H

#include <stdint.h>

// Maximum path length for file paths
#define BOOT_GUI_MAX_PATH 256

// Maximum number of files to list
#define BOOT_GUI_MAX_FILES 32

/*
 *  Initialize the boot GUI system
 *  Must be called after SD card is initialized
 *  Returns true on success
 *
 *  If the touch task is already running (for example because MacSplash
 *  started it earlier), BootGUI_Init leaves it alone instead of starting
 *  a second one.
 */
bool BootGUI_Init(void);

/*
 *  Run the full-screen settings UI. Returns when the user taps Boot
 *  (or the GUI-skip flag in /basilisk_settings.txt is set). Also stops
 *  the shared touch task and settles WiFi before returning.
 */
void BootGUI_RunSettingsOnly(void);

/*
 *  Clean up without showing the settings UI. Used when the pre-boot
 *  splash times out with no tap - we still need to stop the touch task
 *  (so the emulator's input task owns the touch panel) and settle any
 *  in-flight WiFi auto-connect the same way BootGUI_RunSettingsOnly
 *  would have.
 */
void BootGUI_FinishWithoutUI(void);

/* ------------------------------------------------------------------- */
/* Shared touch-task infrastructure. MacSplash reuses this so the same  */
/* 60 Hz polling task handles the pre-boot splash tap AND the settings  */
/* screen without being torn down and re-created in between.            */
/* ------------------------------------------------------------------- */

typedef struct {
    int  x;
    int  y;
    bool is_pressed;
    bool was_pressed;   /* rising edge since last poll */
    bool was_released;  /* falling edge since last poll */
} BootGUITouch;

bool BootGUI_StartTouchTask(void);
void BootGUI_StopTouchTask(void);

/*
 *  Peek the most recent touch event. Returns false if the task is not
 *  running or no event is available. Consuming an event clears its
 *  edge flags so callers see each press/release exactly once.
 */
bool BootGUI_PollTouch(BootGUITouch *out);

/*
 *  Get the selected hard disk path
 *  Returns pointer to static buffer with path (e.g., "/Macintosh8.dsk")
 *  Returns empty string if no disk selected
 */
const char* BootGUI_GetDiskPath(void);

/*
 *  Get the selected CD-ROM path
 *  Returns pointer to static buffer with path (e.g., "/MacOS81.iso")
 *  Returns empty string if no CD-ROM selected
 */
const char* BootGUI_GetCDROMPath(void);

/*
 *  Get the selected shared-folder path for ExtFS.
 *
 *  The returned string is SD-root-relative (e.g. "/Shared"). Callers that
 *  need a VFS path must prepend BOARD_SD_MOUNT_POINT ("/sd" on both Tab5
 *  and Waveshare with the v4.5 mount fix) themselves.
 *
 *  Returns an empty string if the user picked "None" / disabled ExtFS.
 */
const char* BootGUI_GetExtFSPath(void);

/*
 *  Get the selected RAM size in bytes
 *  Returns RAM size (e.g., 8 * 1024 * 1024 for 8MB)
 */
uint32_t BootGUI_GetRAMSize(void);

/*
 *  Get the selected RAM size in megabytes
 *  Returns RAM size in MB (4, 8, 12, or 16)
 */
int BootGUI_GetRAMSizeMB(void);

/*
 *  Get the saved WiFi SSID
 *  Returns pointer to static buffer with SSID
 *  Returns empty string if no WiFi configured
 */
const char* BootGUI_GetWiFiSSID(void);

/*
 *  Get the saved WiFi password
 *  Returns pointer to static buffer with password
 *  Returns empty string if no WiFi configured
 */
const char* BootGUI_GetWiFiPassword(void);

/*
 *  Check if WiFi auto-connect is enabled
 *  Returns true if WiFi should auto-connect on boot
 */
bool BootGUI_GetWiFiAutoConnect(void);

/*
 *  Check if audio is enabled
 *  Returns true if emulator audio should be enabled
 */
bool BootGUI_GetAudioEnabled(void);

/*
 *  Returns true if the user asked to boot from the selected CD-ROM
 *  ("Boot from CD" checkbox). Only meaningful when GetCDROMPath() is
 *  non-empty; LoadPrefs() ignores it otherwise.
 */
bool BootGUI_GetBootFromCD(void);

/*
 *  Returns the selected display rotation: 0 (default landscape) or
 *  180 (flipped). Used by the Tab5 board HAL to set the MiniGfx flip
 *  flag and the touch transform; the Waveshare HAL ignores it for
 *  now (the 10.1" panel orientation is fixed by the ribbon location).
 */
int BootGUI_GetRotation(void);

/*
 *  Check if WiFi is currently connected
 *  Returns true if WiFi is connected and has an IP address
 */
bool BootGUI_IsWiFiConnected(void);

/*
 *  Get the current WiFi IP address
 *  Returns IP address as a 32-bit value in host byte order
 *  Returns 0 if not connected
 */
uint32_t BootGUI_GetWiFiIP(void);

#endif // BOOT_GUI_H
