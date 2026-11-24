/*
 *  compemu.h - JIT compiler stub for ESP32 (no JIT support)
 *
 *  Basilisk II ESP32 Port
 */

#ifndef COMPEMU_H
#define COMPEMU_H

// JIT compiler is not supported on ESP32
#undef USE_JIT

// Stub definitions for JIT-related functions and variables
static inline void compiler_init(void) {}
static inline void compiler_exit(void) {}
// The guest's cache instructions (CINV, CPUSH) end up here. Translated RAM
// code (src/basilisk/aot) checks its bytes again after a flush.
extern void AotCodeChanged(void);
extern void AotCodeChangedGuestRange(uaecptr addr, uae_u32 size);
extern void AotHandleCpusha(void);
static inline void flush_icache(int n) { (void)n; AotCodeChanged(); }
static inline void flush_icache_range(uint8 *start, uint32 length) { (void)start; (void)length; AotCodeChanged(); }
static inline void flush_icache_guest_range(uaecptr addr, uae_u32 length) { AotCodeChangedGuestRange(addr, length); }
static inline void flush_icache_cpusha(void) { AotHandleCpusha(); }

// JIT cache pointers (stubs)
#define compiled_code NULL
#define current_compile_p NULL

// JIT control flags
#define JIT_ACTIVE 0
#define use_jit 0

#endif /* COMPEMU_H */
