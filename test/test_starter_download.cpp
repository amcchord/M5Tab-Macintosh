// Compile the real installer with a synthetic pinned asset and offline server.
#include "starter_stubs/platform.h"
#include <unistd.h>

static uintptr_t dictionary_start;
static ssize_t cardWrite(int fd, const void *data, size_t len) {
    uintptr_t p = reinterpret_cast<uintptr_t>(data);
    if (host_mode == "fragmented" && p >= dictionary_start &&
        p - dictionary_start < TINFL_LZ_DICT_SIZE) {
        assert(p == dictionary_start);
        printf("IMAGE_WRITE %zu\n", len);
    }
    return write(fd, data, len);
}
#define write cardWrite
#include "../src/basilisk/starter_download.cpp"
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
    if (host_mode == "timed") host_read_hook = []() {
        printf("RATE %d\n", (int)StarterDownloadStatus().network_bytes_per_second);
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
