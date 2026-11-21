// Repair the five bytes audited in the published MicroMac7 HFS volume.
// System 7.5.3 predates the File Manager boot-volume check used by HfsCanDeferBootCheck.
#ifdef DISK_IMAGE_HOST_TEST
#include "disk_image_test_stubs.h"
#include "host_sha256.h"
#define BOARD_SD_MOUNT_POINT ""
#else
#include "sysdeps.h"
#include "board_config.h"
#include <mbedtls/sha256.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>

namespace {
constexpr uint32_t diskBytes = 1048576000;
// Boot blocks, primary MDB, allocation bitmap, complete extents and catalog
// trees. Also hash the alternate MDB and final reserved sector at end of disk.
constexpr uint32_t prefixBytes = 19 * 512 + 2 * 1048576;
constexpr uint8_t metadataHash[32] = {
    0x46,0x40,0x2d,0xbd,0xbd,0x6d,0xde,0xed,0xab,0xac,0xc3,0x04,0x34,0xf5,0x7d,0x45,
    0xc5,0x25,0xd5,0x53,0x87,0xcb,0xe3,0xc4,0x2f,0x63,0x94,0xec,0xe4,0xb5,0x3a,0x5b
};
struct Patch { uint32_t offset; uint8_t before, after; };
// Fsync and verify every edit in this order. Make the volume clean last.
constexpr Patch patches[] = {
    {1137333, 0x60, 0x61}, // A second, unthreaded Finder alias reused CNID 864. Assign 865.
    {1057, 0x61, 0x62},   // drNxtCNID: 865 -> 866.
    {1037, 3, 4},          // drNmFls: the four files in the root directory.
    {1111, 0xfc, 0xfd},    // drFilCnt: 508 -> 509.
    {1034, 0, 1}           // drAtrb: kHFSVolumeUnmountedBit.
};

bool readAt(int fd, uint32_t pos, uint8_t *data, size_t n)
{
    return lseek(fd, pos, SEEK_SET) == off_t(pos) && read(fd, data, n) == ssize_t(n);
}

enum class Repair { Unmatched, Complete, Error };
Repair repair(int fd, const uint8_t expected[32])
{
    uint8_t header[46];
    if (!readAt(fd, 1024, header, sizeof(header)) ||
        memcmp(header, "\x42\x44\xe5\x86\xd0\xff", 6) || header[10] || header[11] ||
        memcmp(header + 36, "\x09MicroMac7", 10)) return Repair::Unmatched;

    constexpr size_t chunk = 8192;
    uint8_t *buffer = static_cast<uint8_t *>(malloc(chunk));
    if (!buffer) return Repair::Error;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool ok = mbedtls_sha256_starts(&sha, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0;
    for (uint32_t pos = 0; ok && pos < prefixBytes; pos += chunk) {
        size_t n = prefixBytes - pos;
        if (n > chunk) n = chunk;
        ok = read(fd, buffer, n) == ssize_t(n);
        for (const Patch &p : patches) {
            if (!ok || p.offset < pos || p.offset - pos >= n) continue;
            uint8_t &value = buffer[p.offset - pos];
            // A reset after any completed edit can resume against the same pin.
            ok = value == p.before || value == p.after;
            value = p.before;
        }
        if (ok) ok = mbedtls_sha256_update(&sha, buffer, n) == 0;
    }
    uint8_t digest[32];
    if (ok) ok = readAt(fd, diskBytes - 1024, buffer, 1024) &&
                 mbedtls_sha256_update(&sha, buffer, 1024) == 0 &&
                 mbedtls_sha256_finish(&sha, digest) == 0;
    free(buffer);
    mbedtls_sha256_free(&sha);
    if (!ok || memcmp(digest, expected, sizeof(digest))) return Repair::Unmatched;

    for (const Patch &p : patches) {
        uint8_t value;
        if (!readAt(fd, p.offset, &value, 1)) return Repair::Error;
        if (value == p.after) continue;
        if (value != p.before || lseek(fd, p.offset, SEEK_SET) != off_t(p.offset) ||
            write(fd, &p.after, 1) != 1 || fsync(fd) ||
            !readAt(fd, p.offset, &value, 1) || value != p.after) return Repair::Error;
    }
    return Repair::Complete;
}
} // namespace

void Sys_repair_hfs7_starter(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext || strcasecmp(ext, ".dsk")) return;
    char fullPath[288];
    int n = snprintf(fullPath, sizeof(fullPath), "%s%s", BOARD_SD_MOUNT_POINT, path);
    if (n < 0 || size_t(n) >= sizeof(fullPath)) return;
    int fd = open(fullPath, O_RDWR);
    if (fd < 0) return;
    struct stat st;
    Repair result = Repair::Unmatched;
    if (!fstat(fd, &st) && st.st_size == diskBytes) result = repair(fd, metadataHash);
    if (close(fd) && result == Repair::Complete) result = Repair::Error;
    if (result == Repair::Complete)
        Serial.printf("[HFS BOOT] Repaired verified System 7.5.3 starter: %s\n", path);
    if (result == Repair::Error)
        Serial.printf("[HFS BOOT] System 7.5.3 repair I/O failed: %s\n", path);
}
