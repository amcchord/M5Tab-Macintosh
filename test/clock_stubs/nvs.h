#pragma once
#include <stdint.h>
using nvs_handle_t = int;
constexpr int ESP_OK = 0, NVS_READONLY = 0, NVS_READWRITE = 1;
inline bool test_has_value = false, test_fail_commit = false;
inline uint32_t test_saved = 0, test_pending = 0;
inline unsigned test_commits = 0, test_write_opens = 0;
inline int nvs_open(const char *, int mode, nvs_handle_t *handle) {
    if (mode == NVS_READONLY && !test_has_value) return 1;
    if (mode == NVS_READWRITE) ++test_write_opens;
    *handle = 1;
    return ESP_OK;
}
inline int nvs_get_u32(nvs_handle_t, const char *, uint32_t *value) {
    if (!test_has_value) return 1;
    *value = test_saved;
    return ESP_OK;
}
inline int nvs_set_u32(nvs_handle_t, const char *, uint32_t value) {
    test_pending = value;
    return ESP_OK;
}
inline int nvs_commit(nvs_handle_t) {
    ++test_commits;
    if (test_fail_commit) return 1;
    test_saved = test_pending;
    test_has_value = true;
    return ESP_OK;
}
inline void nvs_close(nvs_handle_t) {}
