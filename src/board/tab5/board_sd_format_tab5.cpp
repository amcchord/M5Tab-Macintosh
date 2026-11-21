#include "board_sd_format.h"
#include "board_sd.h"
#include "tab5_sd_host.h"
#include "tab5_sd_idf.h"
#include "tab5_fat_layout.h"

#include <Arduino.h>
#include <atomic>
#include <driver/spi_master.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <ff.h>
#include <diskio.h>
#include <diskio_impl.h>
#include <diskio_sdmmc.h>

#if TAB5_SD_NATIVE || TAB5_SD_BENCHMARK
static constexpr bool kTryNative = true;
#else
static constexpr bool kTryNative = false;
#endif

namespace {
struct Identity {
    char cid[48];
    uint64_t capacity;
    uint32_t first_crc;
};
Identity inspected;
char confirmation[33];
std::atomic_flag busy = ATOMIC_FLAG_INIT;
struct CardOperation {
    bool held = !busy.test_and_set();
    ~CardOperation() { if (held) busy.clear(); }
};

class RawCard {
public:
    sdmmc_card_t card = {};
    bool open(bool native) {
        sdmmc_host_t host;
        esp_err_t err;
        if (native) {
            sdmmc_slot_config_t slot;
            err = Tab5SD_NativeConfig(host, slot);
            if (err == ESP_OK) err = sdmmc_host_init();
            if (err == ESP_OK) err = sdmmc_host_init_slot(host.slot, &slot);
            native_slot = err == ESP_OK;
        } else {
            sdspi_device_config_t device;
            err = Tab5SD_InitSpiBus(host, device);
            spi_bus = err == ESP_OK;
            if (err == ESP_ERR_INVALID_STATE) Tab5SD_BlockMounts();
            if (err == ESP_OK) err = sdspi_host_init();
            if (err == ESP_OK) err = sdspi_host_init_device(&device, &spi_device);
            if (err == ESP_OK) host.slot = spi_device;
        }
        if (err == ESP_OK) err = sdmmc_card_init(&host, &card);
        if (err != ESP_OK) Serial.printf("[SD] Card inspection: %s\n", esp_err_to_name(err));
        return err == ESP_OK;
    }
    bool close() {
        if (native_slot && sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_0) != ESP_OK) Tab5SD_BlockMounts();
        if (spi_device != -1 && sdspi_host_remove_device(spi_device) != ESP_OK) Tab5SD_BlockMounts();
        if (spi_bus && spi_bus_free(SDSPI_DEFAULT_HOST) != ESP_OK) Tab5SD_BlockMounts();
        native_slot = spi_bus = false;
        spi_device = -1;
        return Tab5SD_CanRetryMount();
    }
    bool start(Tab5SdMode mode) {
        if (!Tab5SD_CanRetryMount()) return false;
        if (mode == Tab5SdMode::Native || (mode == Tab5SdMode::None && kTryNative)) {
            if (open(true)) return true;
            if (!close() || mode == Tab5SdMode::Native) return false;
        }
        return open(false);
    }
private:
    sdspi_dev_handle_t spi_device = -1;
    bool native_slot = false, spi_bus = false;
};

const char *inspect(sdmmc_card_t &card, BoardSdCardInfo &info, Identity &id) {
    using namespace Tab5Fat;
    if (!card.is_mem || card.is_mmc || card.csd.sector_size != 512 || !card.csd.capacity)
        return "This card does not report supported SD sector geometry.";
    const uint32_t sectors = (uint32_t)card.csd.capacity;
    info.capacity_bytes = (uint64_t)sectors * 512;
    info.bus_width = 1U << card.log_bus_width;
    int khz = 0;
    card.host.get_real_freq(card.host.slot, &khz);
    info.clock_khz = khz;
    snprintf(info.driver, sizeof(info.driver), "%s",
             card.host.flags & SDMMC_HOST_FLAG_SPI ? "SPI DMA" : "SDMMC slot 0");
    // Decode CID fields explicitly, excluding padding and variable driver state.
    const auto &cid = card.cid;
    snprintf(info.identity, sizeof(info.identity), "%02X-%04X-%02X%02X%02X%02X%02X-%02X-%08X-%04X",
             cid.mfg_id & 255, cid.oem_id & 65535, (uint8_t)cid.name[0], (uint8_t)cid.name[1],
             (uint8_t)cid.name[2], (uint8_t)cid.name[3], (uint8_t)cid.name[4], cid.revision & 255,
             (unsigned)cid.serial, cid.date & 65535);
    info.format_supported = cid.serial && cid.serial != -1 && allocationUnit(sectors);
    alignas(128) uint8_t b[512];
    if (sdmmc_read_sectors(&card, b, 0, 1) != ESP_OK) return "Could not read the card's first sector.";
    id.capacity = info.capacity_bytes;
    strcpy(id.cid, info.identity);
    id.first_crc = esp_rom_crc32_le(0, b, sizeof(b));
    const char *layout = volume(b, sectors, info.cluster_bytes);
    if (layout) info.volume_bytes = info.capacity_bytes;
    if (!layout && signature(b)) {
        for (unsigned i = 0; i < 4; ++i) {
            const uint8_t *entry = b + 446 + i * 16;
            const uint32_t start = le32(entry + 8), size = le32(entry + 12);
            if (!entry[4] || !start || start >= sectors || !size || size > sectors - start) continue;
            info.volume_bytes = (uint64_t)size * 512;
            info.partition_lba = start;
            info.partition_type = entry[4];
            if (entry[4] == 0xee) { layout = "GPT"; break; }
            if (sdmmc_read_sectors(&card, b, start, 1) != ESP_OK)
                return "Could not read the card partition.";
            layout = volume(b, size, info.cluster_bytes);
            if (!layout) layout = "Other partition";
            break;
        }
    }
    snprintf(info.layout, sizeof(info.layout), "%s", layout ? layout : "Unknown or blank");
    return nullptr;
}

const char *format(sdmmc_card_t &card) {
    const uint32_t sectors = (uint32_t)card.csd.capacity;
    const unsigned au = Tab5Fat::allocationUnit(sectors);
    constexpr unsigned work_bytes = 16 * 1024;
    auto *work = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        128, work_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!work) return "Not enough memory to format the card.";
    BYTE drive = 0xff;
    const char *err = nullptr;
    if (ff_diskio_get_drive(&drive) != ESP_OK) err = "No free card drive.";
    if (!err && (VolToPart[drive].pd != drive || VolToPart[drive].pt != 0))
        err = "Card drive is mapped to a single partition.";
    if (!err) {
        ff_diskio_register_sdmmc(drive, &card);
        // Remove stale GPT signatures that could otherwise confuse a desktop
        // partition scanner after an explicitly requested conversion to MBR.
        const uint32_t headers[] = {1, sectors - 1};
        for (uint32_t sector : headers) {
            if (disk_read(drive, work, sector, 1) != RES_OK) { err = "Could not read partition headers."; break; }
            if (memcmp(work, "EFI PART", 8)) continue;
            memset(work, 0, 512);
            if (disk_write(drive, work, sector, 1) != RES_OK) { err = "Could not clear the old GPT."; break; }
        }
        if (!err) {
            char path[] = {(char)('0' + drive), ':', 0};
            MKFS_PARM opts = {};
            opts.fmt = FM_FAT32;
            opts.n_fat = 2;
            opts.align = 2048; // align the data region to an absolute 1 MiB boundary
            opts.au_size = au;
            const FRESULT result = f_mkfs(path, &opts, work, work_bytes);
            if (result != FR_OK || disk_ioctl(drive, CTRL_SYNC, nullptr) != RES_OK) {
                Serial.printf("[SD] FAT32 format result %d\n", (int)result);
                err = "FAT32 formatting failed. Inspect and retry the card.";
            }
        }
        ff_diskio_unregister(drive);
    }
    free(work);
    return err;
}
}

extern "C" const char *BoardSD_InspectCard(BoardSdCardInfo *out)
{
    CardOperation operation;
    if (!operation.held) return "Another card operation is running.";
    confirmation[0] = 0;
    if (!out) return "Missing card information.";
    *out = {};
    const Tab5SdMode mode = Tab5SD_MountedMode();
    const char *err;
#if TAB5_SD_IDF
    if (const auto *mounted = Tab5SD_Card()) {
        err = inspect(*const_cast<sdmmc_card_t *>(mounted), *out, inspected);
    } else
#endif
    {
        if (!Tab5SD_Unmount()) return "Card bus is busy. Restart before inspecting.";
        RawCard raw;
        err = raw.start(mode) ? inspect(raw.card, *out, inspected) : "Could not identify the card. Reseat it and retry.";
        if (!raw.close()) return "Card bus is busy. Restart before retrying.";
        if (mode != Tab5SdMode::None && !Tab5SD_Restore(mode)) return "Could not remount the card.";
        if (mode == Tab5SdMode::ArduinoSpi) strcpy(out->driver, "Arduino SPI");
    }
    if (err) return err;
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    for (unsigned i = 0; i < sizeof(random); ++i) snprintf(confirmation + 2 * i, 3, "%02x", random[i]);
    strcpy(out->token, confirmation);
    return nullptr;
}

extern "C" const char *BoardSD_FormatCard(const char *token)
{
    CardOperation operation;
    if (!operation.held) return "Another card operation is running.";
    if (!token || !confirmation[0] || strcmp(confirmation, token)) return "Card confirmation expired. Inspect again.";
    confirmation[0] = 0;
    const Tab5SdMode mode = Tab5SD_MountedMode();
    if (!Tab5SD_Unmount()) return "Card bus is busy. Restart before formatting.";
    RawCard raw;
    BoardSdCardInfo info = {};
    Identity current = {};
    const char *err = raw.start(mode) ? inspect(raw.card, info, current) : "Could not identify the card.";
    if (!err && (strcmp(current.cid, inspected.cid) || current.capacity != inspected.capacity ||
                 current.first_crc != inspected.first_crc))
        err = "The card changed. Inspect it again.";
    if (!err && !info.format_supported) err = "This card's size or identity cannot be confirmed for FAT32.";
    Tab5SdMode restore = mode;
    if (!err && mode == Tab5SdMode::None) {
        if (!(raw.card.host.flags & SDMMC_HOST_FLAG_SPI)) restore = Tab5SdMode::Native;
#if TAB5_SD_DMA || TAB5_SD_BENCHMARK
        else restore = Tab5SdMode::DmaSpi;
#else
        else restore = Tab5SdMode::ArduinoSpi;
#endif
    }
    if (!err) err = format(raw.card);
    if (!err) {
        BoardSdCardInfo after = {};
        err = inspect(raw.card, after, current);
        if (!err && (strcmp(after.layout, "FAT32") || after.partition_type != 0x0c ||
                     after.cluster_bytes != Tab5Fat::allocationUnit((uint32_t)raw.card.csd.capacity) ||
                     after.volume_bytes + uint64_t{512} * after.partition_lba != after.capacity_bytes))
            err = "FAT32 partition readback did not match.";
    }
    if (!raw.close()) return "Card bus is busy. Restart before retrying.";
    if ((!err || mode != Tab5SdMode::None) && !Tab5SD_Restore(restore) && !err)
        err = "FAT32 was written. Remount failed. Reseat the card and retry.";
    return err;
}
