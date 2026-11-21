#include "disk_image_test_stubs.h"
#include "sys.h"
HostSerial Serial;

int main(int argc, char **argv)
{
    assert(argc == 3 || argc == 6);
    if (argc == 6) {
        // Normal disk mount sequence from DiskOpen, followed by sector and
        // byte reads from the actual backend used by DiskPrime.
        uint32_t start = strtoul(argv[3], nullptr, 0);
        uint16_t expected = strtoul(argv[4], nullptr, 16);
        bool readonly = !strcmp(argv[2], "readonly");
        void *fh = Sys_open(argv[1], readonly);
        assert(fh);
        uint32_t sectors = (SysGetFileSize(fh) - start) / 512;
        Sys_prepare_hfs_boot(fh, start, sectors);
        uint8_t sector[512];
        assert(Sys_read(fh, sector, start + 1024, sizeof(sector)) == sizeof(sector));
        assert(sector[10] == (expected >> 8) && sector[11] == (expected & 0xff));
        uint8_t high;
        assert(Sys_read(fh, &high, start + 1034, 1) == 1 && high == (expected >> 8));
        assert(!Sys_has_dirty_handles());
        if (!strcmp(argv[5], "guest_write")) {
            // A guest update elsewhere keeps the handoff active. A write
            // to the attribute byte transfers ownership back to the guest.
            uint8_t byte = 0;
            assert(Sys_write(fh, &byte, start + 1035, 1) == 1);
            assert(Sys_read(fh, &high, start + 1034, 1) == 1 && high == 9);
            assert(Sys_write(fh, &byte, start + 1034, 1) == 1);
            Sys_prepare_hfs_boot(fh, start, sectors);
            assert(Sys_read(fh, &high, start + 1034, 1) == 1 && high == 0);
        }
        Sys_close(fh);
        return 0;
    }
    bool write = !strcmp(argv[2], "write");
    bool read_only = !strcmp(argv[2], "readonly");
    void *image = Sys_open(argv[1], read_only);
    assert(image && SysIsReadOnly(image) == read_only);
    uint8_t signature[2];
    assert(Sys_read(image, signature, 1024, 2) == 2);
    assert(signature[0] == 'B' && signature[1] == 'D');
    if (write || read_only) {
        uint8_t data[] = {0xde, 0xad};
        assert(Sys_write(image, data, 2048, 2) == (write ? 2u : 0u));
    }
    assert(Sys_has_dirty_handles() == write);
    Sys_periodic_flush();
    Sys_close(image);
    assert(!Sys_has_dirty_handles());
}
