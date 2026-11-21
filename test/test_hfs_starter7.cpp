#include "disk_image_test_stubs.h"
#include "sys.h"
#include <unistd.h>
HostSerial Serial;

// Inject a failed sync after a given edit, then exercise the real retry path.
static int failSync, syncCount;
static int testSync(int fd) { return ++syncCount == failSync ? -1 : fsync(fd); }
#define fsync testSync
#include "../src/basilisk/hfs_starter7.cpp"
#undef fsync

int main(int argc, char **argv)
{
    assert(argc >= 3);
    const char *path = argv[1], *mode = argv[2];
    if (strlen(mode) == 64) {
        uint8_t digest[32];
        for (int i = 0; i < 32; i++) assert(sscanf(mode + i * 2, "%2hhx", &digest[i]) == 1);
        int fd = open(path, O_RDWR);
        assert(fd >= 0);
        if (argc == 4) failSync = atoi(argv[3]);
        auto result = repair(fd, digest);
        assert(close(fd) == 0);
        return result == Repair::Complete ? 0 : result == Repair::Error ? 2 : 1;
    }
    bool startup = strcmp(mode, "open") != 0;
    void *fh = Sys_open(path, !strcmp(mode, "readonly"), !strcmp(mode, "cdrom"), startup);
    assert(fh);
    if (startup) Sys_prepare_hfs_boot(fh, 0, SysGetFileSize(fh) / 512);
    Sys_close(fh);
}
