#pragma once

#include <stdint.h>
#include <stdio.h>

// Count only new HTTP body bytes. Range offsets and bytes read from FAT do
// not enter this counter. Keep roughly three seconds of recent observations.
class StarterNetworkSpeed {
public:
    void start(uint32_t now) {
        head = 0;
        count = 1;
        received = 0;
        last_byte = now;
        history[0] = {now, 0};
    }
    void stop() { count = 0; }

    void receivedBytes(uint32_t bytes, uint32_t now) {
        if (!count || !bytes) return;
        if (now - last_byte >= stall_ms) start(now);
        received += bytes;
        last_byte = now;
        if (now - history[head].ms >= sample_ms) {
            head = (head + 1) % slots;
            if (count < slots) ++count;
            history[head] = {now, received};
        }
    }

    int32_t bytesPerSecond(uint32_t now) const {
        if (!count) return -1;
        if (now - last_byte >= stall_ms) return 0;
        const auto &oldest = history[(head + slots + 1 - count) % slots];
        const uint32_t elapsed = now - oldest.ms;
        if (elapsed < sample_ms) return -1;
        return (int32_t)((uint64_t)(received - oldest.bytes) * 1000 / elapsed);
    }

private:
    static constexpr unsigned slots = 5;
    static constexpr uint32_t sample_ms = 750, stall_ms = 3000;
    struct Sample { uint32_t ms, bytes; } history[slots] = {};
    unsigned head = 0, count = 0;
    uint32_t received = 0, last_byte = 0;
};

inline void StarterFormatSpeed(char *out, size_t size, uint32_t bps) {
    if (bps < 1024) {
        snprintf(out, size, "%u B/s", (unsigned)bps);
        return;
    }
    const bool mib = bps >= 1024 * 1024;
    const uint32_t unit = mib ? 1024 * 1024 : 1024;
    const uint32_t tenths = ((uint64_t)bps * 10 + unit / 2) / unit;
    snprintf(out, size, "%u.%u %s/s", (unsigned)(tenths / 10), (unsigned)(tenths % 10),
             mib ? "MiB" : "KiB");
}
