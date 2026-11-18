// Arduino File backed by a real host file. The disk backend is compiled as-is.
#pragma once
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/types.h>
#include "Arduino.h"

using uint8 = uint8_t;
#ifdef __APPLE__
using loff_t = int32_t;
#endif
#define UNUSED(x) ((void)(x))
inline constexpr auto FILE_READ = "rb";
inline constexpr int SeekEnd = SEEK_END, SeekSet = SEEK_SET;

class File {
public:
    File() : File(nullptr) {}
    explicit File(FILE *stream) : stream(stream, &fclose) {}
    explicit operator bool() const { return bool(stream); }
    size_t size() {
        auto here = position();
        seek(0, SeekEnd);
        auto end = position();
        seek(here);
        return end;
    }
    size_t position() { return ftell(stream.get()); }
    bool seek(size_t offset, int origin = SeekSet) { return !fseek(stream.get(), offset, origin); }
    size_t read(uint8_t *buf, size_t n) { return fread(buf, 1, n, stream.get()); }
    size_t write(const uint8_t *buf, size_t n) { return fwrite(buf, 1, n, stream.get()); }
    void flush() { fflush(stream.get()); }
    void close() { stream.reset(); }
private:
    std::unique_ptr<FILE, decltype(&fclose)> stream;
};
struct HostFileSystem {
    File open(const char *path, const char *mode) { return File(fopen(path, mode)); }
};
inline HostFileSystem SD_FS;
