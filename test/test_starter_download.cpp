// Compile the real installer with a synthetic pinned asset and offline server.
#include "starter_stubs/platform.h"
#include <unistd.h>
#include <cerrno>

static uintptr_t dictionary_start;
static int stage_fd = -1;
static ssize_t cardWrite(int fd, const void *data, size_t len) {
    uintptr_t p = reinterpret_cast<uintptr_t>(data);
    if (host_mode == "fragmented" && p >= dictionary_start &&
        p - dictionary_start < TINFL_LZ_DICT_SIZE) {
        assert(p == dictionary_start);
        printf("IMAGE_WRITE %zu\n", len);
    }
    if (host_mode == "partial_write_io" || host_mode == "partial_write_full") {
        if (stage_fd < 0) {
            stage_fd = fd;
            return write(fd, data, std::min(len, size_t(181)));
        }
        errno = host_mode == "partial_write_full" ? ENOSPC : EIO;
        return -1;
    }
    if (host_mode == "write_zero") {
        errno = ENOSPC; // A zero return does not set errno.
        return 0;
    }
    stage_fd = fd;
    return write(fd, data, len);
}
static int cardFsync(int fd) {
    if (fd == stage_fd && (host_mode == "flush_full" || host_mode == "partial_write_io")) {
        errno = ENOSPC;
        return -1;
    }
    return fsync(fd);
}
static int cardClose(int fd) {
    int rc = close(fd);
    if (fd == stage_fd && (host_mode == "flush_full" || host_mode == "close_io")) {
        errno = EIO;
        return -1;
    }
    return rc;
}
#define write cardWrite
#define fsync cardFsync
#define close cardClose
#include "../src/basilisk/starter_download.cpp"
#undef close
#undef fsync
#undef write
#include <fstream>
#include <iterator>

static void checkNetworkSpeed() {
    StarterNetworkSpeed speed;
    assert(speed.bytesPerSecond(0) < 0);
    uint32_t now = UINT32_MAX - 500;
    speed.start(now);
    const uint32_t first = now;
    for (unsigned i = 0; i < 32; ++i) {
        now += 125;
        speed.receivedBytes(512, now);
        if (now - first >= 750) assert(speed.bytesPerSecond(now) == 4096);
        else assert(speed.bytesPerSecond(now) < 0);
    }
    assert(speed.bytesPerSecond(now + 3000) == 0);
    now += 4000;
    speed.receivedBytes(128, now);
    assert(speed.bytesPerSecond(now) < 0);
    speed.stop();
    assert(speed.bytesPerSecond(now + 4000) < 0);
    speed.start(20000);
    assert(speed.bytesPerSecond(20750) == 0);
    char line[24];
    StarterFormatSpeed(line, sizeof(line), 0); assert(!strcmp(line, "0 B/s"));
    StarterFormatSpeed(line, sizeof(line), 900); assert(!strcmp(line, "900 B/s"));
    StarterFormatSpeed(line, sizeof(line), 1536); assert(!strcmp(line, "1.5 KiB/s"));
    StarterFormatSpeed(line, sizeof(line), 1572864); assert(!strcmp(line, "1.5 MiB/s"));
}

int main(int argc, char **argv) {
    assert(argc == 6);
    host_mode = argv[1];
    checkNetworkSpeed();
    if (host_mode == "timed" || host_mode == "rate_recover") host_read_hook = []() {
        printf("RATE %d\n", (int)StarterDownloadStatus().network_bytes_per_second);
    };
    host_delay_hook = []() {
        const auto status = StarterDownloadStatus();
        if (strstr(status.message, "Retrying")) {
            assert(status.network_bytes_per_second < 0);
            if (host_mode == "cancel_backoff") StarterDownloadCancel();
            if (host_mode == "changed_file") {
                host_delay_hook = nullptr;
                std::ofstream("card/.m5-starter-v1/Test.dsk.deflate", std::ios::app | std::ios::binary) << 'X';
            }
        }
        if (host_mode == "cancel_connect" || host_mode == "cancel_headers" || host_mode == "cancel_body")
            StarterDownloadCancel();
    };
    std::ifstream source(argv[2], std::ios::binary);
    payload.assign(std::istreambuf_iterator<char>(source), {});
    assert(!payload.empty());
    if (host_mode == "no_space") host_free = 1;
    Asset fixture = {"Test.dsk", asset_offset, (uint32_t)payload.size(),
                     (uint32_t)strtoul(argv[3], nullptr, 10), argv[4], argv[5]};
    Scratch *scratch = Scratch::allocate();
    assert(scratch);
    dictionary_start = reinterpret_cast<uintptr_t>(scratch->output);
    assert(reinterpret_cast<uintptr_t>(scratch->input) % CONFIG_CACHE_L2_CACHE_LINE_SIZE == 0);
    assert(reinterpret_cast<uintptr_t>(scratch->output) % CONFIG_CACHE_L2_CACHE_LINE_SIZE == 0);
    const char *err = prepareStage();
    if (!err) err = install(fixture, *scratch);
    assert(StarterDownloadStatus().network_bytes_per_second < 0);
    free(scratch);
    if (cancelled()) { puts("cancelled"); return 2; }
    if (err) { puts(err); return 1; }
    return 0;
}
