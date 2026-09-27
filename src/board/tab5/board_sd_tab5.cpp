/*
 * board_sd_tab5.cpp - microSD mount for M5Stack Tab5 (SPI mode).
 *
 * Mirrors the initSDCard() behaviour that lived in src/main.cpp before the
 * board HAL landed. Keeping SPI mode on Tab5 preserves the existing, tested
 * throughput characteristics. The mount point is the Arduino SD library
 * default ("/" - all paths are relative to the SD root).
 */

#include "board_sd.h"
#include "board_sd_probe.h"
#include "board_config.h"

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
/* sd_diskio is the low-level SDSPI helper inside the Arduino SD library.
 * It exposes sdcard_init / sd_read_raw / sdcard_uninit so we can probe
 * the boot sector independently of f_mount when an exFAT card causes
 * SD.begin() to fail. */
#include "sd_diskio.h"

static bool     s_mounted = false;
static uint64_t s_size_mb = 0;

extern "C" bool BoardSD_Init(void)
{
    if (s_mounted) return true;

    Serial.println("[SD] Initializing SD card (SPI mode)...");
    Serial.printf("[SD] Pins: SCK=%d MOSI=%d MISO=%d CS=%d\n",
                  BOARD_SD_SPI_SCK, BOARD_SD_SPI_MOSI, BOARD_SD_SPI_MISO, BOARD_SD_SPI_CS);

    SPI.begin(BOARD_SD_SPI_SCK, BOARD_SD_SPI_MISO, BOARD_SD_SPI_MOSI, BOARD_SD_SPI_CS);

    constexpr uint32_t kSdSpiHz = 10000000;
    /* Pass BOARD_SD_MOUNT_POINT explicitly so the VFS prefix the rest
     * of the firmware uses (POSIX stat/mkdir for ExtFS, etc.) stays
     * pinned to a single source of truth. Arduino SD's default is
     * "/sd" anyway, but matching it explicitly keeps the macro honest. */
    if (!SD.begin(BOARD_SD_SPI_CS, SPI, kSdSpiHz,
                  BOARD_SD_MOUNT_POINT, /*max_files=*/5,
                  /*format_if_empty=*/false)) {
        Serial.println("[SD] ERROR: initialization failed!");
        BoardSdFsType fs = BoardSD_ProbeFilesystem();
        if (fs == BOARD_SD_FS_EXFAT) {
            Serial.println("[SD]   Card detected, formatted as exFAT.");
            Serial.println("[SD]   This firmware uses the precompiled FatFs from");
            Serial.println("[SD]   pioarduino which does NOT include exFAT support.");
            Serial.println("[SD]   Please reformat the card as FAT32.");
            Serial.println("[SD]   (Mac OS HFS disk images are 2 GB max anyway, so the");
            Serial.println("[SD]   FAT32 4 GB per-file limit is not a practical issue.)");
        } else if (fs == BOARD_SD_FS_FAT) {
            Serial.println("[SD]   Card detected with a FAT volume but mount still");
            Serial.println("[SD]   failed - the volume may be damaged. Try reformatting.");
        } else {
            Serial.println("[SD]   No recognisable boot sector found. Possible causes:");
            Serial.println("[SD]   card is unformatted, contacts dirty, or not seated.");
            Serial.println("[SD]   If the card was formatted as exFAT (default for");
            Serial.println("[SD]   modern cards >32 GB), reformat as FAT32.");
        }
        return false;
    }

    s_size_mb = SD.cardSize() / (1024 * 1024);
    s_mounted = true;
    Serial.printf("[SD] Mounted, %llu MB\n", (unsigned long long)s_size_mb);
    return true;
}

/*
 * Read sector 0 via sdcard_init / sd_read_raw and look for either the
 * "EXFAT   " ASCII signature (offset 3) or one of the FAT signatures
 * ("FAT12   "/"FAT16   " at offset 54, "FAT32   " at offset 82).
 *
 * Called only after SD.begin() has failed and torn down its own pdrv,
 * so the SDSPI host should be free for us to bring up briefly. If
 * anything in the probe path errors out we just return UNKNOWN - the
 * probe is a diagnostic helper, never a hard requirement.
 */
extern "C" BoardSdFsType BoardSD_ProbeFilesystem(void)
{
    uint8_t pdrv = sdcard_init(BOARD_SD_SPI_CS, &SPI, 1000000);
    if (pdrv == 0xFF) {
        return BOARD_SD_FS_UNKNOWN;
    }

    /* Allocate the sector buffer from the heap rather than BSS. It is
     * only ever live for the duration of this function and we never
     * want it to compete with the 68k emulator's mem_banks dispatch
     * table or the cpufunctbl for the precious internal SRAM at
     * permanent BSS rest. 512 bytes covers every standard SD sector
     * size in use; 4096-byte-sector cards do not exist for consumer
     * microSD and the SDSPI host re-blocks anyway. */
    uint8_t *sector_buf = (uint8_t *)malloc(512);
    if (sector_buf == NULL) {
        sdcard_uninit(pdrv);
        return BOARD_SD_FS_UNKNOWN;
    }

    BoardSdFsType result = BOARD_SD_FS_UNKNOWN;
    if (sd_read_raw(pdrv, sector_buf, 0)) {
        if (memcmp(&sector_buf[3], "EXFAT   ", 8) == 0) {
            result = BOARD_SD_FS_EXFAT;
        } else if (memcmp(&sector_buf[82], "FAT32   ", 8) == 0 ||
                   memcmp(&sector_buf[54], "FAT12   ", 8) == 0 ||
                   memcmp(&sector_buf[54], "FAT16   ", 8) == 0) {
            result = BOARD_SD_FS_FAT;
        }
    }

    free(sector_buf);
    sdcard_uninit(pdrv);
    return result;
}

extern "C" uint64_t BoardSD_CardSizeMB(void)
{
    return s_size_mb;
}
