// Standalone test firmware. Each reboot starts with an unclaimed card bus.
#if defined(TAB5_SD_BENCHMARK)
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <fcntl.h>
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

static bool transfer(const char *path, uint32_t id, bool write_file)
{
    int fd = open(path, write_file ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY, 0600);
    if (fd < 0) {
        Serial.printf("open failed errno=%d\n", errno);
        return false;
    }
    const int64_t start = esp_timer_get_time();
    bool ok = true;
    for (uint32_t pos = 0; ok && pos < kBytes; pos += kChunk) {
        if (write_file) {
            pattern(false, id, pos);
            ok = write(fd, words, kChunk) == kChunk;
        } else {
            ok = read(fd, words, kChunk) == kChunk && pattern(true, id, pos);
        }
        if (!ok) Serial.printf("Transfer failed at byte %u errno=%d\n", pos, errno);
    }
    if (ok && write_file) ok = fsync(fd) == 0;
    if (ok && !write_file) {
        uint8_t extra;
        ok = read(fd, &extra, 1) == 0;
    }
    if (close(fd) != 0) ok = false;
    const uint64_t us = esp_timer_get_time() - start;
    if (ok) {
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
    } else if (line == "WIFI" && mounted) {
        BoardWifi_Prepare();
        if (!WiFi.mode(WIFI_STA)) {
            Serial.println("ERR WiFi start");
            return;
        }
        int count = WiFi.scanNetworks();
        WiFi.scanDelete();
        Serial.printf("%s WIFI networks=%d\n", count < 0 ? "ERR" : "OK", count);
    } else {
        const int sep = line.indexOf(' ');
        const String op = line.substring(0, sep);
        const String arg = line.substring(sep + 1);
        if (!mounted || !words || sep < 0 || arg.length() != 8 ||
            strspn(arg.c_str(), "0123456789abcdefABCDEF") != 8) {
            Serial.println("ERR expected MOUNT, WIFI, WRITE id, CHECK id, CLEAN id or RESET");
            return;
        }
        const uint32_t id = strtoul(arg.c_str(), nullptr, 16);
        char path[40];
        snprintf(path, sizeof(path), BOARD_SD_MOUNT_POINT "/SDB%08x.tmp", (unsigned)id);
        bool ok = false;
        if (op == "WRITE") {
            ok = transfer(path, id, true) && transfer(path, id, false);
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
