/*
 * UAE - The Un*x Amiga Emulator
 *
 * memory management
 *
 * Copyright 1995 Bernd Schmidt
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef UAE_MEMORY_H
#define UAE_MEMORY_H

#if !DIRECT_ADDRESSING && !REAL_ADDRESSING

/* Enabling this adds one additional native memory reference per 68k memory
 * access, but saves one shift (on the x86). Enabling this is probably
 * better for the cache. My favourite benchmark (PP2) doesn't show a
 * difference, so I leave this enabled. */

#if 1 || defined SAVE_MEMORY
#define SAVE_MEMORY_BANKS
#endif

typedef uae_u32 (REGPARAM2 *mem_get_func)(uaecptr) REGPARAM;
typedef void (REGPARAM2 *mem_put_func)(uaecptr, uae_u32) REGPARAM;
typedef uae_u8 *(REGPARAM2 *xlate_func)(uaecptr) REGPARAM;

#undef DIRECT_MEMFUNCS_SUCCESSFUL

#ifndef CAN_MAP_MEMORY
#undef USE_COMPILER
#endif

#if defined(USE_COMPILER) && !defined(USE_MAPPED_MEMORY)
#define USE_MAPPED_MEMORY
#endif

typedef struct {
    /* These ones should be self-explanatory... */
    mem_get_func lget, wget, bget;
    mem_put_func lput, wput, bput;
    /* Use xlateaddr to translate an Amiga address to a uae_u8 * that can
     * be used to address memory without calling the wget/wput functions.
     * This doesn't work for all memory banks, so this function may call
     * abort(). */
    xlate_func xlateaddr;
} addrbank;

extern uae_u8 filesysory[65536];

extern addrbank ram_bank;	// Mac RAM
extern addrbank rom_bank;	// Mac ROM
extern addrbank frame_bank;	// Frame buffer

/* Default memory access functions */

extern uae_u8 *REGPARAM2 default_xlate(uaecptr addr) REGPARAM;

#define bankindex(addr) (((uaecptr)(addr)) >> 16)

#ifdef SAVE_MEMORY_BANKS
// Note: mem_banks is dynamically allocated in PSRAM on ESP32
extern addrbank **mem_banks;
#define get_mem_bank(addr) (*mem_banks[bankindex(addr)])
#define put_mem_bank(addr, b) (mem_banks[bankindex(addr)] = (b))
#else
extern addrbank mem_banks[65536];
#define get_mem_bank(addr) (mem_banks[bankindex(addr)])
#define put_mem_bank(addr, b) (mem_banks[bankindex(addr)] = *(b))
#endif

extern void memory_init(void);
extern void map_banks(addrbank *bank, int first, int count);

#ifndef NO_INLINE_MEMORY_ACCESS

/*
 * FAST-PATH MEMORY ACCESS OPTIMIZATION
 * 
 * Most memory accesses in the emulator are to RAM (code/data) or ROM.
 * By adding inline checks for these common cases, we can bypass the
 * expensive memory bank lookup (pointer indirection + function call)
 * for the majority of accesses.
 * 
 * Performance impact: Significant (2-3x for memory-intensive code)
 * 
 * Memory layout:
 * - RAM: 0x00000000 to RAMSize (typically 8MB)
 * - ROM: ROMBaseMac to ROMBaseMac + ROMSize (varies by ROM type)
 * - Frame buffer: MacFrameBaseMac (0xa0000000)
 */

// Branch prediction hints (may already be defined in sysdeps.h)
#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

// External declarations for fast-path checks
extern uint32 RAMBaseMac;
extern uint8 *RAMBaseHost;
extern uint32 RAMSize;
extern uint32 ROMBaseMac;
extern uint8 *ROMBaseHost;
extern uint32 ROMSize;
extern int MacFrameLayout;
#if !REAL_ADDRESSING && !DIRECT_ADDRESSING
extern uint8 *MacFrameBaseHost;
extern uint32 MacFrameSize;
extern void VideoMarkDirtyOffset(uint32 offset);
extern void VideoMarkDirtyRange(uint32 offset, uint32 size);

#ifndef FLAYOUT_DIRECT
#define FLAYOUT_DIRECT 1
#endif

// Frame buffer base used by banked memory layout in Basilisk.
#ifndef BASILISK_FRAME_BASE_MAC
#define BASILISK_FRAME_BASE_MAC 0xA0000000u
#endif
#endif

// Only guest RAM is expanded inline into the opcode handlers. ROM data,
// frame-buffer writes and bank dispatch are rarer and live in compact
// out-of-line helpers (memory.cpp), which keeps each handler small enough
// for the instruction cache.
extern "C" {
uae_u32 REGPARAM2 mem_slow_lget(uaecptr addr) REGPARAM;
uae_u32 REGPARAM2 mem_slow_wget(uaecptr addr) REGPARAM;
uae_u32 REGPARAM2 mem_slow_bget(uaecptr addr) REGPARAM;
void REGPARAM2 mem_slow_lput(uaecptr addr, uae_u32 l) REGPARAM;
void REGPARAM2 mem_slow_wput(uaecptr addr, uae_u32 w) REGPARAM;
void REGPARAM2 mem_slow_bput(uaecptr addr, uae_u32 b) REGPARAM;
}

#if defined(__riscv) && !defined(MEM_SLOW_DIRECT_CALLS)
// RAMBaseHost and RAMSize are fixed before the first guest instruction. An
// input-free, non-volatile asm is a constant to GCC, so a handler loads each
// once even though its guest stores could otherwise alias them and force
// reloads. Both live in .sdata, so the linker relaxes the lui/lw pair into a
// single gp-relative load.
static ALWAYS_INLINE uint32 mem_ram_size(void) {
    uint32 v;
    __asm__("lui %0, %%hi(RAMSize)\n\tlw %0, %%lo(RAMSize)(%0)" : "=r"(v));
    return v;
}
static ALWAYS_INLINE uint8 *mem_ram_base(void) {
    uint8 *v;
    __asm__("lui %0, %%hi(RAMBaseHost)\n\tlw %0, %%lo(RAMBaseHost)(%0)" : "=r"(v));
    return v;
}

// The slow paths are reached through IRAM thunks (memory.cpp) that preserve
// every integer register except the result and use t0 as the link register.
// The inline fast path therefore contains no real call, so handlers need no
// callee-saved spills. Floating-point temporaries are declared clobbered
// because bank handlers are ordinary C code.
#define MEM_SLOW_FP_CLOBBERS \
    "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7", "ft8", "ft9", \
    "ft10", "ft11", "fa0", "fa1", "fa2", "fa3", "fa4", "fa5", "fa6", "fa7"
#define MEM_SLOW_GET(thunk, addr) ({ \
    register uae_u32 mem_a0_ __asm__("a0") = (addr); \
    __asm__ volatile("1: auipc t0, %%pcrel_hi(" #thunk ")\n\tjalr t0, %%pcrel_lo(1b)(t0)" \
                     : "+r"(mem_a0_) : : "t0", "memory", MEM_SLOW_FP_CLOBBERS); \
    mem_a0_; })
#define MEM_SLOW_PUT(thunk, addr, value) do { \
    register uae_u32 mem_a0_ __asm__("a0") = (addr); \
    register uae_u32 mem_a1_ __asm__("a1") = (value); \
    __asm__ volatile("1: auipc t0, %%pcrel_hi(" #thunk ")\n\tjalr t0, %%pcrel_lo(1b)(t0)" \
                     : : "r"(mem_a0_), "r"(mem_a1_) : "t0", "memory", MEM_SLOW_FP_CLOBBERS); \
} while (0)
#define mem_slow_lget_fast(addr) MEM_SLOW_GET(mem_slow_lget_thunk, addr)
#define mem_slow_wget_fast(addr) MEM_SLOW_GET(mem_slow_wget_thunk, addr)
#define mem_slow_bget_fast(addr) MEM_SLOW_GET(mem_slow_bget_thunk, addr)
#define mem_slow_lput_fast(addr, v) MEM_SLOW_PUT(mem_slow_lput_thunk, addr, v)
#define mem_slow_wput_fast(addr, v) MEM_SLOW_PUT(mem_slow_wput_thunk, addr, v)
#define mem_slow_bput_fast(addr, v) MEM_SLOW_PUT(mem_slow_bput_thunk, addr, v)
#else
static ALWAYS_INLINE uint32 mem_ram_size(void) { return RAMSize; }
static ALWAYS_INLINE uint8 *mem_ram_base(void) { return RAMBaseHost; }
#define mem_slow_lget_fast(addr) mem_slow_lget(addr)
#define mem_slow_wget_fast(addr) mem_slow_wget(addr)
#define mem_slow_bget_fast(addr) mem_slow_bget(addr)
#define mem_slow_lput_fast(addr, v) mem_slow_lput(addr, v)
#define mem_slow_wput_fast(addr, v) mem_slow_wput(addr, v)
#define mem_slow_bput_fast(addr, v) mem_slow_bput(addr, v)
#endif

static ALWAYS_INLINE uae_u32 longget_fastpath(uaecptr addr) {
    if (likely(addr < mem_ram_size())) {
        return do_get_mem_long((uae_u32 *)(mem_ram_base() + addr));
    }
    return mem_slow_lget_fast(addr);
}

static ALWAYS_INLINE uae_u32 wordget_fastpath(uaecptr addr) {
    if (likely(addr < mem_ram_size())) {
        return do_get_mem_word((uae_u16 *)(mem_ram_base() + addr));
    }
    return mem_slow_wget_fast(addr);
}

static ALWAYS_INLINE uae_u32 byteget_fastpath(uaecptr addr) {
    if (likely(addr < mem_ram_size())) {
        return *(uae_u8 *)(mem_ram_base() + addr);
    }
    return mem_slow_bget_fast(addr);
}

static ALWAYS_INLINE void longput_fastpath(uaecptr addr, uae_u32 l) {
    if (likely(addr < mem_ram_size())) {
        do_put_mem_long((uae_u32 *)(mem_ram_base() + addr), l);
        return;
    }
    mem_slow_lput_fast(addr, l);
}

static ALWAYS_INLINE void wordput_fastpath(uaecptr addr, uae_u32 w) {
    if (likely(addr < mem_ram_size())) {
        do_put_mem_word((uae_u16 *)(mem_ram_base() + addr), w);
        return;
    }
    mem_slow_wput_fast(addr, w);
}

static ALWAYS_INLINE void byteput_fastpath(uaecptr addr, uae_u32 b) {
    if (likely(addr < mem_ram_size())) {
        *(uae_u8 *)(mem_ram_base() + addr) = b;
        return;
    }
    mem_slow_bput_fast(addr, b);
}

// Use fast-path functions for all memory access
#define longget(addr) longget_fastpath(addr)
#define wordget(addr) wordget_fastpath(addr)
#define byteget(addr) byteget_fastpath(addr)
#define longput(addr,l) longput_fastpath(addr, l)
#define wordput(addr,w) wordput_fastpath(addr, w)
#define byteput(addr,b) byteput_fastpath(addr, b)

#else

extern uae_u32 longget(uaecptr addr);
extern uae_u32 wordget(uaecptr addr);
extern uae_u32 byteget(uaecptr addr);
extern void longput(uaecptr addr, uae_u32 l);
extern void wordput(uaecptr addr, uae_u32 w);
extern void byteput(uaecptr addr, uae_u32 b);

#endif

#ifndef MD_HAVE_MEM_1_FUNCS

#define longget_1 longget
#define wordget_1 wordget
#define byteget_1 byteget
#define longput_1 longput
#define wordput_1 wordput
#define byteput_1 byteput

#endif

#endif /* !DIRECT_ADDRESSING && !REAL_ADDRESSING */

#if REAL_ADDRESSING
const uintptr MEMBaseDiff = 0;
#elif DIRECT_ADDRESSING
extern uintptr MEMBaseDiff;
#endif

#if REAL_ADDRESSING || DIRECT_ADDRESSING
static __inline__ uae_u8 *do_get_real_address(uaecptr addr)
{
	return (uae_u8 *)MEMBaseDiff + addr;
}
static __inline__ uae_u32 do_get_virtual_address(uae_u8 *addr)
{
	return (uintptr)addr - MEMBaseDiff;
}
static ALWAYS_INLINE uae_u32 get_long(uaecptr addr)
{
    uae_u32 * const m = (uae_u32 *)do_get_real_address(addr);
    return do_get_mem_long(m);
}
static ALWAYS_INLINE uae_u32 get_word(uaecptr addr)
{
    uae_u16 * const m = (uae_u16 *)do_get_real_address(addr);
    return do_get_mem_word(m);
}
static ALWAYS_INLINE uae_u32 get_byte(uaecptr addr)
{
    uae_u8 * const m = (uae_u8 *)do_get_real_address(addr);
    return do_get_mem_byte(m);
}
static ALWAYS_INLINE void put_long(uaecptr addr, uae_u32 l)
{
    uae_u32 * const m = (uae_u32 *)do_get_real_address(addr);
    do_put_mem_long(m, l);
}
static ALWAYS_INLINE void put_word(uaecptr addr, uae_u32 w)
{
    uae_u16 * const m = (uae_u16 *)do_get_real_address(addr);
    do_put_mem_word(m, w);
}
static ALWAYS_INLINE void put_byte(uaecptr addr, uae_u32 b)
{
    uae_u8 * const m = (uae_u8 *)do_get_real_address(addr);
    do_put_mem_byte(m, b);
}
static ALWAYS_INLINE uae_u8 *get_real_address(uaecptr addr)
{
	return do_get_real_address(addr);
}
static __inline__ uae_u32 get_virtual_address(uae_u8 *addr)
{
	return do_get_virtual_address(addr);
}
#else
static ALWAYS_INLINE uae_u32 get_long(uaecptr addr)
{
    return longget_1(addr);
}
static ALWAYS_INLINE uae_u32 get_word(uaecptr addr)
{
    return wordget_1(addr);
}
static ALWAYS_INLINE uae_u32 get_byte(uaecptr addr)
{
    return byteget_1(addr);
}
static ALWAYS_INLINE void put_long(uaecptr addr, uae_u32 l)
{
    longput_1(addr, l);
}
static ALWAYS_INLINE void put_word(uaecptr addr, uae_u32 w)
{
    wordput_1(addr, w);
}
static ALWAYS_INLINE void put_byte(uaecptr addr, uae_u32 b)
{
    byteput_1(addr, b);
}
static ALWAYS_INLINE uae_u8 *get_real_address(uaecptr addr)
{
    return get_mem_bank(addr).xlateaddr(addr);
}
/* gb-- deliberately not implemented since it shall not be used... */
extern uae_u32 get_virtual_address(uae_u8 *addr);
#endif /* DIRECT_ADDRESSING || REAL_ADDRESSING */

#endif /* MEMORY_H */
