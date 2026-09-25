#pragma once
#include <stdint.h>

// Macintosh wall time is local time, in unsigned seconds since 1904.
// Keep it separate from host timers so editing the clock cannot move deadlines.
class MacClock {
public:
    void set(uint32_t seconds, uint64_t now_us) {
        base_seconds = seconds;
        base_us = now_us;
        written_mask = 0;
    }
    uint32_t read(uint64_t now_us) const {
        return base_seconds + static_cast<uint32_t>((now_us - base_us) / 1000000);
    }
    bool writeByte(unsigned reg, uint8_t value, uint64_t now_us) {
        const unsigned index = reg & 3;
        const unsigned shift = index * 8;
        pending = (pending & ~(uint32_t(0xff) << shift)) | (uint32_t(value) << shift);
        written_mask |= 1u << index;
        if (written_mask != 0x0f) return false;
        set(pending, now_us);
        return true;
    }
private:
    uint32_t base_seconds = 0;
    uint64_t base_us = 0;
    uint32_t pending = 0;
    unsigned written_mask = 0;
};

uint32_t TimerDateTime(void);
void TimerWriteRTCByte(unsigned reg, uint8_t value);
// Called on the emulator task, outside the RTC trap; force on clean shutdown.
void MacClockSave(bool force = false);
