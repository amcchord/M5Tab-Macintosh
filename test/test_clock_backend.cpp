#include "mac_clock.h"
#include "nvs.h"
#include "esp_timer.h"
#include <assert.h>
#include <string.h>

static void set_date(uint32_t seconds) {
    for (unsigned i = 0; i < 4; ++i)
        TimerWriteRTCByte(i, uint8_t(seconds >> (8 * i)));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (strcmp(argv[1], "empty") == 0) {
        uint32_t initial = TimerDateTime();
        test_now_us += 1000000;
        assert(TimerDateTime() == initial + 1);
        MacClockSave(true);
        assert(test_write_opens == 0); // No flash wear until manually set.
    } else if (strcmp(argv[1], "saved") == 0) {
        test_has_value = true;
        test_saved = 0; // A saved 1904 date is not "no clock".
        assert(TimerDateTime() == 0);
        test_now_us += 2000000;
        assert(TimerDateTime() == 2);
        MacClockSave(true);
        assert(test_saved == 2);
    } else {
        set_date(1234567890);
        assert(TimerDateTime() == 1234567890);
        MacClockSave();
        assert(test_saved == 1234567890);
        assert(test_commits == 1);
        test_now_us += 299000000;
        MacClockSave();
        assert(test_commits == 1);
        test_now_us += 1000000;
        MacClockSave();
        assert(test_commits == 2 && test_saved == 1234568190);
        test_now_us += 1000000;
        set_date(42);
        test_fail_commit = true;
        MacClockSave();
        assert(test_commits == 3 && test_saved == 1234568190);
        for (int i = 0; i < 100; ++i) MacClockSave();
        assert(test_commits == 3 && test_write_opens == 3);
        test_fail_commit = false;
        test_now_us += 1000000;
        MacClockSave();
        assert(test_commits == 4 && test_saved == 43); // Failed save retried.
    }
    return 0;
}
