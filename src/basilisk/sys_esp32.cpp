/*
 *  sys_esp32.cpp - System dependent routines for ESP32 (SD card I/O)
 *
 *  BasiliskII ESP32 Port
 *
 *  SIMPLIFIED DIRECT I/O - No caching, no buffering
 *  
 *  SD card has internal caching and Mac OS already batches I/O efficiently.
 *  Adding software caching/buffering just adds CPU overhead without benefit.
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

// File handle structure - minimal
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

// Open file handles for periodic flush
static file_handle *open_file_handles[16] = {NULL};

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
 *  Register an open file handle
 */
static void register_file_handle(file_handle *fh)
{
    for (int i = 0; i < 16; i++) {
        if (open_file_handles[i] == NULL) {
            open_file_handles[i] = fh;
            return;
        }
    }
}

/*
 *  Unregister a file handle
 */
static void unregister_file_handle(file_handle *fh)
{
    for (int i = 0; i < 16; i++) {
        if (open_file_handles[i] == fh) {
            open_file_handles[i] = NULL;
            return;
        }
    }
}

/*
 *  Periodic flush - ensures data is written to SD card
 *  Called every 2 seconds from main loop
 */
void Sys_periodic_flush(void)
{
    for (int i = 0; i < 16; i++) {
        if (open_file_handles[i] != NULL && 
            open_file_handles[i]->is_open && 
            !open_file_handles[i]->read_only) {
            open_file_handles[i]->file.flush();
        }
    }
}

/*
 *  Initialization
 */
void SysInit(void)
{
    init_sd_card();
    Serial.println("[SYS] Direct I/O mode (no caching)");
}

/*
 *  Deinitialization
 */
void SysExit(void)
{
    // Flush all open files
    Sys_periodic_flush();
    sd_initialized = false;
}

/*
 *  Mount first floppy disk
 */
void SysAddFloppyPrefs(void)
{
}

/*
 *  Mount first hard disk
 */
void SysAddDiskPrefs(void)
{
}

/*
 *  Mount CD-ROM
 */
void SysAddCDROMPrefs(void)
{
}

/*
 *  Add serial port preferences
 */
void SysAddSerialPrefs(void)
{
}

/*
 *  Repair HFS volume - fix common corruption issues from improper shutdown
 */
static void Sys_repair_hfs_volume(const char *path)
{
    // Only repair .dsk files
    if (strstr(path, ".dsk") == NULL && strstr(path, ".DSK") == NULL) {
        return;
    }
    
    Serial.printf("[SYS] Checking HFS volume: %s\n", path);
    
    File f = SD.open(path, "r+b");
    if (!f) {
        return;
    }
    
    size_t file_size = f.size();
    if (file_size < 1024 + 512) {
        f.close();
        return;
    }
    
    // Read main MDB
    uint8_t mdb[128];
    if (!f.seek(1024) || f.read(mdb, 128) != 128) {
        f.close();
        return;
    }
    
    // Check HFS signature
    uint16_t signature = (mdb[0] << 8) | mdb[1];
    if (signature != 0x4244) {
        f.close();
        return;
    }
    
    // Read key fields
    uint16_t drAtrb = (mdb[10] << 8) | mdb[11];
    uint32_t drFndrInfo2 = (mdb[100] << 24) | (mdb[101] << 16) | (mdb[102] << 8) | mdb[103];
    uint32_t drFndrInfo3 = (mdb[104] << 24) | (mdb[105] << 16) | (mdb[106] << 8) | mdb[107];
    
    // Get original drAtrb from Alternate MDB
    size_t amdb_offset = ((file_size / 512) - 2) * 512;
    uint16_t original_drAtrb = drAtrb;
    
    uint8_t amdb_sig[2];
    if (f.seek(amdb_offset) && f.read(amdb_sig, 2) == 2) {
        if ((amdb_sig[0] << 8 | amdb_sig[1]) == 0x4244) {
            uint8_t amdb_atrb[2];
            if (f.seek(amdb_offset + 10) && f.read(amdb_atrb, 2) == 2) {
                original_drAtrb = (amdb_atrb[0] << 8) | amdb_atrb[1];
            }
        }
    }
    
    bool needs_repair = false;
    
    if (drAtrb != original_drAtrb) {
        mdb[10] = (original_drAtrb >> 8) & 0xFF;
        mdb[11] = original_drAtrb & 0xFF;
        needs_repair = true;
    }
    
    if (drFndrInfo2 != 0) {
        mdb[100] = mdb[101] = mdb[102] = mdb[103] = 0;
        needs_repair = true;
    }
    
    if (drFndrInfo3 != 0) {
        mdb[104] = mdb[105] = mdb[106] = mdb[107] = 0;
        needs_repair = true;
    }
    
    if (needs_repair) {
        Serial.println("[SYS] Repairing HFS volume...");
        f.seek(1024 + 10);
        f.write(&mdb[10], 2);
        f.seek(1024 + 100);
        f.write(&mdb[100], 8);
        f.flush();
        Serial.println("[SYS] Volume repaired");
    } else {
        Serial.println("[SYS] Volume OK");
    }
    
    f.close();
}

/*
 *  Open a file/device
 */
void *Sys_open(const char *name, bool read_only, bool is_cdrom)
{
    if (!name || strlen(name) == 0) {
        return NULL;
    }
    
    // Repair HFS volume before opening
    if (!read_only && !is_cdrom) {
        Sys_repair_hfs_volume(name);
    }
    
    file_handle *fh = new file_handle;
    if (!fh) {
        return NULL;
    }
    
    memset(fh, 0, sizeof(file_handle));
    strncpy(fh->path, name, sizeof(fh->path) - 1);
    fh->is_cdrom = is_cdrom;
    fh->is_floppy = (strstr(name, ".img") != NULL || strstr(name, ".IMG") != NULL);
    
    // Determine read-only status
    if (is_cdrom || strstr(name, ".iso") != NULL || strstr(name, ".ISO") != NULL) {
        fh->read_only = true;
    } else {
        fh->read_only = read_only;
    }
    
    // Open file
    if (fh->read_only) {
        fh->file = SD.open(name, FILE_READ);
    } else {
        fh->file = SD.open(name, "r+b");
        if (!fh->file) {
            fh->file = SD.open(name, FILE_READ);
            fh->read_only = true;
        }
    }
    
    if (!fh->file) {
        delete fh;
        return NULL;
    }
    
    fh->size = fh->file.size();
    if (fh->size == 0) {
        if (fh->file.seek(0, SeekEnd)) {
            fh->size = fh->file.position();
            fh->file.seek(0, SeekSet);
        }
    }
    
    if (fh->size == 0) {
        fh->file.close();
        delete fh;
        return NULL;
    }
    
    fh->is_open = true;
    register_file_handle(fh);
    
    Serial.printf("[SYS] Opened %s (%lld KB, ro=%d)\n", 
                  name, (long long)(fh->size / 1024), fh->read_only);
    
    return fh;
}

/*
 *  Close a file/device
 */
void Sys_close(void *arg)
{
    file_handle *fh = (file_handle *)arg;
    if (!fh) return;
    
    if (fh->is_open) {
        unregister_file_handle(fh);
        fh->file.flush();
        fh->file.close();
        fh->is_open = false;
    }
    
    delete fh;
}

/*
 *  Read from a file/device - direct read, no caching
 */
size_t Sys_read(void *arg, void *buffer, loff_t offset, size_t length)
{
    file_handle *fh = (file_handle *)arg;
    if (!fh || !fh->is_open || !buffer) {
        return 0;
    }
    
    if (!fh->file.seek(offset)) {
        return 0;
    }
    
    return fh->file.read((uint8_t *)buffer, length);
}

/*
 *  Write to a file/device - direct write, no buffering
 */
size_t Sys_write(void *arg, void *buffer, loff_t offset, size_t length)
{
    file_handle *fh = (file_handle *)arg;
    if (!fh || !fh->is_open || !buffer || fh->read_only) {
        return 0;
    }
    
    if (!fh->file.seek(offset)) {
        return 0;
    }
    
    return fh->file.write((uint8_t *)buffer, length);
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
 *  Eject disk (no-op)
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
 *  Check if a fixed disk
 */
bool SysIsFixedDisk(void *arg)
{
    file_handle *fh = (file_handle *)arg;
    if (!fh) return true;
    return !fh->is_floppy && !fh->is_cdrom;
}

/*
 *  Check if disk is inserted
 */
bool SysIsDiskInserted(void *arg)
{
    file_handle *fh = (file_handle *)arg;
    if (!fh) return false;
    return fh->is_open;
}

void SysPreventRemoval(void *arg) { UNUSED(arg); }
void SysAllowRemoval(void *arg) { UNUSED(arg); }

// CD-ROM stubs
bool SysCDReadTOC(void *arg, uint8 *toc) { UNUSED(arg); UNUSED(toc); return false; }
bool SysCDGetPosition(void *arg, uint8 *pos) { UNUSED(arg); UNUSED(pos); return false; }
bool SysCDPlay(void *arg, uint8 start_m, uint8 start_s, uint8 start_f, uint8 end_m, uint8 end_s, uint8 end_f) {
    UNUSED(arg); UNUSED(start_m); UNUSED(start_s); UNUSED(start_f);
    UNUSED(end_m); UNUSED(end_s); UNUSED(end_f); return false;
}
bool SysCDPause(void *arg) { UNUSED(arg); return false; }
bool SysCDResume(void *arg) { UNUSED(arg); return false; }
bool SysCDStop(void *arg, uint8 lead_out_m, uint8 lead_out_s, uint8 lead_out_f) {
    UNUSED(arg); UNUSED(lead_out_m); UNUSED(lead_out_s); UNUSED(lead_out_f); return false;
}
bool SysCDScan(void *arg, uint8 start_m, uint8 start_s, uint8 start_f, bool reverse) {
    UNUSED(arg); UNUSED(start_m); UNUSED(start_s); UNUSED(start_f); UNUSED(reverse); return false;
}
void SysCDSetVolume(void *arg, uint8 left, uint8 right) { UNUSED(arg); UNUSED(left); UNUSED(right); }
void SysCDGetVolume(void *arg, uint8 &left, uint8 &right) { UNUSED(arg); left = right = 0; }
