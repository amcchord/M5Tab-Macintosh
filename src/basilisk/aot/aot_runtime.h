/*
 *  aot_runtime.h - Ahead-of-time translated 68k code
 *
 *  tools/aot/aot_translate.py turns hot 68k code whose bytes are known in
 *  advance into ordinary C++ functions ("regions") at build time. No code is
 *  generated at run time. A region is entered through an entry point: the
 *  first opcode of the 68k code is replaced with an unused opcode whose
 *  handler is the translated function.
 *
 *  ROM regions are installed once by AotInstall(), after it has checked that
 *  the ROM holds the bytes each region was translated from.
 *
 *  RAM code is translated as position-independent modules. A module is
 *  installed where AotRamSample() finds its code executing, after checking
 *  every byte of it. Entering an installed module is only a comparison while
 *  the guest has not flushed the instruction cache since the last check
 *  (AotCodeChanged()); after a flush the bytes are checked again. This is
 *  the contract the guest already keeps with a 68040: code that was changed
 *  is not executed until the cache has been flushed.
 *
 *  See EMULATOR_PERFORMANCE.md.
 */

#ifndef AOT_RUNTIME_H
#define AOT_RUNTIME_H

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "m68k.h"
#include "memory.h"
#include "readcpu.h"
#include "newcpu.h"

#ifndef AOT_ROM
#define AOT_ROM 1
#endif
// Translated RAM modules need AOT_ROM; AOT_RAM=0 leaves guest RAM untouched.
#ifndef AOT_RAM
#define AOT_RAM 1
#endif

// Run the translated code from PSRAM instead of flash. Both are reached
// through the same caches, but a miss is filled from PSRAM (16 lines at
// 200 MHz) several times faster than from flash (4 lines at 80 MHz).
// AotInstall() copies the code; tools/aot/check_blob.py checks at build time
// that it can be moved.
#ifndef AOT_PSRAM
#define AOT_PSRAM 1
#endif

#define AOT_NO_BASE 0xffffffffu

// What translated code calls outside itself. The calls are made through
// this table so that the code holds no PC-relative reference to anything
// that does not move with it.
struct AotFar {
	uae_u32 (*ram_entry_slow)(uae_u32 module, uae_u32 offset);
	void (*mull)(uae_u32 opcode, uae_u32 src, uae_u16 extra);
	void (*exception)(int nr, uaecptr oldpc);
	bool (*trap_verify)(void);
};
extern const AotFar aot_far;

// One per entry point of a multi-entry region, in entry-opcode order.
struct AotEntryInfo {
	uae_u32 offset;          // of the entry instruction: in the ROM, or in its module
	uae_u32 original;        // its opcode, in the form handlers are called with
};

struct AotRange {
	uae_u32 offset;          // of translated instruction bytes: in the ROM, or in a module
	uae_u32 length;
};

struct AotRegion {
	uae_u32 first_range;
	uae_u32 range_count;
	uae_u32 crc;             // CRC-32 of the region's ranges, in order
};

struct AotEntry {
	uae_u32 offset;          // of the entry instruction: in the ROM, or in its module
	uae_u16 original;        // its opcode
	uae_u16 owner;           // region (ROM) or module (RAM)
	cpuop_func *handler;
};

struct AotModule {
	uae_u32 first_range;     // ranges leave out the entry opcodes
	uae_u32 range_count;
	uae_u32 checksum;        // aot_sum16 of the ranges, in order
	uae_u32 first_entry;
	uae_u32 entry_count;
	uae_u32 span;            // bytes from the first to the last translated byte
};

struct AotAnchor {
	uae_u32 hash;            // aot_sum16 of the 8 bytes at an instruction of a module
	uae_u32 module;
	uae_u32 offset;
};

struct AotModuleState {
	uae_u32 base;            // guest address the module is installed at, or AOT_NO_BASE
	uae_u32 epoch;           // aot_flush_epoch when its bytes were last checked
};

struct AotImage {
	uae_u32 rom_base;        // guest address the ROM code was translated for
	uae_u32 rom_size;
	const AotRange *ranges;
	const AotRegion *regions;
	const AotEntry *entries;
	const uae_u16 *patch_opcodes;  // one unused opcode per entry, ROM entries first
	uae_u32 range_count;
	uae_u32 region_count;
	uae_u32 entry_count;

	const AotRange *ram_ranges;
	const AotModule *modules;
	const AotEntry *ram_entries;
	const AotAnchor *anchors;      // sorted by hash
	AotModuleState *module_state;
	uae_u32 ram_range_count;
	uae_u32 module_count;
	uae_u32 ram_entry_count;
	uae_u32 anchor_count;

	// Test builds only: the code of each module as it was profiled.
	const uae_u8 *module_code;
	const uae_u32 *module_code_offsets;

	// Combined multi-entry descriptor table (copied to PSRAM at AotInstall).
	const AotEntryInfo *entry_infos;
	uae_u32 entry_info_count;
};

struct AotStats {
	uae_u32 regions_total;
	uae_u32 regions_installed;
	uae_u32 entries_total;
	uae_u32 entries_installed;
	uae_u32 modules_total;
	uae_u32 modules_installed;     // now
	uae_u32 module_installs;       // since start
	uae_u32 module_checks;         // byte comparisons of a module since start
	uae_u32 module_rejects;        // checks that failed
	uae_u32 flushes;               // AotCodeChanged() calls
	uae_u32 cpusha_narrowed;       // _HLock CPUSHA narrowed to handle range
	uae_u32 cpusha_global;         // CPUSHA that fell back to AotCodeChanged()
	uae_u32 range_flushes_skipped; // range flushes with no overlapping AOT page
	uae_u32 range_invalidations;   // selective per-module invalidations
	uae_u32 code_bytes;            // translated code
	uae_u32 code_in_psram;         // 1 when it runs from PSRAM
	uae_u64 calls;                 // region calls since start
	uae_u64 instructions;          // 68k instructions executed inside regions
};

// Defined by the generated aot_rom_generated.cpp; an empty image otherwise.
extern const AotImage aot_image;

// 68k instructions executed inside regions beyond the one the dispatch loop
// counts per handler call. The CPU loop folds this into its instruction
// accounting at the end of each batch.
extern int32 aot_extra_instructions;
extern uae_u32 aot_region_calls;
extern uae_u64 aot_total_instructions;

// Advances whenever guest code may have changed.
extern uae_u32 aot_flush_epoch;
extern AotModuleState aot_module_state[];
extern const AotEntryInfo *aot_entry_infos_runtime;
extern uae_u32 aot_cpusha_narrowed;
extern uae_u32 aot_cpusha_global;
extern uaecptr aot_trap_vec_cached;
extern bool aot_trap_vec_native;

#if defined(__riscv) && !defined(MEM_SLOW_DIRECT_CALLS)
extern "C" void aot_setpc_thunk(void);
#endif

// Fold the per-batch counters into the running totals (CPU loop, batch end).
static inline void AotAccount(int32 extra_instructions)
{
	aot_total_instructions += (uae_u32)extra_instructions;
}

// Install the translated ROM regions that match the ROM in memory. Call
// once, after PatchROM() and Init680x0(), before the first guest instruction.
extern void AotInstall(void);
extern void AotReadStats(AotStats *stats);

// The 16-bit word the ROM held at a guest address before AotInstall()
// replaced entry opcodes; get_word() for any other address. For code that
// compares ROM instructions with a known sequence.
extern uae_u32 AotOriginalWord(uaecptr address);

// The guest flushed its instruction cache, or the emulator wrote guest
// memory that may hold code.
extern void AotCodeChanged(void);
// The same for a known range of host memory or guest RAM: O(1) no-op when
// no installed module page overlaps [addr, addr + size).
extern void AotCodeChangedRange(const uae_u8 *host, uae_u32 size);
extern void AotCodeChangedGuestRange(uaecptr addr, uae_u32 size);
// CPUSHA cache flush handler: narrows Q650 32-bit _HLock flushes to the
// locked handle's block range when verified, falling back to AotCodeChanged().
extern void AotHandleCpusha(void);

// CPU loop, between instructions: if the code at the PC belongs to a
// translated module that is not installed there, check and install it.
extern void AotRamSample(void);

// Slow half of a RAM entry point: the module is not installed at the address
// being executed, or the guest has flushed its cache since the module was
// checked. Returns the module's base if its code is there, or AOT_NO_BASE
// after putting the original instruction back for the interpreter.
extern uae_u32 AotRamEntrySlow(uae_u32 module, uae_u32 offset);

// For tests: install a module at a guest address whose bytes match.
extern bool AotRamInstallAt(uae_u32 module, uae_u32 base);
extern void AotRamForgetAll(void);

#endif /* AOT_RUNTIME_H */
