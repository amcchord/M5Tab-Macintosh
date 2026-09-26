#ifndef BOOT_CONTROL_VALUES_H
#define BOOT_CONTROL_VALUES_H

#include <stddef.h>
#include <string.h>

// Hex keeps filenames, spaces and UTF-8 independent of the line protocol.
// A dash represents an empty value. Settings are one line each on the SD card.
static inline int BootControlHexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline bool BootControlDecode(const char *hex, char *out, size_t capacity)
{
    if (!hex || !out || !capacity) return false;
    if (strcmp(hex, "-") == 0) { out[0] = 0; return true; }
    size_t n = strlen(hex);
    if (!n || n % 2 || n / 2 >= capacity) return false;
    for (size_t i = 0; i < n; i += 2) {
        int hi = BootControlHexDigit(hex[i]), lo = BootControlHexDigit(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        unsigned char ch = (unsigned char)((hi << 4) | lo);
        if (ch < 32 || ch == 127) return false;
        out[i / 2] = (char)ch;
    }
    out[n / 2] = 0;
    return true;
}

static inline bool BootControlBool(const char *value, bool *out)
{
    if (!strcmp(value, "true")) { *out = true; return true; }
    if (!strcmp(value, "false")) { *out = false; return true; }
    return false;
}

static inline bool BootControlChoice(const char *value, bool ram, int *out)
{
    const char *choices[] = {ram ? "4" : "0", ram ? "8" : "180", "12", "16"};
    const int numbers[] = {ram ? 4 : 0, ram ? 8 : 180, 12, 16};
    for (int i = 0; i < (ram ? 4 : 2); ++i) {
        if (!strcmp(value, choices[i])) { *out = numbers[i]; return true; }
    }
    return false;
}
#endif
