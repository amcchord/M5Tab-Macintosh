#include "disk_image_test_stubs.h"
#include "sys.h"
HostSerial Serial;

int main(int argc, char **argv)
{
    assert(argc == 3);
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
