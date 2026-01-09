/*
 *  sys_esp32.cpp - System dependent routines for ESP32 (SD card I/O)
 *
 *  BasiliskII ESP32 Port
 */

#include "sysdeps.h"
#include "main.h"
#include "macos_util.h"
#include "prefs.h"
#include "sys.h"

#include <SD.h>
#include <FS.h>

#define DEBUG 1
#include "debug.h"

// File handle structure
struct file_handle {
    File file;
    bool is_open;
    bool read_only;
    bool is_floppy;
    bool is_cdrom;
    loff_t size;
    char path[256];
};

// Static flag for SD initialization
static bool sd_initialized = false;

/*
 *  Initialize SD card
 */
static bool init_sd_card(void)
{
    if (sd_initialized) {
        return true;
    }
    
    Serial.println("[SYS] SD card should already be initialized by main.cpp");
    sd_initialized = true;
    
    return true;
}

/*
 *  Initialization
 */
void SysInit(void)
{
    init_sd_card();
}

/*
 *  Deinitialization
 */
void SysExit(void)
{
    sd_initialized = false;
}

/*
 *  Mount first floppy disk
 */
void SysAddFloppyPrefs(void)
{
    // Add default floppy disk image paths
}

/*
 *  Mount first hard disk
 */
void SysAddDiskPrefs(void)
{
    // Add default hard disk image paths
}

/*
 *  Mount CD-ROM
 */
void SysAddCDROMPrefs(void)
{
    // No CD-ROM support
}

/*
 *  Add serial port preferences
 */
void SysAddSerialPrefs(void)
{
    // No serial port support
}

/*
 *  Open a file/device
 */
void *Sys_open(const char *name, bool read_only, bool is_cdrom)
{
    if (!name || strlen(name) == 0) {
        D(bug("[SYS] Sys_open: empty name\n"));
        return NULL;
    }
    
    D(bug("[SYS] Sys_open: %s (read_only=%d, cdrom=%d)\n", name, read_only, is_cdrom));
    
    // Allocate file handle
    file_handle *fh = new file_handle;
    if (!fh) {
        D(bug("[SYS] Sys_open: failed to allocate file handle\n"));
        return NULL;
    }
    
    memset(fh, 0, sizeof(file_handle));
    strncpy(fh->path, name, sizeof(fh->path) - 1);
    fh->read_only = read_only;
    fh->is_cdrom = is_cdrom;
    fh->is_floppy = (strstr(name, ".img") != NULL || strstr(name, ".IMG") != NULL);
    
    // Open file
    // ESP32 SD library quirks:
    // - FILE_READ = "r" (read only)
    // - FILE_WRITE = "w" (truncates file!)
    // - FILE_APPEND = "a" (append mode)
    // For disk images, we always open read-only first to get size, then use FILE_APPEND
    // which allows seeking and writing to existing files on ESP32
    
    // First, open in read mode to check file exists and get size
    File testFile = SD.open(name, FILE_READ);
    if (testFile) {
        fh->size = testFile.size();
        testFile.close();
        Serial.printf("[SYS] File exists: %s (size=%lld bytes)\n", name, (long long)fh->size);
    } else {
        fh->size = 0;
        Serial.printf("[SYS] File not found or empty: %s\n", name);
    }
    
    // Now open for actual access
    if (read_only) {
        fh->file = SD.open(name, FILE_READ);
    } else {
        // Use FILE_APPEND for read+write access to existing file
        // FILE_APPEND ("a+") allows reading and writing, and doesn't truncate
        fh->file = SD.open(name, FILE_APPEND);
        if (fh->file) {
            // Seek to beginning for random access
            fh->file.seek(0);
        }
    }
    
    if (!fh->file) {
        Serial.printf("[SYS] ERROR: Cannot open file: %s\n", name);
        delete fh;
        return NULL;
    }
    
    fh->is_open = true;
    
    Serial.printf("[SYS] Opened: %s (size=%lld bytes, ro=%d)\n", name, (long long)fh->size, read_only);
    
    return fh;
}

/*
 *  Close a file/device
 */
void Sys_close(void *arg)
{
    file_handle *fh = (file_handle *)arg;
    if (!fh) return;
    
    D(bug("[SYS] Sys_close: %s\n", fh->path));
    
    if (fh->is_open) {
        fh->file.close();
        fh->is_open = false;
    }
    
    delete fh;
}

/*
 *  Read from a file/device
 */
size_t Sys_read(void *arg, void *buffer, loff_t offset, size_t length)
{
    file_handle *fh = (file_handle *)arg;
    if (!fh || !fh->is_open || !buffer) {
        return 0;
    }
    
    // Seek to offset
    if (!fh->file.seek(offset)) {
        D(bug("[SYS] Sys_read: seek failed to offset %lld\n", (long long)offset));
        return 0;
    }
    
    // Read data
    size_t bytes_read = fh->file.read((uint8_t *)buffer, length);
    
    D(bug("[SYS] Sys_read: %s offset=%lld len=%d read=%d\n", 
          fh->path, (long long)offset, (int)length, (int)bytes_read));
    
    return bytes_read;
}

/*
 *  Write to a file/device
 */
size_t Sys_write(void *arg, void *buffer, loff_t offset, size_t length)
{
    file_handle *fh = (file_handle *)arg;
    if (!fh || !fh->is_open || !buffer || fh->read_only) {
        return 0;
    }
    
    // Seek to offset
    if (!fh->file.seek(offset)) {
        D(bug("[SYS] Sys_write: seek failed to offset %lld\n", (long long)offset));
        return 0;
    }
    
    // Write data
    size_t bytes_written = fh->file.write((uint8_t *)buffer, length);
    
    D(bug("[SYS] Sys_write: %s offset=%lld len=%d written=%d\n",
          fh->path, (long long)offset, (int)length, (int)bytes_written));
    
    return bytes_written;
}

/*
 *  Return size of file/device
 */
loff_t SysGetFileSize(void *arg)
{
    file_handle *fh = (file_handle *)arg;
    if (!fh || !fh->is_open) {
        return 0;
    }
    return fh->size;
}

/*
 *  Eject disk (no-op for SD card)
 */
void SysEject(void *arg)
{
    UNUSED(arg);
}

/*
 *  Format disk (not supported)
 */
bool SysFormat(void *arg)
{
    UNUSED(arg);
    return false;
}

/*
 *  Check if file/device is read-only
 */
bool SysIsReadOnly(void *arg)
{
    file_handle *fh = (file_handle *)arg;
    if (!fh) return true;
    return fh->read_only;
}

/*
 *  Check if a fixed disk (not removable)
 */
bool SysIsFixedDisk(void *arg)
{
    file_handle *fh = (file_handle *)arg;
    if (!fh) return true;
    return !fh->is_floppy && !fh->is_cdrom;
}

/*
 *  Check if a disk is inserted
 */
bool SysIsDiskInserted(void *arg)
{
    file_handle *fh = (file_handle *)arg;
    if (!fh) return false;
    return fh->is_open;
}

/*
 *  Prevent disk removal (no-op)
 */
void SysPreventRemoval(void *arg)
{
    UNUSED(arg);
}

/*
 *  Allow disk removal (no-op)
 */
void SysAllowRemoval(void *arg)
{
    UNUSED(arg);
}

/*
 *  CD-ROM functions (stubs - no CD-ROM support)
 */
bool SysCDReadTOC(void *arg, uint8 *toc)
{
    UNUSED(arg);
    UNUSED(toc);
    return false;
}

bool SysCDGetPosition(void *arg, uint8 *pos)
{
    UNUSED(arg);
    UNUSED(pos);
    return false;
}

bool SysCDPlay(void *arg, uint8 start_m, uint8 start_s, uint8 start_f, uint8 end_m, uint8 end_s, uint8 end_f)
{
    UNUSED(arg);
    UNUSED(start_m);
    UNUSED(start_s);
    UNUSED(start_f);
    UNUSED(end_m);
    UNUSED(end_s);
    UNUSED(end_f);
    return false;
}

bool SysCDPause(void *arg)
{
    UNUSED(arg);
    return false;
}

bool SysCDResume(void *arg)
{
    UNUSED(arg);
    return false;
}

bool SysCDStop(void *arg, uint8 lead_out_m, uint8 lead_out_s, uint8 lead_out_f)
{
    UNUSED(arg);
    UNUSED(lead_out_m);
    UNUSED(lead_out_s);
    UNUSED(lead_out_f);
    return false;
}

bool SysCDScan(void *arg, uint8 start_m, uint8 start_s, uint8 start_f, bool reverse)
{
    UNUSED(arg);
    UNUSED(start_m);
    UNUSED(start_s);
    UNUSED(start_f);
    UNUSED(reverse);
    return false;
}

void SysCDSetVolume(void *arg, uint8 left, uint8 right)
{
    UNUSED(arg);
    UNUSED(left);
    UNUSED(right);
}

void SysCDGetVolume(void *arg, uint8 &left, uint8 &right)
{
    UNUSED(arg);
    left = right = 0;
}
