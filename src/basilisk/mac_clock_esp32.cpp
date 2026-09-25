#include <Arduino.h>
#include "nvs.h"
#include <time.h>
#include "esp_timer.h"
#include "mac_clock.h"

// Build date/time as base for Mac clock
// Used only until the user first sets the guest clock.
static time_t get_build_timestamp(void) {
    // Parse __DATE__ (format: "Jan 17 2026") and __TIME__ (format: "14:30:00")
    static time_t build_time = 0;
    static bool initialized = false;

    if (!initialized) {
        const char *date_str = __DATE__;  // "Mmm DD YYYY"
        const char *time_str = __TIME__;  // "HH:MM:SS"

        // Month lookup
        const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        int month = 0;
        for (int i = 0; i < 12; i++) {
            if (strncmp(date_str, months[i], 3) == 0) {
                month = i;
                break;
            }
        }

        int day = atoi(date_str + 4);
        int year = atoi(date_str + 7);
        int hour = atoi(time_str);
        int minute = atoi(time_str + 3);
        int second = atoi(time_str + 6);

        struct tm tm_build;
        memset(&tm_build, 0, sizeof(tm_build));
        tm_build.tm_year = year - 1900;
        tm_build.tm_mon = month;
        tm_build.tm_mday = day;
        tm_build.tm_hour = hour;
        tm_build.tm_min = minute;
        tm_build.tm_sec = second;

        build_time = mktime(&tm_build);
        initialized = true;

        Serial.printf("[TIME] Build timestamp: %s %s -> %ld\n", date_str, time_str, (long)build_time);
    }

    return build_time;
}


static MacClock clock_state;
static bool initialized = false;
static bool user_set = false;
static bool dirty = false;
static uint64_t last_save_us = 0;
static uint64_t last_attempt_us = 0;
static bool save_attempted = false;

static void initialize_clock() {
    if (initialized) return;
    uint32_t seconds = static_cast<uint32_t>(get_build_timestamp() + 2082844800LL);
    nvs_handle_t handle;
    if (nvs_open("basilisk-clock", NVS_READONLY, &handle) == ESP_OK) {
        uint32_t saved_seconds;
        user_set = nvs_get_u32(handle, "seconds", &saved_seconds) == ESP_OK;
        if (user_set) seconds = saved_seconds;
        nvs_close(handle);
    }
    const uint64_t now = esp_timer_get_time();
    clock_state.set(seconds, now);
    last_save_us = now;
    initialized = true;
}

uint32_t TimerDateTime(void) {
    initialize_clock();
    return clock_state.read(esp_timer_get_time());
}

void TimerWriteRTCByte(unsigned reg, uint8_t value) {
    initialize_clock();
    if (clock_state.writeByte(reg, value, esp_timer_get_time())) {
        user_set = true;
        dirty = true;
    }
}

void MacClockSave(bool force) {
    if (!initialized || !user_set) return;
    const uint64_t now = esp_timer_get_time();
    // Checkpoint at most every five minutes unless the user changed the clock.
    // NVS commits are atomic, so an interrupted save keeps the previous value.
    if (!force && !dirty && now - last_save_us < 300000000ULL) return;
    // A full or unavailable NVS must not turn every CPU quantum into a write.
    if (!force && save_attempted && now - last_attempt_us < 1000000ULL) return;
    last_attempt_us = now;
    save_attempted = true;
    nvs_handle_t handle;
    if (nvs_open("basilisk-clock", NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_set_u32(handle, "seconds", clock_state.read(now)) == ESP_OK &&
            nvs_commit(handle) == ESP_OK) {
            dirty = false;
            last_save_us = now;
        }
        nvs_close(handle);
    }
}
