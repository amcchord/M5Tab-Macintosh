// Standalone test firmware. Each reboot starts with an unclaimed card bus.
#if TAB5_SD_BENCHMARK
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <ff.h>
#include <diskio.h>
#include <fcntl.h>
#include <soc/sdmmc_reg.h>
#include <soc/soc.h>
#include <unistd.h>

#include "board.h"
#include "board_wifi.h"
#include "tab5/tab5_sd_idf.h"

static constexpr uint32_t kBytes = 4 * 1024 * 1024;
static constexpr uint32_t kChunk = 16 * 1024;
static uint32_t *words;
static bool attempted_mount;
static bool mounted;

// Every sector has different content. The caller supplies a run ID so the
// same sequence can be checked following a complete power cycle.
static bool pattern(bool verify, uint32_t run_id, uint32_t offset)
{
    uint32_t state = (run_id + offset / sizeof(uint32_t)) ^ 0x9471cd23;
    for (size_t i = 0; i < kChunk / sizeof(uint32_t); ++i) {
        state = 1664525U * state + 1013904223U;
        if (verify) {
            if (words[i] != state) {
                Serial.printf("Mismatch at byte %u\n", (unsigned)(offset + i * sizeof(uint32_t)));
                return false;
            }
        } else {
            words[i] = state;
        }
    }
    return true;
}

static void busInfo()
{
    const sdmmc_card_t *card = Tab5SD_Card();
    if (!card) {
        Serial.println("BUS Arduino SPI");
        return;
    }
    const int slot = card->host.slot;
    int khz = 0;
    if (card->host.get_real_freq) card->host.get_real_freq(slot, &khz);
    Serial.printf("CARD product=%.7s mfg=0x%02x oem=0x%04x rev=0x%02x sectors=%u max_khz=%u\n",
                  card->cid.name, card->cid.mfg_id, card->cid.oem_id, card->cid.revision,
                  (unsigned)card->csd.capacity, (unsigned)card->max_freq_khz);
    bool aligned = card->host.check_buffer_alignment && words &&
                   card->host.check_buffer_alignment(slot, words, kChunk);
    if (card->host.flags & SDMMC_HOST_FLAG_SPI) {
        Serial.printf("BUS SPI DMA khz=%d buffer_aligned=%d\n", khz, aligned);
        return;
    }
    // get_real_freq reads the controller dividers at call time. CTYPE is the
    // actual hardware width; get_bus_width() reports the configured maximum.
    uint32_t ctype = REG_READ(SDHOST_CTYPE_REG);
    uint32_t width = (ctype & (1u << (16 + slot))) ? 8 : (ctype & (1u << slot)) ? 4 : 1;
    Serial.printf("BUS native slot=%d live_khz=%d live_width=%u card_width=%u div=0x%08x "
                  "src=0x%08x enable=0x%08x buffer_aligned=%d\n", slot, khz, width,
                  1u << card->log_bus_width, (unsigned)REG_READ(SDHOST_CLKDIV_REG),
                  (unsigned)REG_READ(SDHOST_CLKSRC_REG), (unsigned)REG_READ(SDHOST_CLKENA_REG), aligned);
}

static FATFS *volumeInfo(bool report)
{
    auto card = BoardSD_Media();
    char root[8];
    snprintf(root, sizeof(root), "%u:/", card.drive);
    FATFS *fs = nullptr;
    DWORD freeClusters = 0;
    if (f_getfree(root, &freeClusters, &fs) != FR_OK || !fs || fs->pdrv != card.drive) return nullptr;
    if (report) {
        FF_DIR dir;
        FILINFO entry;
        unsigned files = 0, dirs = 0;
        if (f_opendir(&dir, root) != FR_OK) return nullptr;
        FRESULT status;
        while ((status = f_readdir(&dir, &entry)) == FR_OK && entry.fname[0]) {
            if (entry.fattrib & AM_DIR) ++dirs;
            else ++files;
        }
        if (f_closedir(&dir) != FR_OK || status != FR_OK) return nullptr;
        const char *type = fs->fs_type == FS_FAT32 ? "FAT32" : fs->fs_type == FS_FAT16 ? "FAT16" :
                           fs->fs_type == FS_FAT12 ? "FAT12" : "other";
        Serial.printf("FAT type=%s sector_bytes=%u cluster_sectors=%u cluster_bytes=%u copies=%u "
                      "total_clusters=%u free_clusters=%u volume_lba=%u fat_lba=%u data_lba=%u "
                      "root_files=%u root_dirs=%u\n", type, card.sector_size, fs->csize,
                      fs->csize * card.sector_size, fs->n_fats, (unsigned)(fs->n_fatent - 2),
                      (unsigned)freeClusters, (unsigned)fs->volbase, (unsigned)fs->fatbase,
                      (unsigned)fs->database, files, dirs);
    }
    return fs;
}

static bool rawRead(bool single)
{
    const BoardSdMedia card = BoardSD_Media();
    const FATFS *fs = volumeInfo(false);
    if (!words || !fs || !card.sector_size || kChunk % card.sector_size || kBytes % card.sector_size)
        return false;
    const uint32_t first = fs->database, sectors = kBytes / card.sector_size;
    if (first > card.sectors || sectors > card.sectors - first) return false;
    const unsigned count = single ? 1 : kChunk / card.sector_size;
    const int64_t start = esp_timer_get_time();
    for (uint32_t n = 0; n < sectors; n += count) {
        if (disk_read(card.drive, (BYTE *)words, first + n, count) != RES_OK) return false;
    }
    const int64_t us = esp_timer_get_time() - start;
    Serial.printf("RAW_READ first_lba=%u bytes=%u sectors_per_request=%u elapsed_us=%lld KiB/s=%llu\n",
                  first, kBytes, count, (long long)us, kBytes * 1000000ULL / 1024 / us);
    return true;
}

static bool transfer(const char *path, uint32_t id, bool write_file, bool preallocate = false)
{
    int fd = open(path, write_file ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY, 0600);
    if (fd < 0) {
        Serial.printf("open failed errno=%d\n", errno);
        return false;
    }
    // The O_EXCL above establishes ownership before the IDF allocation helper
    // opens the same file. The diagnostic has no other file-writing task.
    const int64_t start = esp_timer_get_time();
    int64_t allocationUs = 0;
    if (preallocate) {
        if (close(fd) != 0) return false;
        esp_err_t err = esp_vfs_fat_create_contiguous_file(BOARD_SD_MOUNT_POINT, path, kBytes, true);
        if (err != ESP_OK) {
            Serial.printf("Preallocation failed error=%s errno=%d\n", esp_err_to_name(err), errno);
            unlink(path); // This invocation created the previously absent file.
            return false;
        }
        allocationUs = esp_timer_get_time() - start;
        fd = open(path, O_WRONLY);
        if (fd < 0) { unlink(path); return false; }
    }
    int64_t ioUs = 0, syncUs = 0;
    bool ok = true;
    for (uint32_t pos = 0; ok && pos < kBytes; pos += kChunk) {
        if (write_file) {
            pattern(false, id, pos);
            auto ioStart = esp_timer_get_time();
            ok = write(fd, words, kChunk) == kChunk;
            ioUs += esp_timer_get_time() - ioStart;
        } else {
            auto ioStart = esp_timer_get_time();
            ok = read(fd, words, kChunk) == kChunk;
            ioUs += esp_timer_get_time() - ioStart;
            if (ok) ok = pattern(true, id, pos);
        }
        if (!ok) Serial.printf("Transfer failed at byte %u errno=%d\n", pos, errno);
    }
    if (ok && write_file) {
        auto syncStart = esp_timer_get_time();
        ok = fsync(fd) == 0;
        syncUs = esp_timer_get_time() - syncStart;
    }
    if (ok && !write_file) {
        uint8_t extra;
        ok = read(fd, &extra, 1) == 0;
    }
    if (close(fd) != 0) ok = false;
    const uint64_t us = esp_timer_get_time() - start;
    if (!ok && write_file) unlink(path); // Only the O_EXCL creator reaches this path.
    if (ok) {
        if (write_file) {
            bool contiguous = false;
            auto status = esp_vfs_fat_test_contiguous_file(BOARD_SD_MOUNT_POINT, path, &contiguous);
            Serial.printf("FILE preallocated=%d contiguous=%d check=%s allocation_us=%lld sync_us=%lld\n",
                          preallocate, contiguous, esp_err_to_name(status), (long long)allocationUs, (long long)syncUs);
        }
        Serial.printf("IO calls_us=%lld calls_KiB/s=%llu\n", (long long)ioUs,
                      ioUs ? kBytes * 1000000ULL / 1024 / ioUs : 0ULL);
        Serial.printf("%s blocks=%u block_bytes=%u bytes=%u elapsed_us=%llu KiB/s=%llu\n",
                      write_file ? "WRITE_SYNC" : "READ_VERIFY", kBytes / kChunk,
                      kChunk, kBytes, (unsigned long long)us,
                      (unsigned long long)(kBytes * 1000000ULL / 1024 / us));
    }
    return ok;
}

static void command(const String &line)
{
    if (line == "RESET") {
        Serial.println("Restarting");
        Serial.flush();
        ESP.restart();
    } else if (line.startsWith("MOUNT ")) {
        if (attempted_mount) {
            Serial.println("ERR reset before selecting another mode");
            return;
        }
        const String mode = line.substring(6);
        if (mode != "SPI" && mode != "DMA" && mode != "NATIVE") {
            Serial.println("ERR mode");
            return;
        }
        attempted_mount = true;
        if (mode == "SPI") mounted = Tab5SD_MountArduino();
        if (mode == "DMA") mounted = Tab5SD_MountDma();
        if (mode == "NATIVE") mounted = Tab5SD_MountNative();
        if (!mounted) {
            Serial.println("ERR mount");
            return;
        }
        const BoardSdMedia card = BoardSD_Media();
        File root = SD_FS.open("/");
        mounted = root && root.isDirectory() && card.sector_size && card.sectors && card.drive != 0xff;
        root.close();
        Serial.printf("%s MOUNT %s sectors=%u sector_bytes=%u drive=%u\n",
                      mounted ? "OK" : "ERR", mode.c_str(), card.sectors, card.sector_size, card.drive);
        if (mounted) busInfo();
    } else if (line == "INFO" && mounted) {
        busInfo();
        Serial.println(volumeInfo(true) ? "OK INFO" : "ERR INFO");
    } else if ((line == "RAW" || line == "RAW1") && mounted) {
        busInfo();
        Serial.printf("%s %s\n", rawRead(line == "RAW1") ? "OK" : "ERR", line.c_str());
    } else if (line == "WIFI" && mounted) {
        busInfo();
        BoardWifi_Prepare();
        if (!WiFi.mode(WIFI_STA)) {
            Serial.println("ERR WiFi start");
            return;
        }
        int count = WiFi.scanNetworks();
        WiFi.scanDelete();
        busInfo();
        Serial.printf("%s WIFI networks=%d\n", count < 0 ? "ERR" : "OK", count);
    } else {
        const int sep = line.indexOf(' ');
        const String op = line.substring(0, sep);
        const String arg = line.substring(sep + 1);
        if (!mounted || !words || sep < 0 || arg.length() != 8 ||
            strspn(arg.c_str(), "0123456789abcdefABCDEF") != 8) {
            Serial.println("ERR expected MOUNT, INFO, RAW, RAW1, WIFI, WRITE id, PREALLOC id, CHECK id, CLEAN id or RESET");
            return;
        }
        const uint32_t id = strtoul(arg.c_str(), nullptr, 16);
        char path[40];
        snprintf(path, sizeof(path), BOARD_SD_MOUNT_POINT "/SDB%08x.tmp", (unsigned)id);
        bool ok = false;
        if (op == "WRITE" || op == "PREALLOC") {
            busInfo();
            ok = transfer(path, id, true, op == "PREALLOC") && transfer(path, id, false);
        } else if (op == "CHECK" || op == "CLEAN") {
            ok = transfer(path, id, false);
            if (ok && op == "CLEAN") ok = unlink(path) == 0;
        }
        Serial.printf("%s %s %s\n", ok ? "OK" : "ERR", op.c_str(), arg.c_str());
    }
}

void setup()
{
    Serial.begin(115200);
    Board_Init();
    words = static_cast<uint32_t *>(
        heap_caps_aligned_alloc(64, kChunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!words) Serial.println("ERR DMA buffer");
    Serial.println("SD benchmark ready");
}

void loop()
{
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        command(line);
    }
    delay(5);
}
#endif
