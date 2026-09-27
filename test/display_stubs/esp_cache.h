#pragma once
#include <cstddef>
#include <functional>
using esp_err_t = int;
#define ESP_OK 0
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED 2
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE 4
inline int cache_result = ESP_OK, cache_calls = 0;
inline void *cache_address = nullptr;
inline size_t cache_size = 0;
inline std::function<void()> cache_hook;
inline esp_err_t esp_cache_msync(void *address, size_t size, int) {
    ++cache_calls; cache_address=address; cache_size=size;
    if (cache_hook) { auto hook=std::move(cache_hook); cache_hook={}; hook(); }
    return cache_result;
}
inline const char *esp_err_to_name(int) { return "test"; }
