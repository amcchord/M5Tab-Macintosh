/*
 *  sys.h - System dependent routines (mostly I/O)
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef SYS_H
#define SYS_H

// Supported media types
enum {
	MEDIA_FLOPPY		= 1,
	MEDIA_CD			= 2,
	MEDIA_HD			= 4,
	MEDIA_REMOVABLE		= MEDIA_FLOPPY | MEDIA_CD
};

extern void SysInit(void);
extern void SysExit(void);

extern void SysAddFloppyPrefs(void);
extern void SysAddDiskPrefs(void);
extern void SysAddCDROMPrefs(void);
extern void SysAddSerialPrefs(void);

/*
 *  These routines are used for reading from and writing to disk files
 *  or devices in sony.cpp, disk.cpp and cdrom.cpp. Their purpose is to
 *  hide all OS-specific details of file and device access. The routines
 *  must also hide all restrictions on the location or alignment of the
 *  data buffer or on the transfer length that may be imposed by the
 *  underlying OS.
 *  A file/device is identified by a "filename" (character string) that
 *  may (but need not) map to a valid file path. After Sys_open(), the
 *  file/device is identified by an abstract "file handle" (void *),
 *  that is freed by Sys_close().
 */

extern void *Sys_open(const char *name, bool read_only, bool is_cdrom = false, bool startup = false);
extern void Sys_close(void *fh);
extern size_t Sys_read(void *fh, void *buffer, loff_t offset, size_t length);
extern size_t Sys_write(void *fh, void *buffer, loff_t offset, size_t length);
extern loff_t SysGetFileSize(void *fh);
// Repair the authenticated, published System 7.5.3 starter before mounting it.
extern void Sys_repair_hfs7_starter(const char *path);
// Arm the first hard disk for the classic File Manager's boot-volume check.
extern void Sys_prepare_hfs_boot(void *fh, loff_t start, uint32_t sectors);
extern bool HfsCanDeferBootCheck(void *fh, loff_t start, uint32_t sectors);
extern void SysEject(void *fh);
extern bool SysFormat(void *fh);
extern bool SysIsReadOnly(void *fh);
extern bool SysIsFixedDisk(void *fh);
extern bool SysIsDiskInserted(void *fh);

extern void SysPreventRemoval(void *fh);
extern void SysAllowRemoval(void *fh);
extern bool SysCDReadTOC(void *fh, uint8 *toc);
extern bool SysCDGetPosition(void *fh, uint8 *pos);
extern bool SysCDPlay(void *fh, uint8 start_m, uint8 start_s, uint8 start_f, uint8 end_m, uint8 end_s, uint8 end_f);
extern bool SysCDPause(void *fh);
extern bool SysCDResume(void *fh);
extern bool SysCDStop(void *fh, uint8 lead_out_m, uint8 lead_out_s, uint8 lead_out_f);
extern bool SysCDScan(void *fh, uint8 start_m, uint8 start_s, uint8 start_f, bool reverse);
extern void SysCDSetVolume(void *fh, uint8 left, uint8 right);
extern void SysCDGetVolume(void *fh, uint8 &left, uint8 &right);

// Periodic flush for sector cache - call from main loop
extern void Sys_periodic_flush(void);

// Force-flush every dirty open handle right now. Safe to call from a
// shutdown hook or power-button handler; logs the number of handles
// it touched so the serial console shows the sync did happen before
// reset.
extern void Sys_flush_now(void);

// Returns true if any open handle still has un-flushed writes. Used by
// the basilisk_loop idle-flush path so we can run a fast flush within
// ~500 ms of the last write without thrashing the SD during heavy I/O.
extern bool Sys_has_dirty_handles(void);

// Returns the millis() timestamp of the most recent dirtying write, or
// 0 if no writes have happened since boot. Used by the idle-flush logic.
extern uint32_t Sys_last_write_ms(void);

#endif
