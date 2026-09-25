#include "mac_clock.h"
#include "guest_range.h"
#include <assert.h>
#include <stdint.h>

int main() {
    MacClock clock;
    clock.set(2082844800u, 0);  // 1970 is a valid guest date.
    assert(clock.read(2000000) == 2082844802u);
    // A clock write is four bytes in either ROM register order. Reads
    // between writes must never observe a partially replaced timestamp.
    const uint32_t date = 1234567890u;  // Before the old 2020 cutoff.
    for (unsigned i = 0; i < 4; ++i) {
        bool committed = clock.writeByte(i, uint8_t(date >> (8 * i)), 3000000);
        assert(committed == (i == 3));
        if (!committed) assert(clock.read(3000000) == 2082844803u);
    }
    assert(clock.read(4000000) == date + 1);
    // RTC registers 4..7 alias 0..3. Accept high-byte-first writes too.
    for (int i = 3; i >= 0; --i)
        assert(clock.writeByte(i + 4, 0, 5000000) == (i == 0));
    assert(clock.read(5000000) == 0);  // 1904, not "unset".
    clock.set(100, 0);
    assert(clock.read(5000000000000ULL) == 5000100u); // Beyond millis() wrap.
    clock.set(UINT32_MAX, 0);
    assert(clock.read(1000000) == 0);  // Native 32-bit Mac epoch rollover.

    const uint32_t ram = 8 * 1024 * 1024;
    assert(GuestRangeContains(0, 24, 0, ram));
    assert(GuestRangeContains(ram - 4096, 4096, 0, ram));
    assert(!GuestRangeContains(ram - 4095, 4096, 0, ram));
    assert(!GuestRangeContains(ram, 1, 0, ram));
    assert(!GuestRangeContains(UINT32_MAX - 10, 24, 0, ram));
    assert(!GuestRangeContains(0, UINT32_MAX, 0, ram));
    assert(GuestRangeContains(0x40800000, 4096, 0x40800000, 1024 * 1024));
    assert(!GuestRangeContains(0x407fffff, 24, 0x40800000, 1024 * 1024));
    return 0;
}
