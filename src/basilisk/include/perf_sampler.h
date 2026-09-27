/*
 *  perf_sampler.h - Diagnostic statistical profiler for the emulator core
 *
 *  A periodic timer interrupt pinned to the sampled core records the
 *  interrupted host PC/RA together with the 68k PC and opcode. Hardware
 *  cycle, retired-instruction and cache counters are captured for the same
 *  window. Sampling is off unless explicitly started over the automation
 *  protocol; the sample buffer lives in PSRAM only while a session exists.
 *
 *  The cache "hit" registers count accesses, but calibration with known
 *  access patterns showed the "miss" registers advance roughly once per
 *  stall cycle rather than once per line fill (a 1 MB PSRAM stream reads
 *  ~160 misses per 64-byte line). Treat misses as a stall-time proxy.
 */

#ifndef PERF_SAMPLER_H
#define PERF_SAMPLER_H

#include <stddef.h>
#include <stdint.h>

struct PerfSample {
    uint32_t host_pc;
    uint32_t host_ra;
    uint32_t m68k_pc;
    uint16_t opcode;   // Raw (host byte order) 68k opcode word at pc_p.
    uint16_t flags;    // Bit 0: interrupt frame did not match mepc.
};

enum {
    PERF_COUNTER_CYCLES = 0,
    PERF_COUNTER_INSTRET,
    PERF_COUNTER_M68K_INSTRUCTIONS,
    PERF_COUNTER_L1I_HIT,
    PERF_COUNTER_L1I_MISS,
    PERF_COUNTER_L1D_HIT,
    PERF_COUNTER_L1D_MISS,
    PERF_COUNTER_L2I_HIT,
    PERF_COUNTER_L2I_MISS,
    PERF_COUNTER_L2D_HIT,
    PERF_COUNTER_L2D_MISS,
    PERF_COUNTER_C0_L1I_MISS,
    PERF_COUNTER_C0_L1D_HIT,
    PERF_COUNTER_C0_L1D_MISS,
    PERF_COUNTER_C0_L2I_MISS,
    PERF_COUNTER_C0_L2D_HIT,
    PERF_COUNTER_C0_L2D_MISS,
    PERF_COUNTER_ELAPSED_US,
    PERF_COUNTER_COUNT
};

struct PerfSamplerState {
    bool running;
    uint32_t hz;
    uint32_t capacity;
    uint32_t count;
    uint32_t dropped;
    uint64_t counters[PERF_COUNTER_COUNT];
};

// Samples `core`; the cycle/instret counters are that core's, and the m68k
// fields are meaningful only for the emulator core (1).
bool PerfSamplerStart(uint32_t hz, uint32_t capacity, int core, char *error, size_t error_size);
void PerfSamplerStop(void);
void PerfSamplerRelease(void);
void PerfSamplerReadState(PerfSamplerState *state);
uint32_t PerfSamplerRead(uint32_t offset, PerfSample *samples, uint32_t max_samples);
const char *PerfSamplerCounterName(int index);

#endif
