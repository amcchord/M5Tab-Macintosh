/*
 * UAE - The Un*x Amiga Emulator
 *
 * Memory management
 *
 * (c) 1995 Bernd Schmidt
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

#include <stdio.h>
#include <stdlib.h>

#include "sysdeps.h"

#ifdef ARDUINO
#include <esp_heap_caps.h>
#include <esp_attr.h>
#endif

#include "cpu_emulation.h"
#include "main.h"
#include "video.h"

#include "m68k.h"
#include "memory.h"
#include "readcpu.h"
#include "newcpu.h"

#if !REAL_ADDRESSING && !DIRECT_ADDRESSING

static bool illegal_mem = false;

#ifdef SAVE_MEMORY_BANKS
// 256KB pointer array - dynamically allocated in PSRAM on ESP32
addrbank **mem_banks = NULL;
#else
addrbank mem_banks[65536];
#endif

#ifdef WORDS_BIGENDIAN
# define swap_words(X) (X)
#else
# define swap_words(X) (((X) >> 16) | ((X) << 16))
#endif

#ifdef NO_INLINE_MEMORY_ACCESS
uae_u32 longget (uaecptr addr)
{
    return call_mem_get_func (get_mem_bank (addr).lget, addr);
}
uae_u32 wordget (uaecptr addr)
{
    return call_mem_get_func (get_mem_bank (addr).wget, addr);
}
uae_u32 byteget (uaecptr addr)
{
    return call_mem_get_func (get_mem_bank (addr).bget, addr);
}
void longput (uaecptr addr, uae_u32 l)
{
    call_mem_put_func (get_mem_bank (addr).lput, addr, l);
}
void wordput (uaecptr addr, uae_u32 w)
{
    call_mem_put_func (get_mem_bank (addr).wput, addr, w);
}
void byteput (uaecptr addr, uae_u32 b)
{
    call_mem_put_func (get_mem_bank (addr).bput, addr, b);
}
#endif

#ifndef NO_INLINE_MEMORY_ACCESS
#ifdef ARDUINO
#define MEM_SLOW_ATTR IRAM_ATTR
#else
#define MEM_SLOW_ATTR
#endif

// Out-of-line halves of the inline RAM fast paths in memory.h. ROM data is
// read directly; frame-buffer writes in the direct layout publish a dirty
// range after storing, exactly as the frame bank handlers do.
extern "C" {

MEM_SLOW_ATTR uae_u32 REGPARAM2 mem_slow_lget(uaecptr addr)
{
    if (addr - ROMBaseMac < ROMSize) {
        return do_get_mem_long((uae_u32 *)(ROMBaseHost + (addr - ROMBaseMac)));
    }
    return call_mem_get_func(get_mem_bank(addr).lget, addr);
}

MEM_SLOW_ATTR uae_u32 REGPARAM2 mem_slow_wget(uaecptr addr)
{
    if (addr - ROMBaseMac < ROMSize) {
        return do_get_mem_word((uae_u16 *)(ROMBaseHost + (addr - ROMBaseMac)));
    }
    return call_mem_get_func(get_mem_bank(addr).wget, addr);
}

MEM_SLOW_ATTR uae_u32 REGPARAM2 mem_slow_bget(uaecptr addr)
{
    if (addr - ROMBaseMac < ROMSize) {
        return *(uae_u8 *)(ROMBaseHost + (addr - ROMBaseMac));
    }
    return call_mem_get_func(get_mem_bank(addr).bget, addr);
}

MEM_SLOW_ATTR void REGPARAM2 mem_slow_lput(uaecptr addr, uae_u32 l)
{
    if (MacFrameLayout == FLAYOUT_DIRECT && addr - BASILISK_FRAME_BASE_MAC < MacFrameSize) {
        const uint32 offset = addr - BASILISK_FRAME_BASE_MAC;
        do_put_mem_long((uae_u32 *)(MacFrameBaseHost + offset), l);
        VideoMarkDirtyRange(offset, 4);
        return;
    }
    call_mem_put_func(get_mem_bank(addr).lput, addr, l);
}

MEM_SLOW_ATTR void REGPARAM2 mem_slow_wput(uaecptr addr, uae_u32 w)
{
    if (MacFrameLayout == FLAYOUT_DIRECT && addr - BASILISK_FRAME_BASE_MAC < MacFrameSize) {
        const uint32 offset = addr - BASILISK_FRAME_BASE_MAC;
        do_put_mem_word((uae_u16 *)(MacFrameBaseHost + offset), w);
        VideoMarkDirtyRange(offset, 2);
        return;
    }
    call_mem_put_func(get_mem_bank(addr).wput, addr, w);
}

MEM_SLOW_ATTR void REGPARAM2 mem_slow_bput(uaecptr addr, uae_u32 b)
{
    if (MacFrameLayout == FLAYOUT_DIRECT && addr - BASILISK_FRAME_BASE_MAC < MacFrameSize) {
        const uint32 offset = addr - BASILISK_FRAME_BASE_MAC;
        *(uae_u8 *)(MacFrameBaseHost + offset) = b;
        VideoMarkDirtyOffset(offset);
        return;
    }
    call_mem_put_func(get_mem_bank(addr).bput, addr, b);
}

#if defined(__riscv) && !defined(MEM_SLOW_DIRECT_CALLS)
// Register-preserving entry points for the inline fast paths in memory.h.
// Entered with `jalr t0`: a0 (and a1 for stores) carry arguments, t0 is the
// return address. Everything else a C call may clobber, except a0 for loads
// and the FP temporaries the call sites declare, is preserved.
//
// ROM loads and direct-layout frame-buffer loads/stores are the common slow
// cases (QuickDraw tables and screen drawing), so the thunks finish them with
// four scratch registers before falling back to a full save and C call. A
// frame-buffer store publishes its pixels, fences, then sets the damage flag
// for each span it touched (see video_dirty_chunks).
#define MEM_THUNK_FULL_SAVE \
    "addi sp, sp, -64\n" \
    "sw ra, 0(sp)\n  sw t0, 4(sp)\n  sw t1, 8(sp)\n  sw t2, 12(sp)\n" \
    "sw a0, 16(sp)\n sw a1, 20(sp)\n sw a2, 24(sp)\n sw a3, 28(sp)\n" \
    "sw a4, 32(sp)\n sw a5, 36(sp)\n sw a6, 40(sp)\n sw a7, 44(sp)\n" \
    "sw t3, 48(sp)\n sw t4, 52(sp)\n sw t5, 56(sp)\n sw t6, 60(sp)\n"
#define MEM_THUNK_FULL_RESTORE \
    "lw ra, 0(sp)\n  lw t0, 4(sp)\n  lw t1, 8(sp)\n  lw t2, 12(sp)\n" \
    "lw a1, 20(sp)\n lw a2, 24(sp)\n lw a3, 28(sp)\n" \
    "lw a4, 32(sp)\n lw a5, 36(sp)\n lw a6, 40(sp)\n lw a7, 44(sp)\n" \
    "lw t3, 48(sp)\n lw t4, 52(sp)\n lw t5, 56(sp)\n lw t6, 60(sp)\n" \
    "addi sp, sp, 64\n"
#define MEM_THUNK_SCRATCH_SAVE \
    "addi sp, sp, -16\n sw t1, 0(sp)\n sw t2, 4(sp)\n sw t3, 8(sp)\n sw t4, 12(sp)\n"
#define MEM_THUNK_SCRATCH_RESTORE \
    "lw t1, 0(sp)\n lw t2, 4(sp)\n lw t3, 8(sp)\n lw t4, 12(sp)\n addi sp, sp, 16\n"

// t1 = a0 - base; branch to `miss` unless [t1, t1 + extra] lies inside size.
#define MEM_THUNK_RANGE(base_sym, size_sym, extra, miss) \
    "lui t1, %hi(" base_sym ")\n lw t1, %lo(" base_sym ")(t1)\n sub t1, a0, t1\n" \
    "lui t2, %hi(" size_sym ")\n lw t2, %lo(" size_sym ")(t2)\n" \
    "bgeu t1, t2, " miss "\n addi t3, t1, " #extra "\n bgeu t3, t2, " miss "\n"
// Same for the direct-layout frame buffer at 0xa0000000.
#define MEM_THUNK_FRAME_RANGE(extra, miss) \
    "lui t1, %hi(MacFrameLayout)\n lw t1, %lo(MacFrameLayout)(t1)\n" \
    "addi t1, t1, -1\n bnez t1, " miss "\n" /* FLAYOUT_DIRECT == 1 */ \
    "lui t1, 0xa0000\n sub t1, a0, t1\n" \
    "lui t2, %hi(MacFrameSize)\n lw t2, %lo(MacFrameSize)(t2)\n" \
    "bgeu t1, t2, " miss "\n addi t3, t1, " #extra "\n bgeu t3, t2, " miss "\n"

// Loads: t2 = host base, t1 = offset. ROM first, then the frame buffer.
#define MEM_GET_THUNK(name, target, extra, load_be) \
MEM_SLOW_ATTR __attribute__((naked, used)) void name(void) \
{ \
    __asm__ volatile( \
        MEM_THUNK_SCRATCH_SAVE \
        MEM_THUNK_RANGE("ROMBaseMac", "ROMSize", extra, "2f") \
        "lui t2, %hi(ROMBaseHost)\n lw t2, %lo(ROMBaseHost)(t2)\n j 5f\n" \
        "2:\n" \
        MEM_THUNK_FRAME_RANGE(extra, "9f") \
        "lui t2, %hi(MacFrameBaseHost)\n lw t2, %lo(MacFrameBaseHost)(t2)\n" \
        "5:\n add t2, t2, t1\n" \
        load_be \
        MEM_THUNK_SCRATCH_RESTORE \
        "jr t0\n" \
        "9:\n" \
        MEM_THUNK_SCRATCH_RESTORE \
        MEM_THUNK_FULL_SAVE \
        "call " #target "\n" \
        MEM_THUNK_FULL_RESTORE \
        "jr t0\n"); \
}

// Stores: frame buffer only (ROM writes go to the bank handler).
#define MEM_PUT_THUNK(name, target, extra, store_be) \
MEM_SLOW_ATTR __attribute__((naked, used)) void name(void) \
{ \
    __asm__ volatile( \
        MEM_THUNK_SCRATCH_SAVE \
        MEM_THUNK_FRAME_RANGE(extra, "9f") \
        "lui t2, %hi(MacFrameBaseHost)\n lw t2, %lo(MacFrameBaseHost)(t2)\n" \
        "add t2, t2, t1\n" \
        store_be \
        "fence w, w\n" \
        "lui t4, %hi(video_dirty_limit)\n lw t4, %lo(video_dirty_limit)(t4)\n" \
        "addi t3, t1, " #extra "\n bgeu t3, t4, 8f\n" /* below the displayed rows */ \
        "lui t4, %hi(video_dirty_shift)\n lw t4, %lo(video_dirty_shift)(t4)\n" \
        "lui t2, %hi(video_dirty_chunks)\n addi t2, t2, %lo(video_dirty_chunks)\n" \
        "srl t3, t3, t4\n add t3, t2, t3\n" \
        "srl t1, t1, t4\n add t1, t2, t1\n" \
        "li t4, 1\n sb t4, 0(t1)\n sb t4, 0(t3)\n" \
        "8:\n" \
        MEM_THUNK_SCRATCH_RESTORE \
        "jr t0\n" \
        "9:\n" \
        MEM_THUNK_SCRATCH_RESTORE \
        MEM_THUNK_FULL_SAVE \
        "call " #target "\n" \
        "lw a0, 16(sp)\n" \
        MEM_THUNK_FULL_RESTORE \
        "jr t0\n"); \
}

MEM_GET_THUNK(mem_slow_lget_thunk, mem_slow_lget, 3,
    "lbu a0, 0(t2)\n slli a0, a0, 24\n"
    "lbu t3, 1(t2)\n slli t3, t3, 16\n or a0, a0, t3\n"
    "lbu t3, 2(t2)\n slli t3, t3, 8\n or a0, a0, t3\n"
    "lbu t3, 3(t2)\n or a0, a0, t3\n")
MEM_GET_THUNK(mem_slow_wget_thunk, mem_slow_wget, 1,
    "lbu a0, 0(t2)\n slli a0, a0, 8\n lbu t3, 1(t2)\n or a0, a0, t3\n")
MEM_GET_THUNK(mem_slow_bget_thunk, mem_slow_bget, 0,
    "lbu a0, 0(t2)\n")
MEM_PUT_THUNK(mem_slow_lput_thunk, mem_slow_lput, 3,
    "srli t3, a1, 24\n sb t3, 0(t2)\n srli t3, a1, 16\n sb t3, 1(t2)\n"
    "srli t3, a1, 8\n sb t3, 2(t2)\n sb a1, 3(t2)\n")
MEM_PUT_THUNK(mem_slow_wput_thunk, mem_slow_wput, 1,
    "srli t3, a1, 8\n sb t3, 0(t2)\n sb a1, 1(t2)\n")
MEM_PUT_THUNK(mem_slow_bput_thunk, mem_slow_bput, 0,
    "sb a1, 0(t2)\n")
#undef MEM_GET_THUNK
#undef MEM_PUT_THUNK
#endif

}  // extern "C"
#endif

/* A dummy bank that only contains zeros */

static uae_u32 REGPARAM2 dummy_lget (uaecptr) REGPARAM;
static uae_u32 REGPARAM2 dummy_wget (uaecptr) REGPARAM;
static uae_u32 REGPARAM2 dummy_bget (uaecptr) REGPARAM;
static void REGPARAM2 dummy_lput (uaecptr, uae_u32) REGPARAM;
static void REGPARAM2 dummy_wput (uaecptr, uae_u32) REGPARAM;
static void REGPARAM2 dummy_bput (uaecptr, uae_u32) REGPARAM;

uae_u32 REGPARAM2 dummy_lget (uaecptr addr)
{
    if (illegal_mem)
	write_log ("Illegal lget at %08x\n", addr);

    return 0;
}

uae_u32 REGPARAM2 dummy_wget (uaecptr addr)
{
    if (illegal_mem)
	write_log ("Illegal wget at %08x\n", addr);

    return 0;
}

uae_u32 REGPARAM2 dummy_bget (uaecptr addr)
{
    if (illegal_mem)
	write_log ("Illegal bget at %08x\n", addr);

    return 0;
}

void REGPARAM2 dummy_lput (uaecptr addr, uae_u32 l)
{
    if (illegal_mem)
	write_log ("Illegal lput at %08x\n", addr);
}
void REGPARAM2 dummy_wput (uaecptr addr, uae_u32 w)
{
    if (illegal_mem)
	write_log ("Illegal wput at %08x\n", addr);
}
void REGPARAM2 dummy_bput (uaecptr addr, uae_u32 b)
{
    if (illegal_mem)
	write_log ("Illegal bput at %08x\n", addr);
}

/* Mac RAM (32 bit addressing) */

static uae_u32 REGPARAM2 ram_lget(uaecptr) REGPARAM;
static uae_u32 REGPARAM2 ram_wget(uaecptr) REGPARAM;
static uae_u32 REGPARAM2 ram_bget(uaecptr) REGPARAM;
static void REGPARAM2 ram_lput(uaecptr, uae_u32) REGPARAM;
static void REGPARAM2 ram_wput(uaecptr, uae_u32) REGPARAM;
static void REGPARAM2 ram_bput(uaecptr, uae_u32) REGPARAM;
static uae_u8 *REGPARAM2 ram_xlate(uaecptr addr) REGPARAM;

static uintptr RAMBaseDiff;	// RAMBaseHost - RAMBaseMac

uae_u32 REGPARAM2 ram_lget(uaecptr addr)
{
    uae_u32 *m;
    m = (uae_u32 *)(RAMBaseDiff + addr);
    return do_get_mem_long(m);
}

uae_u32 REGPARAM2 ram_wget(uaecptr addr)
{
    uae_u16 *m;
    m = (uae_u16 *)(RAMBaseDiff + addr);
    return do_get_mem_word(m);
}

uae_u32 REGPARAM2 ram_bget(uaecptr addr)
{
    return (uae_u32)*(uae_u8 *)(RAMBaseDiff + addr);
}

void REGPARAM2 ram_lput(uaecptr addr, uae_u32 l)
{
    uae_u32 *m;
    m = (uae_u32 *)(RAMBaseDiff + addr);
    do_put_mem_long(m, l);
}

void REGPARAM2 ram_wput(uaecptr addr, uae_u32 w)
{
    uae_u16 *m;
    m = (uae_u16 *)(RAMBaseDiff + addr);
    do_put_mem_word(m, w);
}

void REGPARAM2 ram_bput(uaecptr addr, uae_u32 b)
{
	*(uae_u8 *)(RAMBaseDiff + addr) = b;
}

uae_u8 *REGPARAM2 ram_xlate(uaecptr addr)
{
    return (uae_u8 *)(RAMBaseDiff + addr);
}

/* Mac RAM (24 bit addressing) */

static uae_u32 REGPARAM2 ram24_lget(uaecptr) REGPARAM;
static uae_u32 REGPARAM2 ram24_wget(uaecptr) REGPARAM;
static uae_u32 REGPARAM2 ram24_bget(uaecptr) REGPARAM;
static void REGPARAM2 ram24_lput(uaecptr, uae_u32) REGPARAM;
static void REGPARAM2 ram24_wput(uaecptr, uae_u32) REGPARAM;
static void REGPARAM2 ram24_bput(uaecptr, uae_u32) REGPARAM;
static uae_u8 *REGPARAM2 ram24_xlate(uaecptr addr) REGPARAM;

uae_u32 REGPARAM2 ram24_lget(uaecptr addr)
{
    uae_u32 *m;
    m = (uae_u32 *)(RAMBaseDiff + (addr & 0xffffff));
    return do_get_mem_long(m);
}

uae_u32 REGPARAM2 ram24_wget(uaecptr addr)
{
    uae_u16 *m;
    m = (uae_u16 *)(RAMBaseDiff + (addr & 0xffffff));
    return do_get_mem_word(m);
}

uae_u32 REGPARAM2 ram24_bget(uaecptr addr)
{
    return (uae_u32)*(uae_u8 *)(RAMBaseDiff + (addr & 0xffffff));
}

void REGPARAM2 ram24_lput(uaecptr addr, uae_u32 l)
{
    uae_u32 *m;
    m = (uae_u32 *)(RAMBaseDiff + (addr & 0xffffff));
    do_put_mem_long(m, l);
}

void REGPARAM2 ram24_wput(uaecptr addr, uae_u32 w)
{
    uae_u16 *m;
    m = (uae_u16 *)(RAMBaseDiff + (addr & 0xffffff));
    do_put_mem_word(m, w);
}

void REGPARAM2 ram24_bput(uaecptr addr, uae_u32 b)
{
	*(uae_u8 *)(RAMBaseDiff + (addr & 0xffffff)) = b;
}

uae_u8 *REGPARAM2 ram24_xlate(uaecptr addr)
{
    return (uae_u8 *)(RAMBaseDiff + (addr & 0xffffff));
}

/* Mac ROM (32 bit addressing) */

static uae_u32 REGPARAM2 rom_lget(uaecptr) REGPARAM;
static uae_u32 REGPARAM2 rom_wget(uaecptr) REGPARAM;
static uae_u32 REGPARAM2 rom_bget(uaecptr) REGPARAM;
static void REGPARAM2 rom_lput(uaecptr, uae_u32) REGPARAM;
static void REGPARAM2 rom_wput(uaecptr, uae_u32) REGPARAM;
static void REGPARAM2 rom_bput(uaecptr, uae_u32) REGPARAM;
static uae_u8 *REGPARAM2 rom_xlate(uaecptr addr) REGPARAM;

static uintptr ROMBaseDiff;	// ROMBaseHost - ROMBaseMac

uae_u32 REGPARAM2 rom_lget(uaecptr addr)
{
    uae_u32 *m;
    m = (uae_u32 *)(ROMBaseDiff + addr);
    return do_get_mem_long(m);
}

uae_u32 REGPARAM2 rom_wget(uaecptr addr)
{
    uae_u16 *m;
    m = (uae_u16 *)(ROMBaseDiff + addr);
    return do_get_mem_word(m);
}

uae_u32 REGPARAM2 rom_bget(uaecptr addr)
{
    return (uae_u32)*(uae_u8 *)(ROMBaseDiff + addr);
}

void REGPARAM2 rom_lput(uaecptr addr, uae_u32 b)
{
    if (illegal_mem)
	write_log ("Illegal ROM lput at %08x\n", addr);
}

void REGPARAM2 rom_wput(uaecptr addr, uae_u32 b)
{
    if (illegal_mem)
	write_log ("Illegal ROM wput at %08x\n", addr);
}

void REGPARAM2 rom_bput(uaecptr addr, uae_u32 b)
{
    if (illegal_mem)
	write_log ("Illegal ROM bput at %08x\n", addr);
}

uae_u8 *REGPARAM2 rom_xlate(uaecptr addr)
{
    return (uae_u8 *)(ROMBaseDiff + addr);
}

/* Mac ROM (24 bit addressing) */

static uae_u32 REGPARAM2 rom24_lget(uaecptr) REGPARAM;
static uae_u32 REGPARAM2 rom24_wget(uaecptr) REGPARAM;
static uae_u32 REGPARAM2 rom24_bget(uaecptr) REGPARAM;
static uae_u8 *REGPARAM2 rom24_xlate(uaecptr addr) REGPARAM;

uae_u32 REGPARAM2 rom24_lget(uaecptr addr)
{
    uae_u32 *m;
    m = (uae_u32 *)(ROMBaseDiff + (addr & 0xffffff));
    return do_get_mem_long(m);
}

uae_u32 REGPARAM2 rom24_wget(uaecptr addr)
{
    uae_u16 *m;
    m = (uae_u16 *)(ROMBaseDiff + (addr & 0xffffff));
    return do_get_mem_word(m);
}

uae_u32 REGPARAM2 rom24_bget(uaecptr addr)
{
    return (uae_u32)*(uae_u8 *)(ROMBaseDiff + (addr & 0xffffff));
}

uae_u8 *REGPARAM2 rom24_xlate(uaecptr addr)
{
    return (uae_u8 *)(ROMBaseDiff + (addr & 0xffffff));
}

/* Frame buffer */

static uae_u32 REGPARAM2 frame_direct_lget(uaecptr) REGPARAM;
static uae_u32 REGPARAM2 frame_direct_wget(uaecptr) REGPARAM;
static uae_u32 REGPARAM2 frame_direct_bget(uaecptr) REGPARAM;
static void REGPARAM2 frame_direct_lput(uaecptr, uae_u32) REGPARAM;
static void REGPARAM2 frame_direct_wput(uaecptr, uae_u32) REGPARAM;
static void REGPARAM2 frame_direct_bput(uaecptr, uae_u32) REGPARAM;

static uae_u32 REGPARAM2 frame_host_555_lget(uaecptr) REGPARAM;
static uae_u32 REGPARAM2 frame_host_555_wget(uaecptr) REGPARAM;
static void REGPARAM2 frame_host_555_lput(uaecptr, uae_u32) REGPARAM;
static void REGPARAM2 frame_host_555_wput(uaecptr, uae_u32) REGPARAM;

static uae_u32 REGPARAM2 frame_host_565_lget(uaecptr) REGPARAM;
static uae_u32 REGPARAM2 frame_host_565_wget(uaecptr) REGPARAM;
static void REGPARAM2 frame_host_565_lput(uaecptr, uae_u32) REGPARAM;
static void REGPARAM2 frame_host_565_wput(uaecptr, uae_u32) REGPARAM;

static uae_u32 REGPARAM2 frame_host_888_lget(uaecptr) REGPARAM;
static void REGPARAM2 frame_host_888_lput(uaecptr, uae_u32) REGPARAM;

static uae_u8 *REGPARAM2 frame_xlate(uaecptr addr) REGPARAM;

static uintptr FrameBaseDiff;	// MacFrameBaseHost - MacFrameBaseMac

#ifdef ARDUINO
IRAM_ATTR
#endif
uae_u32 REGPARAM2 frame_direct_lget(uaecptr addr)
{
    // Track read-back from video RAM for debugging (write-through queue only)
#if VIDEO_USE_WRITE_THROUGH_QUEUE
    VideoTrackReadBack(addr - MacFrameBaseMac, 4);
#endif
    
    uae_u32 *m;
    m = (uae_u32 *)(FrameBaseDiff + addr);
    return do_get_mem_long(m);
}

#ifdef ARDUINO
IRAM_ATTR
#endif
uae_u32 REGPARAM2 frame_direct_wget(uaecptr addr)
{
    // Track read-back from video RAM for debugging (write-through queue only)
#if VIDEO_USE_WRITE_THROUGH_QUEUE
    VideoTrackReadBack(addr - MacFrameBaseMac, 2);
#endif
    
    uae_u16 *m;
    m = (uae_u16 *)(FrameBaseDiff + addr);
    return do_get_mem_word(m);
}

#ifdef ARDUINO
IRAM_ATTR
#endif
uae_u32 REGPARAM2 frame_direct_bget(uaecptr addr)
{
    // Track read-back from video RAM for debugging (write-through queue only)
#if VIDEO_USE_WRITE_THROUGH_QUEUE
    VideoTrackReadBack(addr - MacFrameBaseMac, 1);
#endif
    
    return (uae_u32)*(uae_u8 *)(FrameBaseDiff + addr);
}

#ifdef ARDUINO
IRAM_ATTR
#endif
void REGPARAM2 frame_direct_lput(uaecptr addr, uae_u32 l)
{
    uae_u32 *m;
    m = (uae_u32 *)(FrameBaseDiff + addr);
    do_put_mem_long(m, l);
    
    uint32_t offset = addr - MacFrameBaseMac;
    
#if VIDEO_USE_WRITE_THROUGH_QUEUE
    // Queue the write with pixel data (big-endian byte order as stored in Mac memory)
    uint8_t data[4];
    data[0] = (l >> 24) & 0xFF;
    data[1] = (l >> 16) & 0xFF;
    data[2] = (l >> 8) & 0xFF;
    data[3] = l & 0xFF;
    VideoQueueWrite(offset, data, 4);
#endif
    
    // Mark dirty tiles for write-time tracking
    VideoMarkDirtyRange(offset, 4);
}

#ifdef ARDUINO
IRAM_ATTR
#endif
void REGPARAM2 frame_direct_wput(uaecptr addr, uae_u32 w)
{
    uae_u16 *m;
    m = (uae_u16 *)(FrameBaseDiff + addr);
    do_put_mem_word(m, w);
    
    uint32_t offset = addr - MacFrameBaseMac;
    
#if VIDEO_USE_WRITE_THROUGH_QUEUE
    // Queue the write with pixel data (big-endian byte order)
    uint8_t data[2];
    data[0] = (w >> 8) & 0xFF;
    data[1] = w & 0xFF;
    VideoQueueWrite(offset, data, 2);
#endif
    
    // Mark dirty tiles for write-time tracking
    VideoMarkDirtyRange(offset, 2);
}

#ifdef ARDUINO
IRAM_ATTR
#endif
void REGPARAM2 frame_direct_bput(uaecptr addr, uae_u32 b)
{
    *(uae_u8 *)(FrameBaseDiff + addr) = b;
    
    uint32_t offset = addr - MacFrameBaseMac;
    
#if VIDEO_USE_WRITE_THROUGH_QUEUE
    // Queue the write with pixel data
    uint8_t data[1];
    data[0] = b & 0xFF;
    VideoQueueWrite(offset, data, 1);
#endif
    
    // Mark dirty tile for write-time tracking
    VideoMarkDirtyOffset(offset);
}

uae_u32 REGPARAM2 frame_host_555_lget(uaecptr addr)
{
    uae_u32 *m, l;
    m = (uae_u32 *)(FrameBaseDiff + addr);
    l = *m;
	return swap_words(l);
}

uae_u32 REGPARAM2 frame_host_555_wget(uaecptr addr)
{
    uae_u16 *m;
    m = (uae_u16 *)(FrameBaseDiff + addr);
    return *m;
}

void REGPARAM2 frame_host_555_lput(uaecptr addr, uae_u32 l)
{
    uae_u32 *m;
    m = (uae_u32 *)(FrameBaseDiff + addr);
    *m = swap_words(l);
}

void REGPARAM2 frame_host_555_wput(uaecptr addr, uae_u32 w)
{
    uae_u16 *m;
    m = (uae_u16 *)(FrameBaseDiff + addr);
    *m = w;
}

uae_u32 REGPARAM2 frame_host_565_lget(uaecptr addr)
{
    uae_u32 *m, l;
    m = (uae_u32 *)(FrameBaseDiff + addr);
    l = *m;
    l = (l & 0x001f001f) | ((l >> 1) & 0x7fe07fe0);
    return swap_words(l);
}

uae_u32 REGPARAM2 frame_host_565_wget(uaecptr addr)
{
    uae_u16 *m, w;
    m = (uae_u16 *)(FrameBaseDiff + addr);
    w = *m;
    return (w & 0x1f) | ((w >> 1) & 0x7fe0);
}

void REGPARAM2 frame_host_565_lput(uaecptr addr, uae_u32 l)
{
    uae_u32 *m;
    m = (uae_u32 *)(FrameBaseDiff + addr);
    l = (l & 0x001f001f) | ((l << 1) & 0xffc0ffc0);
    *m = swap_words(l);
}

void REGPARAM2 frame_host_565_wput(uaecptr addr, uae_u32 w)
{
    uae_u16 *m;
    m = (uae_u16 *)(FrameBaseDiff + addr);
    *m = (w & 0x1f) | ((w << 1) & 0xffc0);
}

uae_u32 REGPARAM2 frame_host_888_lget(uaecptr addr)
{
    uae_u32 *m, l;
    m = (uae_u32 *)(FrameBaseDiff + addr);
    return *m;
}

void REGPARAM2 frame_host_888_lput(uaecptr addr, uae_u32 l)
{
    uae_u32 *m;
    m = (uae_u32 *)(MacFrameBaseHost + addr - MacFrameBaseMac);
    *m = l;
}

uae_u8 *REGPARAM2 frame_xlate(uaecptr addr)
{
    return (uae_u8 *)(FrameBaseDiff + addr);
}

/* Mac framebuffer RAM (24 bit addressing)
 *
 * This works by duplicating appropriate writes to the 32-bit
 * address-space framebuffer.
 */

static void REGPARAM2 fram24_lput(uaecptr, uae_u32) REGPARAM;
static void REGPARAM2 fram24_wput(uaecptr, uae_u32) REGPARAM;
static void REGPARAM2 fram24_bput(uaecptr, uae_u32) REGPARAM;

void REGPARAM2 fram24_lput(uaecptr addr, uae_u32 l)
{
    uaecptr page_off = addr & 0xffff;
    if (0xa700 <= page_off && page_off < 0xfc80) {
	uae_u32 *fm;
	fm = (uae_u32 *)(MacFrameBaseHost + page_off - 0xa700);
	do_put_mem_long(fm, l);
	// Mark dirty tiles for write-time tracking (24-bit addressing)
	VideoMarkDirtyRange(page_off - 0xa700, 4);
    }

    uae_u32 *m;
    m = (uae_u32 *)(RAMBaseDiff + (addr & 0xffffff));
    do_put_mem_long(m, l);
}

void REGPARAM2 fram24_wput(uaecptr addr, uae_u32 w)
{
    uaecptr page_off = addr & 0xffff;
    if (0xa700 <= page_off && page_off < 0xfc80) {
	uae_u16 *fm;
	fm = (uae_u16 *)(MacFrameBaseHost + page_off - 0xa700);
	do_put_mem_word(fm, w);
	// Mark dirty tiles for write-time tracking
	VideoMarkDirtyRange(page_off - 0xa700, 2);
    }

    uae_u16 *m;
    m = (uae_u16 *)(RAMBaseDiff + (addr & 0xffffff));
    do_put_mem_word(m, w);
}

void REGPARAM2 fram24_bput(uaecptr addr, uae_u32 b)
{
    uaecptr page_off = addr & 0xffff;
    if (0xa700 <= page_off && page_off < 0xfc80) {
        *(uae_u8 *)(MacFrameBaseHost + page_off - 0xa700) = b;
        // Mark dirty tile for write-time tracking
        VideoMarkDirtyOffset(page_off - 0xa700);
    }

    *(uae_u8 *)(RAMBaseDiff + (addr & 0xffffff)) = b;
}

/* Default memory access functions */

uae_u8 *REGPARAM2 default_xlate (uaecptr a)
{
    write_log("Your Mac program just did something terribly stupid\n");
    return NULL;
}

/* Address banks */

addrbank dummy_bank = {
    dummy_lget, dummy_wget, dummy_bget,
    dummy_lput, dummy_wput, dummy_bput,
    default_xlate
};

addrbank ram_bank = {
    ram_lget, ram_wget, ram_bget,
    ram_lput, ram_wput, ram_bput,
    ram_xlate
};

addrbank ram24_bank = {
    ram24_lget, ram24_wget, ram24_bget,
    ram24_lput, ram24_wput, ram24_bput,
    ram24_xlate
};

addrbank rom_bank = {
    rom_lget, rom_wget, rom_bget,
    rom_lput, rom_wput, rom_bput,
    rom_xlate
};

addrbank rom24_bank = {
    rom24_lget, rom24_wget, rom24_bget,
    rom_lput, rom_wput, rom_bput,
    rom24_xlate
};

addrbank frame_direct_bank = {
    frame_direct_lget, frame_direct_wget, frame_direct_bget,
    frame_direct_lput, frame_direct_wput, frame_direct_bput,
    frame_xlate
};

addrbank frame_host_555_bank = {
    frame_host_555_lget, frame_host_555_wget, frame_direct_bget,
    frame_host_555_lput, frame_host_555_wput, frame_direct_bput,
    frame_xlate
};

addrbank frame_host_565_bank = {
    frame_host_565_lget, frame_host_565_wget, frame_direct_bget,
    frame_host_565_lput, frame_host_565_wput, frame_direct_bput,
    frame_xlate
};

addrbank frame_host_888_bank = {
    frame_host_888_lget, frame_direct_wget, frame_direct_bget,
    frame_host_888_lput, frame_direct_wput, frame_direct_bput,
    frame_xlate
};

addrbank fram24_bank = {
    ram24_lget, ram24_wget, ram24_bget,
    fram24_lput, fram24_wput, fram24_bput,
    ram24_xlate
};

void memory_init(void)
{
#if defined(ARDUINO) && defined(SAVE_MEMORY_BANKS)
	// Allocate 256KB memory bank pointer array
	// This is accessed on EVERY memory operation (multiple times per instruction)
	// Gets PRIORITY for internal SRAM since it's the hottest path
	if (mem_banks == NULL) {
		// Report available internal SRAM before allocation
		size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
		size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
		write_log("mem_banks allocation: need 256KB, internal SRAM has %d bytes free (largest: %d)\n",
		          free_before, largest_block);
		
		// Prefer internal 32-bit memory for pointer-table lookups in the hot path.
		mem_banks = (addrbank **)heap_caps_malloc(
			65536 * sizeof(addrbank *),
			MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT
		);
		if (mem_banks != NULL) {
			write_log("Allocated mem_banks (256KB) in internal 32-bit memory\n");
		} else {
			// Fall back to PSRAM when internal pools are too fragmented.
			mem_banks = (addrbank **)heap_caps_malloc(65536 * sizeof(addrbank *), MALLOC_CAP_SPIRAM);
			if (mem_banks != NULL) {
				write_log("Allocated mem_banks (256KB) in PSRAM (fallback)\n");
			} else {
				// Final fallback: byte-addressable internal SRAM.
				mem_banks = (addrbank **)heap_caps_malloc(
					65536 * sizeof(addrbank *),
					MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
				);
				if (mem_banks == NULL) {
					write_log("ERROR: Failed to allocate mem_banks!\n");
					return;
				}
				write_log("Allocated mem_banks (256KB) in internal SRAM (8-bit fallback)\n");
			}
		}
	}
#endif

	for(long i=0; i<65536; i++)
		put_mem_bank(i<<16, &dummy_bank);

	// Limit RAM size to not overlap ROM
	uint32 ram_size = RAMSize > ROMBaseMac ? ROMBaseMac : RAMSize;

	RAMBaseDiff = (uintptr)RAMBaseHost - (uintptr)RAMBaseMac;
	ROMBaseDiff = (uintptr)ROMBaseHost - (uintptr)ROMBaseMac;
	FrameBaseDiff = (uintptr)MacFrameBaseHost - (uintptr)MacFrameBaseMac;

	// Map RAM, ROM and display
	if (TwentyFourBitAddressing) {
		map_banks(&ram24_bank, RAMBaseMac >> 16, ram_size >> 16);
		map_banks(&rom24_bank, ROMBaseMac >> 16, ROMSize >> 16);

		// Map frame buffer at end of RAM.
		map_banks(&fram24_bank, ((RAMBaseMac + ram_size) >> 16) - 1, 1);
	} else {
		map_banks(&ram_bank, RAMBaseMac >> 16, ram_size >> 16);
		map_banks(&rom_bank, ROMBaseMac >> 16, ROMSize >> 16);

                // Map frame buffer
		switch (MacFrameLayout) {
			case FLAYOUT_DIRECT:
				map_banks(&frame_direct_bank, MacFrameBaseMac >> 16, (MacFrameSize >> 16) + 1);
				break;
			case FLAYOUT_HOST_555:
				map_banks(&frame_host_555_bank, MacFrameBaseMac >> 16, (MacFrameSize >> 16) + 1);
				break;
			case FLAYOUT_HOST_565:
				map_banks(&frame_host_565_bank, MacFrameBaseMac >> 16, (MacFrameSize >> 16) + 1);
				break;
			case FLAYOUT_HOST_888:
				map_banks(&frame_host_888_bank, MacFrameBaseMac >> 16, (MacFrameSize >> 16) + 1);
				break;
		}
	}
}

void map_banks(addrbank *bank, int start, int size)
{
    int bnr;
    unsigned long int hioffs = 0, endhioffs = 0x100;

    if (start >= 0x100) {
	for (bnr = start; bnr < start + size; bnr++)
	    put_mem_bank (bnr << 16, bank);
	return;
    }
    if (TwentyFourBitAddressing) endhioffs = 0x10000;
    for (hioffs = 0; hioffs < endhioffs; hioffs += 0x100)
	for (bnr = start; bnr < start+size; bnr++)
	    put_mem_bank((bnr + hioffs) << 16, bank);
}

/*
 *  get_virtual_address - Convert host address to Mac address
 *  This is only called in virtual addressing mode
 */
uae_u32 get_virtual_address(uae_u8 *addr)
{
    // Check if address is in RAM
    uintptr host_addr = (uintptr)addr;
    uintptr ram_start = (uintptr)RAMBaseHost;
    uintptr ram_end = ram_start + RAMSize;
    
    if (host_addr >= ram_start && host_addr < ram_end) {
        return RAMBaseMac + (host_addr - ram_start);
    }
    
    // Check if address is in ROM
    uintptr rom_start = (uintptr)ROMBaseHost;
    uintptr rom_end = rom_start + ROMSize;
    
    if (host_addr >= rom_start && host_addr < rom_end) {
        return ROMBaseMac + (host_addr - rom_start);
    }
    
    // Check if address is in frame buffer
    uintptr frame_start = (uintptr)MacFrameBaseHost;
    uintptr frame_end = frame_start + MacFrameSize;
    
    if (host_addr >= frame_start && host_addr < frame_end) {
        return MacFrameBaseMac + (host_addr - frame_start);
    }
    
    // Address not found - return 0
    write_log("get_virtual_address: unknown host address %p\n", addr);
    return 0;
}

#endif /* !REAL_ADDRESSING && !DIRECT_ADDRESSING */
