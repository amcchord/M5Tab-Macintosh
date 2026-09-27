#pragma once
#include "FreeRTOS.h"
#include <atomic>
struct FakeTask { std::thread thread; std::atomic<unsigned> notifications{0}; };
using TaskHandle_t = FakeTask *;
inline std::vector<std::unique_ptr<FakeTask>> fake_tasks;
inline thread_local TaskHandle_t current_task;
inline std::atomic<uint32_t> test_ms{0};
inline uint32_t millis() { return test_ms.load(); }
inline uint32_t micros() { return millis() * 1000; }
inline TickType_t xTaskGetTickCount() { return millis(); }
inline void vTaskDelay(TickType_t ticks) { std::this_thread::sleep_for(std::chrono::milliseconds(ticks)); }
inline void taskYIELD() { std::this_thread::yield(); }
inline void xTaskNotifyGive(TaskHandle_t task) { ++task->notifications; }
inline uint32_t ulTaskNotifyTake(int, TickType_t ticks) {
    auto n = current_task->notifications.exchange(0);
    if (!n) vTaskDelay(ticks);
    return n;
}
inline int xTaskCreatePinnedToCore(void (*fn)(void *), const char *, int, void *arg, int, TaskHandle_t *out, int) {
    if (fail_task_create) return 0;
    auto task = std::make_unique<FakeTask>(); *out = task.get();
    task->thread = std::thread([ptr=task.get(),fn,arg]{ current_task=ptr; fn(arg); });
    fake_tasks.push_back(std::move(task)); return pdPASS;
}
inline void vTaskDelete(void *) {}
inline void joinTasks() { for (auto &task: fake_tasks) task->thread.join(); fake_tasks.clear(); }
