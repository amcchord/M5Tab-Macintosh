/*
 *  perf_sampler.cpp - Diagnostic statistical profiler for the emulator core
 *
 *  See perf_sampler.h. The timer, its interrupt and all counter reads that
 *  are per-hart (mcycle/minstret) live on the emulator core. Control calls
 *  come from the automation task and hand timer ownership to a short-lived
 *  helper task pinned to that core, because the interrupt allocator binds a
 *  timer interrupt to the core that installs it.
 */

#include "sysdeps.h"
#include "perf_sampler.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gptimer.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "soc/cache_reg.h"
#include "soc/soc.h"

#include "cpu_emulation.h"
#include "uae_cpu/memory.h"
#include "uae_cpu/newcpu.h"

extern uint64_t EmulatorInstructionCount(void);

#define PERF_EMULATOR_CORE 1

namespace {

gptimer_handle_t s_timer = NULL;
PerfSample *s_samples = NULL;
volatile uint32_t s_capacity = 0;
volatile uint32_t s_count = 0;
volatile uint32_t s_dropped = 0;
volatile uint32_t s_hz = 0;
volatile bool s_running = false;
volatile int s_core = PERF_EMULATOR_CORE;  // core whose interrupted PCs are sampled

// Counter bookkeeping. `start` is taken by the first interrupt after start
// and `stop` by the control path after the timer stops. Cache counters are
// only 32 bits, so the interrupt folds them into 64-bit totals regularly.
volatile bool s_need_start_snapshot = false;
volatile bool s_need_stop_snapshot = false;
uint64_t s_start_cycles, s_start_instret, s_start_m68k, s_start_us;
uint64_t s_stop_cycles, s_stop_instret, s_stop_m68k, s_stop_us;
#define PERF_CACHE_COUNTERS 14
uint32_t s_cache_last[PERF_CACHE_COUNTERS];
uint64_t s_cache_total[PERF_CACHE_COUNTERS];
uint32_t s_fold_countdown = 0;

// Emulator core (bus 1) first, then the other core's traffic into the shared
// L1 data cache and L2, which competes with the emulator for capacity.
const uint32_t kCacheRegs[PERF_CACHE_COUNTERS] = {
    CACHE_L1_IBUS1_ACS_HIT_CNT_REG,  CACHE_L1_IBUS1_ACS_MISS_CNT_REG,
    CACHE_L1_DBUS1_ACS_HIT_CNT_REG,  CACHE_L1_DBUS1_ACS_MISS_CNT_REG,
    CACHE_L2_IBUS1_ACS_HIT_CNT_REG,  CACHE_L2_IBUS1_ACS_MISS_CNT_REG,
    CACHE_L2_DBUS1_ACS_HIT_CNT_REG,  CACHE_L2_DBUS1_ACS_MISS_CNT_REG,
    CACHE_L1_IBUS0_ACS_MISS_CNT_REG,
    CACHE_L1_DBUS0_ACS_HIT_CNT_REG,  CACHE_L1_DBUS0_ACS_MISS_CNT_REG,
    CACHE_L2_IBUS0_ACS_MISS_CNT_REG,
    CACHE_L2_DBUS0_ACS_HIT_CNT_REG,  CACHE_L2_DBUS0_ACS_MISS_CNT_REG,
};

const char *const kCounterNames[PERF_COUNTER_COUNT] = {
    "cycles", "instret", "m68k", "l1i_hit", "l1i_miss", "l1d_hit",
    "l1d_miss", "l2i_hit", "l2i_miss", "l2d_hit", "l2d_miss",
    "c0_l1i_miss", "c0_l1d_hit", "c0_l1d_miss", "c0_l2i_miss", "c0_l2d_hit",
    "c0_l2d_miss", "us",
};

inline uint64_t readCycles(void)
{
    uint32_t hi, lo, hi2;
    do {
        asm volatile("csrr %0, mcycleh" : "=r"(hi));
        asm volatile("csrr %0, mcycle" : "=r"(lo));
        asm volatile("csrr %0, mcycleh" : "=r"(hi2));
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

inline uint64_t readInstret(void)
{
    uint32_t hi, lo, hi2;
    do {
        asm volatile("csrr %0, minstreth" : "=r"(hi));
        asm volatile("csrr %0, minstret" : "=r"(lo));
        asm volatile("csrr %0, minstreth" : "=r"(hi2));
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

void cacheCountersReset(void)
{
    const uint32_t l1_ena = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5);  // ibus0/1, dbus0/1
    const uint32_t l1_clr = l1_ena << 16;
    const uint32_t l2_ena = (1u << 8) | (1u << 9) | (1u << 12) | (1u << 13);
    const uint32_t l2_clr = l2_ena << 16;
    REG_WRITE(CACHE_L1_CACHE_ACS_CNT_CTRL_REG, l1_ena | l1_clr);
    REG_WRITE(CACHE_L1_CACHE_ACS_CNT_CTRL_REG, l1_ena);
    REG_WRITE(CACHE_L2_CACHE_ACS_CNT_CTRL_REG, l2_ena | l2_clr);
    REG_WRITE(CACHE_L2_CACHE_ACS_CNT_CTRL_REG, l2_ena);
    for (int i = 0; i < PERF_CACHE_COUNTERS; ++i) {
        s_cache_last[i] = REG_READ(kCacheRegs[i]);
        s_cache_total[i] = 0;
    }
}

void cacheCountersFold(void)
{
    for (int i = 0; i < PERF_CACHE_COUNTERS; ++i) {
        const uint32_t now = REG_READ(kCacheRegs[i]);
        s_cache_total[i] += (uint32_t)(now - s_cache_last[i]);
        s_cache_last[i] = now;
    }
}

bool IRAM_ATTR onAlarm(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *)
{
    uint32_t mepc;
    asm volatile("csrr %0, mepc" : "=r"(mepc));

    if (s_need_start_snapshot) {
        s_need_start_snapshot = false;
        s_start_cycles = readCycles();
        s_start_instret = readInstret();
        s_start_m68k = EmulatorInstructionCount();
        s_start_us = (uint64_t)esp_timer_get_time();
        cacheCountersReset();
        s_fold_countdown = 64;
        return false;
    }
    if (s_need_stop_snapshot) {
        s_need_stop_snapshot = false;
        s_stop_cycles = readCycles();
        s_stop_instret = readInstret();
        s_stop_m68k = EmulatorInstructionCount();
        s_stop_us = (uint64_t)esp_timer_get_time();
        cacheCountersFold();
        s_running = false;
        return false;
    }
    if (!s_running) return false;

    if (--s_fold_countdown == 0) {
        s_fold_countdown = 64;
        cacheCountersFold();
    }

    const uint32_t n = s_count;
    if (n >= s_capacity) {
        s_dropped = s_dropped + 1;
        return false;
    }

    // The FreeRTOS port stores the interrupted frame pointer in the current
    // TCB's first word on the outermost interrupt. A nested interrupt leaves
    // the outer frame there, which the flag below exposes.
    uint32_t frame_pc = 0, frame_ra = 0;
    TaskHandle_t current = xTaskGetCurrentTaskHandleForCore(s_core);
    if (current) {
        const uint32_t *frame = *(const uint32_t *const *)current;
        if (frame) {
            frame_pc = frame[0];
            frame_ra = frame[1];
        }
    }

    PerfSample *sample = &s_samples[n];
    sample->host_pc = mepc;
    sample->host_ra = frame_ra;
    uae_u8 *pc_p = regs.pc_p;
    sample->m68k_pc = regs.pc + (uint32_t)(pc_p - regs.pc_oldp);
    sample->opcode = pc_p ? *(const uint16_t *)pc_p : 0;
    sample->flags = (frame_pc == mepc) ? 0 : 1;
    s_count = n + 1;
    return false;
}

struct ControlJob {
    SemaphoreHandle_t done;
    bool start;
    uint32_t hz;
    esp_err_t result;
};

void controlTask(void *arg)
{
    ControlJob *job = (ControlJob *)arg;
    esp_err_t err = ESP_OK;
    if (job->start) {
        gptimer_config_t config = {};
        config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
        config.direction = GPTIMER_COUNT_UP;
        config.resolution_hz = 1000000;
        err = gptimer_new_timer(&config, &s_timer);
        if (err == ESP_OK) {
            gptimer_event_callbacks_t callbacks = {};
            callbacks.on_alarm = onAlarm;
            err = gptimer_register_event_callbacks(s_timer, &callbacks, NULL);
        }
        if (err == ESP_OK) {
            gptimer_alarm_config_t alarm = {};
            alarm.alarm_count = 1000000 / job->hz;
            alarm.reload_count = 0;
            alarm.flags.auto_reload_on_alarm = 1;
            err = gptimer_set_alarm_action(s_timer, &alarm);
        }
        if (err == ESP_OK) err = gptimer_enable(s_timer);
        if (err == ESP_OK) err = gptimer_start(s_timer);
        if (err != ESP_OK && s_timer) {
            gptimer_del_timer(s_timer);
            s_timer = NULL;
        }
    } else if (s_timer) {
        gptimer_stop(s_timer);
        gptimer_disable(s_timer);
        gptimer_del_timer(s_timer);
        s_timer = NULL;
    }
    job->result = err;
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

esp_err_t runOnEmulatorCore(bool start, uint32_t hz)
{
    ControlJob job = {};
    job.done = xSemaphoreCreateBinary();
    if (!job.done) return ESP_ERR_NO_MEM;
    job.start = start;
    job.hz = hz;
    job.result = ESP_FAIL;
    if (xTaskCreatePinnedToCore(controlTask, "perf_ctl", 4096, &job,
                                configMAX_PRIORITIES - 2, NULL,
                                s_core) != pdPASS) {
        vSemaphoreDelete(job.done);
        return ESP_ERR_NO_MEM;
    }
    const bool finished = xSemaphoreTake(job.done, pdMS_TO_TICKS(2000)) == pdTRUE;
    vSemaphoreDelete(job.done);
    return finished ? job.result : ESP_ERR_TIMEOUT;
}

}  // namespace

bool PerfSamplerStart(uint32_t hz, uint32_t capacity, int core, char *error, size_t error_size)
{
    if (core != 0 && core != 1) {
        snprintf(error, error_size, "bad_core");
        return false;
    }
    if (s_timer) {
        snprintf(error, error_size, "running");
        return false;
    }
    if (hz < 10 || hz > 20000 || capacity == 0 || capacity > 262144) {
        snprintf(error, error_size, "bad_args");
        return false;
    }
    if (s_samples && s_capacity != capacity) PerfSamplerRelease();
    if (!s_samples) {
        s_samples = (PerfSample *)heap_caps_malloc(capacity * sizeof(PerfSample),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_samples) {
            snprintf(error, error_size, "no_memory");
            return false;
        }
    }
    s_core = core;
    s_capacity = capacity;
    s_count = 0;
    s_dropped = 0;
    s_hz = hz;
    s_need_stop_snapshot = false;
    s_running = true;
    s_need_start_snapshot = true;
    const esp_err_t err = runOnEmulatorCore(true, hz);
    if (err != ESP_OK) {
        s_running = false;
        snprintf(error, error_size, "timer_%s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void PerfSamplerStop(void)
{
    if (!s_timer) return;
    s_need_stop_snapshot = true;
    const uint32_t deadline = millis() + 500;
    while (s_need_stop_snapshot && (int32_t)(deadline - millis()) > 0) {
        vTaskDelay(1);
    }
    runOnEmulatorCore(false, 0);
    s_running = false;
}

void PerfSamplerRelease(void)
{
    PerfSamplerStop();
    if (s_samples) heap_caps_free(s_samples);
    s_samples = NULL;
    s_capacity = 0;
    s_count = 0;
}

void PerfSamplerReadState(PerfSamplerState *state)
{
    memset(state, 0, sizeof(*state));
    state->running = s_timer != NULL;
    state->hz = s_hz;
    state->capacity = s_capacity;
    state->count = s_count;
    state->dropped = s_dropped;
    if (state->running) return;
    state->counters[PERF_COUNTER_CYCLES] = s_stop_cycles - s_start_cycles;
    state->counters[PERF_COUNTER_INSTRET] = s_stop_instret - s_start_instret;
    state->counters[PERF_COUNTER_M68K_INSTRUCTIONS] = s_stop_m68k - s_start_m68k;
    for (int i = 0; i < PERF_CACHE_COUNTERS; ++i) {
        state->counters[PERF_COUNTER_L1I_HIT + i] = s_cache_total[i];
    }
    state->counters[PERF_COUNTER_ELAPSED_US] = s_stop_us - s_start_us;
}

uint32_t PerfSamplerRead(uint32_t offset, PerfSample *samples, uint32_t max_samples)
{
    if (!s_samples || s_timer) return 0;
    const uint32_t count = s_count;
    if (offset >= count) return 0;
    uint32_t n = count - offset;
    if (n > max_samples) n = max_samples;
    memcpy(samples, &s_samples[offset], n * sizeof(PerfSample));
    return n;
}

const char *PerfSamplerCounterName(int index)
{
    if (index < 0 || index >= PERF_COUNTER_COUNT) return "?";
    return kCounterNames[index];
}
