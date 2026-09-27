#pragma once
#include "FreeRTOS.h"
struct FakeSemaphore { std::mutex mutex; std::condition_variable cv; int count; };
using SemaphoreHandle_t = FakeSemaphore *;
inline SemaphoreHandle_t makeSemaphore(int count) {
    if (fail_semaphore_after == 0) { fail_semaphore_after = -1; return nullptr; }
    if (fail_semaphore_after > 0) --fail_semaphore_after;
    auto *s = new FakeSemaphore; s->count = count; return s;
}
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return makeSemaphore(1); }
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return makeSemaphore(0); }
inline int xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
    assert(s);
    if (take_hook) { auto hook = std::move(take_hook); take_hook = {}; hook(); }
    std::unique_lock<std::mutex> lock(s->mutex);
    if (ticks == portMAX_DELAY) s->cv.wait(lock, [&]{ return s->count != 0; });
    else if (!s->cv.wait_for(lock, std::chrono::milliseconds(ticks), [&]{ return s->count != 0; })) return pdFALSE;
    --s->count; return pdTRUE;
}
inline int xSemaphoreGive(SemaphoreHandle_t s) {
    std::lock_guard<std::mutex> lock(s->mutex); ++s->count; s->cv.notify_one(); return pdTRUE;
}
inline void vSemaphoreDelete(SemaphoreHandle_t s) { delete s; }
struct StaticSemaphore_t { std::recursive_mutex recursive; };
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *s) { return reinterpret_cast<SemaphoreHandle_t>(s); }
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t) { reinterpret_cast<StaticSemaphore_t *>(s)->recursive.lock(); return pdTRUE; }
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t s) { reinterpret_cast<StaticSemaphore_t *>(s)->recursive.unlock(); return pdTRUE; }
