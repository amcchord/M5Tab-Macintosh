/*
 * sysdeps.h - Host system definitions for CPU table generation tools
 * 
 * This is a simplified sysdeps.h for compiling build68k and gencpu on macOS
 */

#ifndef SYSDEPS_H
#define SYSDEPS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

/* Basic data types */
typedef uint8_t uint8;
typedef int8_t int8;
typedef uint16_t uint16;
typedef int16_t int16;
typedef uint32_t uint32;
typedef int32_t int32;
typedef uint64_t uint64;
typedef int64_t int64;
typedef uintptr_t uintptr;

/* UAE CPU data types */
typedef int8_t uae_s8;
typedef uint8_t uae_u8;
typedef int16_t uae_s16;
typedef uint16_t uae_u16;
typedef int32_t uae_s32;
typedef uint32_t uae_u32;
typedef int64_t uae_s64;
typedef uint64_t uae_u64;
typedef uint32_t uaecptr;

/* Memory pointer type */
#define memptr uint32

/* Endianness - macOS is little-endian on x86/ARM */
#undef WORDS_BIGENDIAN

/* Data sizes */
#define SIZEOF_SHORT 2
#define SIZEOF_INT 4
#define SIZEOF_LONG 8  /* 64-bit on macOS */
#define SIZEOF_LONG_LONG 8
#define SIZEOF_VOID_P 8  /* 64-bit on macOS */

/* 64-bit value macros */
#define VAL64(a) (a ## LL)
#define UVAL64(a) (a ## ULL)

/* Inline hints */
#define __inline__ inline

/* Logging function (no-op for build tools) */
#define write_log(...)

/* Register parameter hints */
#define REGPARAM
#define REGPARAM2

/* Unused parameter */
#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

/* Branch prediction hints */
#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/* FPU configuration - not needed for table generation */
#define FPU_IEEE 1
#define FPU_X86 0
#define FPU_UAE 0

/* CPU configuration */
#define EMULATED_68K 1
#define CPU_EMU_SIZE 0  /* 0 = all instructions, higher values exclude some */

/* Enum declaration macros - required by readcpu.h */
#define ENUMDECL typedef enum
#define ENUMNAME(name) name

#endif /* SYSDEPS_H */
