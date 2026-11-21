// Compile the real installer with a synthetic pinned asset and offline server.
#include "../src/basilisk/starter_download.cpp"
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
    Scratch scratch;
    const char *err = prepareStage();
    if (!err) err = install(fixture, scratch);
    if (cancelled()) { puts("cancelled"); return 2; }
    if (err) { puts(err); return 1; }
    return 0;
}
