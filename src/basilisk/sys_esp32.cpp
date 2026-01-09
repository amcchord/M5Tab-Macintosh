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

#define DEBUG 0
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
 *  
 *  For read-write access, we use "r+b" mode which opens an existing file
 *  for both reading and writing WITHOUT truncation.
 *  DO NOT use FILE_WRITE as it will TRUNCATE the file!
 */
void *Sys_open(const char *name, bool read_only, bool is_cdrom)
{
    if (!name || strlen(name) == 0) {
        Serial.println("[SYS] Sys_open: empty name");
        return NULL;
    }
    
    Serial.printf("[SYS] Sys_open: %s (requested read_only=%d, is_cdrom=%d)\n", name, read_only, is_cdrom);
    
    // Allocate file handle
    file_handle *fh = new file_handle;
    if (!fh) {
        Serial.println("[SYS] Sys_open: failed to allocate file handle");
        return NULL;
    }
    
    memset(fh, 0, sizeof(file_handle));
    strncpy(fh->path, name, sizeof(fh->path) - 1);
    fh->is_cdrom = is_cdrom;
    fh->is_floppy = (strstr(name, ".img") != NULL || strstr(name, ".IMG") != NULL);
    
    // CD-ROMs and ISO files are always read-only
    // Otherwise, respect the read_only parameter from caller
    if (is_cdrom || strstr(name, ".iso") != NULL || strstr(name, ".ISO") != NULL) {
        fh->read_only = true;
    } else {
        fh->read_only = read_only;
    }
    
    // Open file based on read_only flag
    if (fh->read_only) {
        Serial.printf("[SYS] Opening %s in READ-ONLY mode\n", name);
        fh->file = SD.open(name, FILE_READ);
    } else {
        // Use "r+b" mode: read+write without truncation (binary mode)
        // This is safe - it does NOT truncate like FILE_WRITE does
        Serial.printf("[SYS] Opening %s in READ-WRITE mode (r+b)\n", name);
        fh->file = SD.open(name, "r+b");
        if (!fh->file) {
            // Fall back to read-only if read-write mode fails
            Serial.printf("[SYS] WARNING: Read-write open failed, falling back to read-only\n");
            fh->file = SD.open(name, FILE_READ);
            fh->read_only = true;
        }
    }
    
    if (!fh->file) {
        Serial.printf("[SYS] ERROR: Cannot open file: %s\n", name);
        delete fh;
        return NULL;
    }
    
    // Get file size
    fh->size = fh->file.size();
    Serial.printf("[SYS] File size from size(): %lld bytes\n", (long long)fh->size);
    
    // If size() returns 0, try alternative methods
    if (fh->size == 0) {
        // Method: seek to end and get position
        if (fh->file.seek(0, SeekEnd)) {
            fh->size = fh->file.position();
            fh->file.seek(0, SeekSet);
            Serial.printf("[SYS] File size from seek: %lld bytes\n", (long long)fh->size);
        }
    }
    
    // Validate file size
    if (fh->size == 0) {
        Serial.printf("[SYS] ERROR: File %s appears to be empty or size cannot be determined\n", name);
        fh->file.close();
        delete fh;
        return NULL;
    }
    
    fh->is_open = true;
    
    Serial.printf("[SYS] SUCCESS: Opened %s (%lld bytes = %lld KB, floppy=%d, read_only=%d)\n", 
                  name, (long long)fh->size, (long long)(fh->size / 1024), fh->is_floppy, fh->read_only);
    
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
        // Flush any pending writes before closing to ensure data integrity
        fh->file.flush();
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
    
    // Log first few reads from each file to see boot activity
    static int disk_reads = 0;
    static int cdrom_reads = 0;
    if (fh->is_cdrom) {
        cdrom_reads++;
        if (cdrom_reads <= 5 || cdrom_reads % 500 == 0) {
            Serial.printf("[BOOT] CD-ROM read #%d: offset=%lld len=%d\n", cdrom_reads, (long long)offset, (int)length);
        }
    } else {
        disk_reads++;
        if (disk_reads <= 5 || disk_reads % 500 == 0) {
            Serial.printf("[BOOT] Disk read #%d: %s offset=%lld len=%d\n", disk_reads, fh->path, (long long)offset, (int)length);
        }
    }
    
    return bytes_read;
}

/*
 *  Write to a file/device
 */
size_t Sys_write(void *arg, void *buffer, loff_t offset, size_t length)
{
    file_handle *fh = (file_handle *)arg;
    if (!fh || !fh->is_open || !buffer) {
        return 0;
    }
    
    if (fh->read_only) {
        // Log write attempts to read-only disks
        static int ro_write_attempts = 0;
        ro_write_attempts++;
        if (ro_write_attempts <= 5 || ro_write_attempts % 100 == 0) {
            Serial.printf("[SYS] Write blocked (read-only): %s attempt #%d\n", fh->path, ro_write_attempts);
        }
        return 0;
    }
    
    // Seek to offset
    if (!fh->file.seek(offset)) {
        Serial.printf("[SYS] Sys_write: seek failed to offset %lld\n", (long long)offset);
        return 0;
    }
    
    // Write data
    size_t bytes_written = fh->file.write((uint8_t *)buffer, length);
    
    // Flush to ensure data is written to SD card (prevents data loss on reboot)
    if (bytes_written > 0) {
        fh->file.flush();
    }
    
    // Log write operations to track disk activity
    static int disk_writes = 0;
    disk_writes++;
    if (disk_writes <= 10 || disk_writes % 100 == 0) {
        Serial.printf("[SYS] Disk write #%d: %s offset=%lld len=%d written=%d\n",
                      disk_writes, fh->path, (long long)offset, (int)length, (int)bytes_written);
    }
    
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
