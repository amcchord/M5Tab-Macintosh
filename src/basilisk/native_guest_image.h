// Guest disk files on the Tab5 native SDMMC interface. Pass sector-aligned
// packets through the FAT VFS without adding a newlib FILE buffer in between.
#pragma once

#include <atomic>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef DISK_IMAGE_HOST_TEST
#include "tab5/tab5_sd_idf.h"
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <soc/soc_caps.h>
#else
#include <cstdlib>
#define BOARD_SD_MOUNT_POINT ""
#endif

#define TAB5_NATIVE_GUEST_IMAGE 1

class NativeGuestImage {
    static constexpr size_t maxPacket = 16 * 1024;
    static constexpr size_t alignment = 128;
    inline static uint8_t *packet = nullptr;
    inline static size_t packetSize = maxPacket;
    inline static std::atomic_flag packetBusy = ATOMIC_FLAG_INIT;
    int fd = -1;
    File fallback;

    static bool nativeMounted() {
#ifdef DISK_IMAGE_HOST_TEST
        return getenv("TAB5_TEST_NATIVE") != nullptr;
#else
        const auto *card = Tab5SD_Card();
        return card && !(card->host.flags & SDMMC_HOST_FLAG_SPI);
#endif
    }

    static bool dmaCompatible(const uint8_t *data, size_t length) {
#ifdef DISK_IMAGE_HOST_TEST
        return !(reinterpret_cast<uintptr_t>(data) % alignment) && !(length % alignment);
#else
        const auto *card = Tab5SD_Card();
#if !SOC_SDMMC_PSRAM_DMA_CAPABLE
        if (esp_ptr_external_ram(data)) return false;
#endif
        // Use the same host predicate as sdmmc_read/write_sectors. P4 SRAM
        // and PSRAM may have different cache-line requirements.
        return card && card->host.check_buffer_alignment(card->host.slot, data, length);
#endif
    }

    size_t transfer(bool writing, uint8_t *data, size_t length) {
        if (!length) return 0;
        auto direct = [&](uint8_t *buffer, size_t n) -> size_t {
            ssize_t actual = writing ? ::write(fd, buffer, n) : ::read(fd, buffer, n);
            return actual < 0 ? 0 : static_cast<size_t>(actual);
        };
        // Aligned buffers can go directly through IDF. Small byte probes use
        // FatFs's sector cache. Contending readers retain the ordinary path.
        if (length < 512 || dmaCompatible(data, length) ||
            packetBusy.test_and_set(std::memory_order_acquire))
            return direct(data, length);
        while (!packet && packetSize >= 4096) {
#ifdef DISK_IMAGE_HOST_TEST
            packet = static_cast<uint8_t *>(std::aligned_alloc(alignment, packetSize));
#else
            packet = static_cast<uint8_t *>(heap_caps_aligned_alloc(alignment, packetSize,
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
#endif
            if (!packet) packetSize /= 2;
        }

        size_t done = 0;
        if (!packet) {
            done = direct(data, length);
        } else {
            // A misaligned PSRAM address would otherwise make IDF allocate a
            // bounce buffer and issue a separate SD command for every sector.
            while (done < length) {
                const size_t n = length - done < packetSize ? length - done : packetSize;
                if (writing) memcpy(packet, data + done, n);
                const size_t actual = direct(packet, n);
                if (!writing) memcpy(data + done, packet, actual);
                done += actual;
                if (actual != n) break;
            }
        }
        packetBusy.clear(std::memory_order_release);
        return done;
    }

public:
    void open(const char *name, bool readonly) {
        if (!nativeMounted()) {
            fallback = SD_FS.open(name, readonly ? FILE_READ : "r+b");
            return;
        }
        const size_t len = strlen(name) + sizeof(BOARD_SD_MOUNT_POINT);
        char *path = static_cast<char *>(malloc(len));
        if (!path) return;
        snprintf(path, len, "%s%s", BOARD_SD_MOUNT_POINT, name);
        fd = ::open(path, readonly ? O_RDONLY : O_RDWR);
        free(path);
    }

    explicit operator bool() const { return fd >= 0 || bool(fallback); }
    size_t size() {
        if (fd < 0) return fallback.size();
        struct stat st;
        return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) ? st.st_size : 0;
    }
    bool seek(uint32_t offset, SeekMode mode = SeekSet) {
        if (fd < 0) return fallback.seek(offset, mode);
        return lseek(fd, offset, mode == SeekEnd ? SEEK_END : SEEK_SET) >= 0;
    }
    size_t position() {
        return fd < 0 ? fallback.position() : lseek(fd, 0, SEEK_CUR);
    }
    size_t read(uint8_t *data, size_t n) {
        return fd < 0 ? fallback.read(data, n) : transfer(false, data, n);
    }
    size_t write(uint8_t *data, size_t n) {
        return fd < 0 ? fallback.write(data, n) : transfer(true, data, n);
    }
    void flush() {
        if (fd < 0) fallback.flush();
        else fsync(fd);
    }
    void close() {
        if (fd < 0) fallback.close();
        else { ::close(fd); fd = -1; }
    }
    static void releaseTransferBuffer() {
        if (packetBusy.test_and_set(std::memory_order_acquire)) return;
        free(packet);
        packet = nullptr;
        packetSize = maxPacket;
        packetBusy.clear(std::memory_order_release);
    }
};
