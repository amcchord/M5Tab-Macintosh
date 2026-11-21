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

int main(int argc, char **argv) {
    assert(argc == 6);
    host_mode = argv[1];
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
    free(scratch);
    if (cancelled()) { puts("cancelled"); return 2; }
    if (err) { puts(err); return 1; }
    return 0;
}
