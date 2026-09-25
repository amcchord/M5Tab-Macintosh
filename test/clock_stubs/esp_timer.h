#pragma once
#include <stdint.h>
inline int64_t test_now_us = 1000000;
inline int64_t esp_timer_get_time() { return test_now_us; }
