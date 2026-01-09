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
 */
bool BootGUI_Init(void);

/*
 *  Run the boot GUI
 *  Shows countdown screen, then settings screen if user interacts
 *  Returns when user chooses to boot (either by countdown or Boot button)
 */
void BootGUI_Run(void);

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
 *  Get the selected RAM size in bytes
 *  Returns RAM size (e.g., 8 * 1024 * 1024 for 8MB)
 */
uint32_t BootGUI_GetRAMSize(void);

/*
 *  Get the selected RAM size in megabytes
 *  Returns RAM size in MB (4, 8, 12, or 16)
 */
int BootGUI_GetRAMSizeMB(void);

#endif // BOOT_GUI_H
