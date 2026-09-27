/*
 *  sysdeps.h - System dependent definitions for ESP32-P4
 *
 *  BasiliskII ESP32 Port
 *  Based on Basilisk II (C) 1997-2008 Christian Bauer
 */

#ifndef SYSDEPS_H
#define SYSDEPS_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// FreeRTOS for mutex support
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// C++ STL headers needed by BasiliskII
#include <vector>
#include <map>
using std::vector;
// Note: Don't use "using std::map" as it conflicts with Arduino's map() function
// Use std::map<> explicitly in code instead

// Include ESP32 user strings
#include "user_strings_esp32.h"

/*
 * CPU and addressing mode configuration
 */

// Using 68k emulator (not native 68k CPU)
#define EMULATED_68K 1

// Mac and host address space are distinct (virtual addressing)
#define REAL_ADDRESSING 0

// Use bank-based memory access (DIRECT_ADDRESSING requires contiguous memory layout)
#define DIRECT_ADDRESSING 0

// ROM is write protected in virtual addressing mode
#define ROM_IS_WRITE_PROTECTED 1

// No prefetch buffer needed
#define USE_PREFETCH_BUFFER 0

// ExtFS (shared host folder mounted as a Mac volume) is supported on ESP32.
// The host backend lives in extfs_esp32.cpp and stores Finder info + resource
// forks as .finf/ and .rsrc/ sidecar dirs since FAT/exFAT has no xattrs.
#define SUPPORTS_EXTFS 1

// No UDP tunnel support
#define SUPPORTS_UDP_TUNNEL 0

// Use CPU emulation for periodic tasks (no threads)
#define USE_CPU_EMUL_SERVICES 1

/*
 * ESP32-P4 is little-endian RISC-V
 */
#undef WORDS_BIGENDIAN

/*
 * Data type sizes for ESP32-P4
 */
#define SIZEOF_SHORT 2
#define SIZEOF_INT 4
#define SIZEOF_LONG 4
#define SIZEOF_LONG_LONG 8
#define SIZEOF_VOID_P 4
#define SIZEOF_FLOAT 4
#define SIZEOF_DOUBLE 8

/*
 * Basic data types
 */
typedef uint8_t uint8;
typedef int8_t int8;
typedef uint16_t uint16;
typedef int16_t int16;
typedef uint32_t uint32;
typedef int32_t int32;
typedef uint64_t uint64;
typedef int64_t int64;
typedef uint32_t uintptr;
typedef int32_t intptr;

// File offset type
typedef int32_t loff_t;

// Character address type
typedef char* caddr_t;

// Time data type for timer emulation
typedef uint64_t tm_time_t;

/*
 * UAE CPU data types
 */
typedef int8 uae_s8;
typedef uint8 uae_u8;
typedef int16 uae_s16;
typedef uint16 uae_u16;
typedef int32 uae_s32;
typedef uint32 uae_u32;
typedef int64 uae_s64;
typedef uint64 uae_u64;
typedef uae_u32 uaecptr;

/*
 * ESP32-P4 RISC-V does NOT support unaligned memory access safely
 */
#undef CPU_CAN_ACCESS_UNALIGNED

/*
 * 64-bit value macros
 */
#define VAL64(a) (a ## LL)
#define UVAL64(a) (a ## ULL)

/*
 * Memory pointer type for Mac addresses
 */
#define memptr uint32

/*
 * Float format
 */
#define IEEE_FLOAT_FORMAT 1
#define HOST_FLOAT_FORMAT IEEE_FLOAT_FORMAT

/*
 * Inline hints - must be defined before use
 */
#define __inline__ inline
#define ALWAYS_INLINE inline __attribute__((always_inline))

/*
 * Small globals read on every emulated instruction live in .sdata beside the
 * global pointer, where the linker relaxes each access to one gp-relative
 * instruction instead of a lui/addi pair.
 */
#if defined(__riscv)
#define SDATA_ATTR(name) __attribute__((section(".sdata." name)))
#else
#define SDATA_ATTR(name)
#endif

/*
 * Byte swapping functions for little-endian ESP32 accessing big-endian Mac data
 *
 * The ESP32-P4 core has no Zbb byte-reverse instruction, so GCC lowers the
 * bswap builtins to a libgcc call that lands in mask ROM. Every guest memory
 * access paid that call. These shift/mask forms stay inline and use only
 * immediates, so they also schedule well inside the opcode handlers.
 */
static ALWAYS_INLINE uae_u32 do_byteswap_32(uae_u32 v) {
    return (v << 24) | ((v >> 8 & 0xff) << 16) | ((v >> 16 & 0xff) << 8) | (v >> 24);
}

static ALWAYS_INLINE uae_u16 do_byteswap_16(uae_u16 v) {
    return (uae_u16)((v << 8) | (v >> 8));
}

// Big-endian guest memory is accessed a byte at a time. On this core that is
// shorter than a word access plus a shift/mask swap, and 68k long/word
// operands at 2-byte alignment never become misaligned host accesses.
// GCC's bswap pass would otherwise rebuild these byte loads into a word
// load plus an expensive swap sequence; an empty asm hides the provenance
// of the high byte(s) at no cost.
#if defined(__riscv)
#define UAE_OPAQUE(x) __asm__("" : "+r"(x))
#else
#define UAE_OPAQUE(x) ((void)0)
#endif

static ALWAYS_INLINE uae_u32 do_get_mem_long(uae_u32 *a) {
    const uae_u8 *p = (const uae_u8 *)a;
    uae_u32 b0 = p[0], b2 = p[2];
    UAE_OPAQUE(b0);
    UAE_OPAQUE(b2);
    return (b0 << 24) | ((uae_u32)p[1] << 16) | (b2 << 8) | p[3];
}

static ALWAYS_INLINE uae_u32 do_get_mem_word(uae_u16 *a) {
    const uae_u8 *p = (const uae_u8 *)a;
    uae_u32 b0 = p[0];
    UAE_OPAQUE(b0);
    return (b0 << 8) | p[1];
}

/*
 * Fast opcode fetch path:
 * On little-endian hosts, opcode words in emulated memory are stored byte-swapped.
 * Expose the raw word so the CPU core can skip per-instruction bswap and instead
 * use swapped opcode tables/bit extraction paths.
 */
#define HAVE_GET_WORD_UNSWAPPED 1
static ALWAYS_INLINE uae_u32 do_get_mem_word_unswapped(const uae_u8 *a) {
    return *(const uae_u16 *)a;
}

// Get 8-bit value from memory
#define do_get_mem_byte(a) ((uae_u32)*((uae_u8 *)(a)))

static ALWAYS_INLINE void do_put_mem_long(uae_u32 *a, uae_u32 v) {
    uae_u8 *p = (uae_u8 *)a;
    p[0] = (uae_u8)(v >> 24);
    p[1] = (uae_u8)(v >> 16);
    p[2] = (uae_u8)(v >> 8);
    p[3] = (uae_u8)v;
}

static ALWAYS_INLINE void do_put_mem_word(uae_u16 *a, uae_u32 v) {
    uae_u8 *p = (uae_u8 *)a;
    p[0] = (uae_u8)(v >> 8);
    p[1] = (uae_u8)v;
}

// Put 8-bit value to memory
#define do_put_mem_byte(a, v) (*(uae_u8 *)(a) = (v))

/*
 * Memory bank access function call macros
 */
#define call_mem_get_func(func, addr) ((*func)(addr))
#define call_mem_put_func(func, addr, v) ((*func)(addr, v))

/*
 * CPU emulation size (0 = normal)
 */
#define CPU_EMU_SIZE 0
#undef NO_INLINE_MEMORY_ACCESS

/*
 * Enum declaration macros
 */
#define ENUMDECL typedef enum
#define ENUMNAME(name) name

/*
 * Logging function
 */
#define write_log Serial.printf

/*
 * Register parameter hints (not used on ESP32)
 */
#define REGPARAM
#define REGPARAM2

/*
 * Unused parameter macro
 */
#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

/*
 * Branch prediction hints
 * Note: ESP32 may already define these, so only define if not present
 */
#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/*
 * Spinlock implementation (single-threaded, no-op)
 * Note: ESP32 already defines spinlock_t, so we use our own type
 */
typedef volatile int b2_spinlock_t;
#define spinlock_t b2_spinlock_t
#define SPIN_LOCK_UNLOCKED 0

static inline void spin_lock(b2_spinlock_t *lock) {
    UNUSED(lock);
}

static inline void spin_unlock(b2_spinlock_t *lock) {
    UNUSED(lock);
}

static inline int spin_trylock(b2_spinlock_t *lock) {
    UNUSED(lock);
    return 1;
}

/*
 * Mutex implementation using FreeRTOS semaphores for thread safety
 */
struct B2_mutex {
    SemaphoreHandle_t sem;
};

/*
 * Timing functions (implemented in timer_esp32.cpp)
 */
extern uint64 GetTicks_usec(void);
extern void Delay_usec(uint64 usec);

/*
 * Disable features not needed on ESP32
 */
#undef ENABLE_MON
#undef USE_JIT
#undef ENABLE_GTK
#undef ENABLE_XF86_DGA
#undef USE_SDL
#undef USE_SDL_VIDEO
#undef USE_SDL_AUDIO

/*
 * FPU configuration
 */
#define FPU_IEEE 1
#define FPU_X86 0
#define FPU_UAE 0

/*
 * Assembly symbol naming (not used)
 */
#define ASM_SYM(a)

/*
 * POSIX-like file I/O macros
 */
#ifndef O_RDONLY
#define O_RDONLY 0
#endif
#ifndef O_RDWR
#define O_RDWR 2
#endif

/*
 * Debug configuration
 */
#ifndef DEBUG
#define DEBUG 0
#endif

/*
 * PSRAM allocation helper - use Arduino's ps_malloc
 */
#define psram_malloc(size) ps_malloc(size)
#define psram_calloc(n, size) ps_calloc(n, size)

#endif /* SYSDEPS_H */
