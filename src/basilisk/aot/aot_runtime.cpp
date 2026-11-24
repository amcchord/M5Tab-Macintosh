/*
 *  aot_runtime.cpp - Install ahead-of-time translated 68k code
 */

#include "aot_runtime.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef ARDUINO
#include <esp_attr.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#endif

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#ifndef DRAM_ATTR
#define DRAM_ATTR
#endif

extern int CPUType;
extern void REGPARAM2 op_illg_1(uae_u32 opcode) REGPARAM;

int32 aot_extra_instructions SDATA_ATTR("aot_extra") = 0;
uae_u32 aot_region_calls SDATA_ATTR("aot_calls") = 0;
uae_u32 aot_flush_epoch SDATA_ATTR("aot_epoch") = 1;
const AotEntryInfo *aot_entry_infos_runtime SDATA_ATTR("aot_entry_infos") = NULL;
uaecptr aot_trap_vec_cached SDATA_ATTR("aot_trap_vec") = 1u;
bool aot_trap_vec_native SDATA_ATTR("aot_trap_ok") = false;
uae_u32 aot_cpusha_narrowed SDATA_ATTR("aot_cpusha_n") = 0;
uae_u32 aot_cpusha_global SDATA_ATTR("aot_cpusha_g") = 0;
uae_u64 aot_total_instructions = 0;

static AotStats aot_stats;
static bool aot_installed = false;
static bool aot_ram_enabled = false;
static uae_u32 aot_ram_opcode_base = 0;   // index of the first RAM entry's patch opcode

// 4 KiB page occupancy bitmap for installed RAM modules (1024 bytes covers 32 MiB).
static constexpr uae_u32 kAotPageShift = 12;
static constexpr uae_u32 kAotBitmapPages = 8192u;
static constexpr uae_u32 kAotBitmapWords = kAotBitmapPages / 32u;
static uae_u32 aot_page_bitmap[kAotBitmapWords] = {};
static bool aot_overflow_pages_installed = false;

// Builds without a generated image link this empty one.
__attribute__((weak)) AotModuleState aot_module_state[1];
__attribute__((weak)) extern const AotImage aot_image = {
	0, 0, NULL, NULL, NULL, NULL, 0, 0, 0,
	NULL, NULL, NULL, NULL, NULL, 0, 0, 0, 0,
	NULL, NULL,
	NULL, 0,
};

extern void m68k_mull(uae_u32, uae_u32, uae_u16);

static uaecptr aot_trap_vec_checked = 1u;

static bool aot_trap_verify(void)
{
	static const uae_u16 kTrapDispatcher[] = {
		0x2F0A, 0x2F02, 0x246F, 0x000A, 0x341A, 0x0C42, 0xA800, 0x6530,
		0x0442, 0xAC00, 0x641A, 0x2F70, 0x25A0, 0x1E00, 0x0008, 0x2F4A,
		0x000C, 0x241F, 0x245F, 0x4E75, 0x0000, 0x0000, 0x0000, 0x0000,
		0x2F70, 0x25A0, 0x0E00, 0x0008, 0x241F, 0x245F, 0x4E74, 0x0004,
		0x2F01, 0x2F09, 0x3202, 0x2F4A, 0x0014, 0x0242, 0x0100, 0x6620,
		0x1401, 0x2F08, 0x4EB0, 0x25A1, 0x0400, 0x205F, 0x225F, 0x221F,
		0x241F, 0x245F, 0x4A40, 0x584F, 0x4E75, 0x0000, 0x0000, 0x0000,
		0x1401, 0x4EB0, 0x2591, 0x225F, 0x221F, 0x241F, 0x245F, 0x4A40,
		0x584F, 0x4E75,
	};
	const uaecptr vector = get_long(regs.vbr + 4 * 10);
	if (vector != aot_trap_vec_checked) {
		aot_trap_vec_checked = vector;
		bool native = true;
		for (unsigned i = 0; i < sizeof(kTrapDispatcher) / sizeof(kTrapDispatcher[0]); ++i) {
			if (AotOriginalWord(vector + 2 * i) != kTrapDispatcher[i]) {
				native = false;
				break;
			}
		}
		aot_trap_vec_native = native;
		aot_trap_vec_cached = (regs.vbr == 0) ? vector : 1u;
	}
	return aot_trap_vec_native && regs.s && !regs.m && !regs.t0 && !regs.t1;
}

DRAM_ATTR
const AotFar aot_far = { AotRamEntrySlow, m68k_mull, Exception, aot_trap_verify };

struct AotOriginalEntry {
	uae_u32 offset;
	uae_u16 original;
};

static AotOriginalEntry *aot_entry_sorted = NULL;
static uae_u32 aot_entry_sorted_count = 0;

static const AotRange *rt_ram_ranges = NULL;
static const AotModule *rt_modules = NULL;
static const AotEntry *rt_ram_entries = NULL;
static const AotAnchor *rt_anchors = NULL;
static const uae_u16 *rt_patch_opcodes = NULL;

static void *aot_psram_allocs[7] = {};
static uae_u32 aot_psram_alloc_count = 0;

static void aot_free_runtime_tables(void)
{
	for (uae_u32 i = 0; i < aot_psram_alloc_count; ++i) {
		if (aot_psram_allocs[i] != NULL) {
#if defined(ARDUINO) && AOT_PSRAM
			heap_caps_free(aot_psram_allocs[i]);
#else
			free(aot_psram_allocs[i]);
#endif
			aot_psram_allocs[i] = NULL;
		}
	}
	aot_psram_alloc_count = 0;
	aot_entry_sorted = NULL;
	aot_entry_sorted_count = 0;
	aot_entry_infos_runtime = aot_image.entry_infos;
	rt_ram_ranges = aot_image.ram_ranges;
	rt_modules = aot_image.modules;
	rt_ram_entries = aot_image.ram_entries;
	rt_anchors = aot_image.anchors;
	rt_patch_opcodes = aot_image.patch_opcodes;
}

template <typename T>
static const T *aot_copy_table_to_psram(const T *src, uae_u32 count)
{
	if (src == NULL || count == 0)
		return src;
#if defined(ARDUINO) && AOT_PSRAM
	const size_t bytes = (size_t)count * sizeof(T);
	const size_t padded = (bytes + 63u) & ~(size_t)63u;
	void *copy = heap_caps_aligned_alloc(64, padded, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (copy == NULL)
		return src;
	memcpy(copy, src, bytes);
	if (padded > bytes)
		memset((uae_u8 *)copy + bytes, 0, padded - bytes);
	if (esp_cache_msync(copy, padded, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA) != ESP_OK) {
		heap_caps_free(copy);
		return src;
	}
	aot_psram_allocs[aot_psram_alloc_count++] = copy;
	return (const T *)copy;
#else
	return src;
#endif
}

static int aot_compare_original_entry(const void *a, const void *b)
{
	const AotOriginalEntry *ea = (const AotOriginalEntry *)a;
	const AotOriginalEntry *eb = (const AotOriginalEntry *)b;
	return (ea->offset > eb->offset) - (ea->offset < eb->offset);
}

#if defined(__riscv) && !defined(MEM_SLOW_DIRECT_CALLS)
extern "C" void aot_setpc_slow(uaecptr newpc)
{
	uae_u8 *host = get_real_address(newpc);
	regs.pc_p = regs.pc_oldp = host;
	regs.pc = newpc;
}

extern "C" IRAM_ATTR __attribute__((naked, used)) void aot_setpc_thunk(void)
{
	__asm__ volatile(
		"lui t1, %%hi(RAMSize)\n lw t1, %%lo(RAMSize)(t1)\n"
		"lui t2, %%hi(RAMBaseHost)\n lw t2, %%lo(RAMBaseHost)(t2)\n"
		"bgeu a0, t1, 2f\n"
		"add t1, t2, a0\n"
		"1:\n"
		"lui t2, %%hi(regs)\n addi t2, t2, %%lo(regs)\n"
		"sw t1, %c0(t2)\n"
		"sw t1, %c1(t2)\n"
		"sw a0, %c2(t2)\n"
		"jr t0\n"
		"2:\n"
		"lui t1, %%hi(ROMBaseMac)\n lw t1, %%lo(ROMBaseMac)(t1)\n sub t1, a0, t1\n"
		"lui t2, %%hi(ROMSize)\n lw t2, %%lo(ROMSize)(t2)\n"
		"bgeu t1, t2, 9f\n"
		"lui t2, %%hi(ROMBaseHost)\n lw t2, %%lo(ROMBaseHost)(t2)\n"
		"add t1, t1, t2\n"
		"j 1b\n"
		"9:\n"
		"addi sp, sp, -64\n"
		"sw ra, 0(sp)\n sw t0, 4(sp)\n"
		"sw a1, 8(sp)\n sw a2, 12(sp)\n sw a3, 16(sp)\n sw a4, 20(sp)\n"
		"sw a5, 24(sp)\n sw a6, 28(sp)\n sw a7, 32(sp)\n"
		"sw t3, 36(sp)\n sw t4, 40(sp)\n sw t5, 44(sp)\n sw t6, 48(sp)\n"
		"call aot_setpc_slow\n"
		"lw ra, 0(sp)\n lw t0, 4(sp)\n"
		"lw a1, 8(sp)\n lw a2, 12(sp)\n lw a3, 16(sp)\n lw a4, 20(sp)\n"
		"lw a5, 24(sp)\n lw a6, 28(sp)\n lw a7, 32(sp)\n"
		"lw t3, 36(sp)\n lw t4, 40(sp)\n lw t5, 44(sp)\n lw t6, 48(sp)\n"
		"addi sp, sp, 64\n"
		"jr t0\n"
		:
		: "i"(offsetof(regstruct, pc_p)),
		  "i"(offsetof(regstruct, pc_oldp)),
		  "i"(offsetof(regstruct, pc))
	);
}
#endif

static intptr_t aot_code_delta = 0;   // from the linked address to where the code runs

#if defined(ARDUINO) && AOT_PSRAM
// The translated code, bracketed by the generated file.
extern "C" {
extern const uae_u8 aot_blob_begin[] __attribute__((weak));
extern const uae_u8 aot_blob_end[] __attribute__((weak));
}

// Copy the translated code to PSRAM. The code was checked at build time to
// reach everything outside itself through absolute addresses, so the copy
// runs unchanged.
static void aot_move_code(void)
{
	aot_code_delta = 0;
	if (&aot_blob_begin[0] == NULL || &aot_blob_end[0] == NULL || aot_blob_end <= aot_blob_begin)
		return;
	const size_t size = (size_t)(aot_blob_end - aot_blob_begin);
	aot_stats.code_bytes = (uae_u32)size;
	const size_t padded = (size + 127) & ~(size_t)127;
	uae_u8 *copy = (uae_u8 *)heap_caps_aligned_alloc(128, padded, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (copy == NULL) {
		write_log("[AOT] no PSRAM for %u bytes of translated code; it runs from flash\n", (unsigned)size);
		return;
	}
	memcpy(copy, aot_blob_begin, size);
	memset(copy + size, 0, padded - size);
	if (esp_cache_msync(copy, padded, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA) != ESP_OK ||
	    esp_cache_msync(copy, padded, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST) != ESP_OK) {
		write_log("[AOT] cache synchronization failed; translated code runs from flash\n");
		heap_caps_free(copy);
		return;
	}
	aot_code_delta = copy - aot_blob_begin;
	aot_stats.code_in_psram = 1;
	write_log("[AOT] %u bytes of translated code run from PSRAM at %p\n", (unsigned)size, copy);
}

// Where a region function runs.
static cpuop_func *aot_handler(cpuop_func *linked)
{
	const uae_u8 *address = (const uae_u8 *)linked;
	if (aot_code_delta != 0 && address >= aot_blob_begin && address < aot_blob_end)
		return (cpuop_func *)(address + aot_code_delta);
	return linked;
}
#else
static void aot_move_code(void) {}
static cpuop_func *aot_handler(cpuop_func *linked) { return linked; }
#endif

static uae_u32 aot_crc32(uae_u32 crc, const uae_u8 *data, uae_u32 length)
{
	crc = ~crc;
	while (length--) {
		crc ^= *data++;
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
	}
	return ~crc;
}

// FNV-1a over 16-bit units. Guest code is word aligned and so is guest RAM
// in host memory, so these are aligned loads.
static ALWAYS_INLINE uae_u32 aot_sum16(uae_u32 value, const uae_u8 *data, uae_u32 bytes)
{
	uae_u32 words_left = bytes >> 1;
	const uae_u16 *words = (const uae_u16 *)data;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	if (words_left > 0 && (((uintptr_t)words) & 2u) != 0) {
		value ^= *words++;
		value *= 0x01000193u;
		--words_left;
	}
	const uae_u32 *dwords = (const uae_u32 *)words;
	while (words_left >= 4) {
		const uae_u32 w0 = dwords[0];
		const uae_u32 w1 = dwords[1];
		dwords += 2;
		words_left -= 4;
		value = (value ^ (w0 & 0xffffu)) * 0x01000193u;
		value = (value ^ (w0 >> 16)) * 0x01000193u;
		value = (value ^ (w1 & 0xffffu)) * 0x01000193u;
		value = (value ^ (w1 >> 16)) * 0x01000193u;
	}
	while (words_left >= 2) {
		const uae_u32 w0 = *dwords++;
		words_left -= 2;
		value = (value ^ (w0 & 0xffffu)) * 0x01000193u;
		value = (value ^ (w0 >> 16)) * 0x01000193u;
	}
	words = (const uae_u16 *)dwords;
#endif
	while (words_left-- > 0) {
		value ^= *words++;
		value *= 0x01000193u;
	}
	return value;
}

// Opcode table index of a big-endian opcode (see cft_map() in newcpu.cpp).
static inline uae_u32 aot_table_index(uae_u32 opcode)
{
#ifdef HAVE_GET_WORD_UNSWAPPED
	return ((opcode >> 8) & 255) | ((opcode & 255) << 8);
#else
	return opcode;
#endif
}

static ALWAYS_INLINE uae_u32 host_word(const uae_u8 *host)
{
	return do_get_mem_word((uae_u16 *)host);
}

static ALWAYS_INLINE bool aot_lookup_original_entry(uae_u32 offset, uae_u16 *original_out)
{
	const AotOriginalEntry *entries = aot_entry_sorted;
	if (entries != NULL) {
		uae_u32 low = 0;
		uae_u32 high = aot_entry_sorted_count;
		while (low < high) {
			const uae_u32 mid = low + ((high - low) >> 1);
			const uae_u32 mid_off = entries[mid].offset;
			if (mid_off < offset) {
				low = mid + 1;
			} else if (mid_off > offset) {
				high = mid;
			} else {
				*original_out = entries[mid].original;
				return true;
			}
		}
		return false;
	}
	const AotImage &image = aot_image;
	for (uae_u32 e = 0; e < image.entry_count; ++e) {
		if (image.entries[e].offset == offset) {
			*original_out = image.entries[e].original;
			return true;
		}
	}
	return false;
}

static inline void aot_bitmap_clear(void)
{
	memset(aot_page_bitmap, 0, sizeof(aot_page_bitmap));
	aot_overflow_pages_installed = false;
}

static inline void aot_bitmap_set_range(uae_u32 base, uae_u32 span)
{
	if (span == 0)
		return;
	const uae_u32 first_page = base >> kAotPageShift;
	const uae_u32 last_page = (base + span - 1u) >> kAotPageShift;
	if (last_page >= kAotBitmapPages)
		aot_overflow_pages_installed = true;
	if (first_page >= kAotBitmapPages)
		return;
	const uae_u32 clamped_last = last_page < kAotBitmapPages ? last_page : (kAotBitmapPages - 1u);
	for (uae_u32 page = first_page; page <= clamped_last; ++page)
		aot_page_bitmap[page >> 5] |= 1u << (page & 31u);
}

void AotReadStats(AotStats *stats)
{
	*stats = aot_stats;
	uae_u32 installed = 0;
	for (uae_u32 m = 0; m < aot_image.module_count; ++m) {
		if (aot_image.module_state[m].base != AOT_NO_BASE)
			++installed;
	}
	stats->modules_installed = installed;
	stats->cpusha_narrowed = aot_cpusha_narrowed;
	stats->cpusha_global = aot_cpusha_global;
	// A 32-bit call counter wraps after hours; callers take differences.
	stats->calls = aot_region_calls;
	stats->instructions = aot_total_instructions + (uae_u64)aot_region_calls;
}

uae_u32 AotOriginalWord(uaecptr address)
{
	const AotImage &image = aot_image;
	const uae_u32 offset = address - image.rom_base;
	uae_u16 original = 0;
	if (aot_installed && offset < image.rom_size && aot_lookup_original_entry(offset, &original))
		return original;
	return get_word(address);
}

void AotInstall(void)
{
	const AotImage &image = aot_image;
	aot_free_runtime_tables();
	aot_stats = AotStats();
	aot_stats.regions_total = image.region_count;
	aot_stats.entries_total = image.entry_count;
	aot_stats.modules_total = image.module_count;
	aot_cpusha_narrowed = 0;
	aot_cpusha_global = 0;
	aot_trap_vec_checked = 1u;
	aot_trap_vec_cached = 1u;
	aot_trap_vec_native = false;
	aot_installed = false;
	aot_ram_enabled = false;
	aot_bitmap_clear();
	for (uae_u32 m = 0; m < image.module_count; ++m) {
		image.module_state[m].base = AOT_NO_BASE;
		image.module_state[m].epoch = 0;
	}
	if (!AOT_ROM || image.entry_count + image.ram_entry_count == 0) {
		write_log("[AOT] no translated image in this build\n");
		return;
	}
	if (CPUType != 4 || ROMBaseMac != image.rom_base || ROMSize != image.rom_size) {
		write_log("[AOT] disabled: CPU %d, ROM base %08x size %u do not match the image\n",
		          CPUType, (unsigned)ROMBaseMac, (unsigned)ROMSize);
		return;
	}
	uae_u8 *region_ok = (uae_u8 *)calloc(image.region_count + 1, 1);
	if (region_ok == NULL) {
		write_log("[AOT] disabled: out of memory\n");
		return;
	}

	aot_move_code();
	aot_entry_infos_runtime = aot_copy_table_to_psram(image.entry_infos, image.entry_info_count);
	rt_ram_ranges = aot_copy_table_to_psram(image.ram_ranges, image.ram_range_count);
	rt_modules = aot_copy_table_to_psram(image.modules, image.module_count);
	rt_ram_entries = aot_copy_table_to_psram(image.ram_entries, image.ram_entry_count);
	rt_anchors = aot_copy_table_to_psram(image.anchors, image.anchor_count);
	rt_patch_opcodes = aot_copy_table_to_psram(image.patch_opcodes, image.entry_count + image.ram_entry_count);

	if (image.entry_count > 0) {
		const size_t bytes = (size_t)image.entry_count * sizeof(AotOriginalEntry);
		AotOriginalEntry *sorted = (AotOriginalEntry *)
#if defined(ARDUINO) && AOT_PSRAM
			heap_caps_aligned_alloc(64, (bytes + 63u) & ~(size_t)63u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
			malloc(bytes);
#endif
		if (sorted != NULL) {
			for (uae_u32 e = 0; e < image.entry_count; ++e) {
				sorted[e].offset = image.entries[e].offset;
				sorted[e].original = image.entries[e].original;
			}
			qsort(sorted, image.entry_count, sizeof(AotOriginalEntry), aot_compare_original_entry);
#if defined(ARDUINO) && AOT_PSRAM
			esp_cache_msync(sorted, (bytes + 63u) & ~(size_t)63u,
			                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
#endif
			aot_psram_allocs[aot_psram_alloc_count++] = sorted;
			aot_entry_sorted = sorted;
			aot_entry_sorted_count = image.entry_count;
		}
	}

	// Check every region against the ROM before changing any byte of it:
	// entry points of one region lie inside the ranges of others.
	for (uae_u32 r = 0; r < image.region_count; ++r) {
		const AotRegion &region = image.regions[r];
		uae_u32 crc = 0;
		bool in_range = true;
		for (uae_u32 i = 0; i < region.range_count; ++i) {
			const AotRange &range = image.ranges[region.first_range + i];
			if (range.offset > ROMSize || range.length > ROMSize - range.offset) {
				in_range = false;
				break;
			}
			crc = aot_crc32(crc, ROMBaseHost + range.offset, range.length);
		}
		if (in_range && crc == region.crc) {
			region_ok[r] = 1;
			aot_stats.regions_installed++;
		}
	}

	for (uae_u32 e = 0; e < image.entry_count; ++e) {
		const AotEntry &entry = image.entries[e];
		if (!region_ok[entry.owner])
			continue;
		const uae_u32 slot = aot_table_index(image.patch_opcodes[e]);
		if (cpufunctbl[slot] != op_illg_1)
			continue;  // the opcode is in use after all
		uae_u8 *host = ROMBaseHost + entry.offset;
		cpufunctbl[slot] = aot_handler(entry.handler);
		host[0] = (uae_u8)(image.patch_opcodes[e] >> 8);
		host[1] = (uae_u8)image.patch_opcodes[e];
		aot_stats.entries_installed++;
	}
	free(region_ok);
	aot_installed = true;

	// RAM entry handlers are always reachable; the opcodes are written into
	// guest RAM only where a module is installed.
	aot_ram_opcode_base = image.entry_count;
#if AOT_RAM
	aot_ram_enabled = image.module_count != 0;
	for (uae_u32 e = 0; aot_ram_enabled && e < image.ram_entry_count; ++e) {
		const uae_u32 slot = aot_table_index(image.patch_opcodes[aot_ram_opcode_base + e]);
		if (cpufunctbl[slot] != op_illg_1) {
			aot_ram_enabled = false;
			break;
		}
	}
	for (uae_u32 e = 0; aot_ram_enabled && e < image.ram_entry_count; ++e)
		cpufunctbl[aot_table_index(image.patch_opcodes[aot_ram_opcode_base + e])] =
			aot_handler(image.ram_entries[e].handler);
#endif
	write_log("[AOT] ROM: installed %u of %u regions, %u of %u entry points; RAM: %u modules %s\n",
	          (unsigned)aot_stats.regions_installed, (unsigned)aot_stats.regions_total,
	          (unsigned)aot_stats.entries_installed, (unsigned)aot_stats.entries_total,
	          (unsigned)image.module_count, aot_ram_enabled ? "armed" : "off");
}

__attribute__((noinline))
void AotCodeChanged(void)
{
	if (++aot_flush_epoch == 0)
		aot_flush_epoch = 1;
	aot_stats.flushes++;
	aot_bitmap_clear();
}

__attribute__((noinline))
static void aot_invalidate_overlapping_modules(uaecptr addr, uae_u32 end)
{
	const AotImage &image = aot_image;
	const AotModule *modules = rt_modules ? rt_modules : image.modules;
	for (uae_u32 m = 0; m < image.module_count; ++m) {
		AotModuleState &state = image.module_state[m];
		if (state.base == AOT_NO_BASE)
			continue;
		const uae_u32 m_start = state.base;
		const uae_u32 m_span = modules[m].span;
		if (m_span == 0)
			continue;
		if (m_start < end && (m_start >= addr || m_span > addr - m_start)) {
			if (state.epoch != 0) {
				state.epoch = 0;
				aot_stats.range_invalidations++;
			}
		}
	}
}

IRAM_ATTR
void AotCodeChangedGuestRange(uaecptr addr, uae_u32 size)
{
	if (!aot_ram_enabled || size == 0 || addr >= RAMSize)
		return;
	const uae_u32 max_size = RAMSize - addr;
	if (size > max_size)
		size = max_size;
	const uae_u32 end = addr + size;
	const uae_u32 first_page = addr >> kAotPageShift;
	const uae_u32 last_page = (end - 1u) >> kAotPageShift;
	if (first_page == last_page && first_page < kAotBitmapPages) {
		if (((aot_page_bitmap[first_page >> 5] >> (first_page & 31u)) & 1u) == 0) {
			aot_stats.range_flushes_skipped++;
			return;
		}
	} else {
		bool hit = (last_page >= kAotBitmapPages) && aot_overflow_pages_installed;
		if (!hit && first_page < kAotBitmapPages) {
			const uae_u32 clamped_last = last_page < kAotBitmapPages ? last_page : (kAotBitmapPages - 1u);
			const uae_u32 first_word = first_page >> 5;
			const uae_u32 last_word = clamped_last >> 5;
			for (uae_u32 w = first_word; w <= last_word; ++w) {
				const uae_u32 lo = (w == first_word) ? (first_page & 31u) : 0u;
				const uae_u32 hi = (w == last_word) ? (clamped_last & 31u) : 31u;
				const uae_u32 mask = (~0u << lo) & (~0u >> (31u - hi));
				if ((aot_page_bitmap[w] & mask) != 0) {
					hit = true;
					break;
				}
			}
		}
		if (!hit) {
			aot_stats.range_flushes_skipped++;
			return;
		}
	}
	aot_invalidate_overlapping_modules(addr, end);
}

void AotCodeChangedRange(const uae_u8 *host, uae_u32 size)
{
	if (RAMBaseHost == NULL || host < RAMBaseHost || size == 0)
		return;
	const uintptr_t offset = (uintptr_t)(host - RAMBaseHost);
	if (offset >= RAMSize)
		return;
	AotCodeChangedGuestRange((uaecptr)offset, size);
}

IRAM_ATTR
void AotHandleCpusha(void)
{
	static const uae_u16 kHLockSeq[10] = {
		0x08e8, 0x0007, 0xfff5, 0x0c38, 0x0004,
		0x012f, 0x6606, 0x4eb0, 0x81e1, 0x06f4,
	};
	if (ROMBaseHost == NULL || ROMSize < 4 || regs.pc_p < ROMBaseHost) {
		aot_cpusha_global++;
		AotCodeChanged();
		return;
	}
	const uintptr_t pc_off = (uintptr_t)(regs.pc_p - ROMBaseHost);
	if (pc_off > ROMSize - 4) {
		aot_cpusha_global++;
		AotCodeChanged();
		return;
	}
	const uae_u32 op0 = host_word(regs.pc_p);
	if ((op0 & 0xff38u) != 0xf438u || (op0 & 0x0080u) == 0) {
		aot_cpusha_global++;
		AotCodeChanged();
		return;
	}
	const uae_u32 op1 = host_word(regs.pc_p + 2);
	if (op1 != 0x4e75u) {
		uae_u16 orig1 = 0;
		if (!aot_installed || !aot_lookup_original_entry((uae_u32)pc_off + 2u, &orig1) || orig1 != 0x4e75u) {
			aot_cpusha_global++;
			AotCodeChanged();
			return;
		}
	}
	const uaecptr sp = m68k_areg(regs, 7);
	if (RAMBaseHost == NULL || RAMSize < 4 || sp > RAMSize - 4) {
		aot_cpusha_global++;
		AotCodeChanged();
		return;
	}
	const uaecptr ret_addr = do_get_mem_long((uae_u32 *)(RAMBaseHost + sp));
	if (ret_addr < ROMBaseMac) {
		aot_cpusha_global++;
		AotCodeChanged();
		return;
	}
	const uae_u32 ret_off = ret_addr - ROMBaseMac;
	if (ret_off < 20u || ret_off > ROMSize || (ret_off & 1u) != 0) {
		aot_cpusha_global++;
		AotCodeChanged();
		return;
	}
	for (uae_u32 i = 0; i < 10u; ++i) {
		const uae_u32 w_off = ret_off - 20u + 2u * i;
		const uae_u32 w = host_word(ROMBaseHost + w_off);
		if (w != kHLockSeq[i]) {
			uae_u16 orig = 0;
			if (!aot_installed || !aot_lookup_original_entry(w_off, &orig) || orig != kHLockSeq[i]) {
				aot_cpusha_global++;
				AotCodeChanged();
				return;
			}
		}
	}
	const uaecptr ptr = m68k_areg(regs, 0);
	if (RAMSize < 12u || ptr < 12u || ptr >= RAMSize) {
		aot_cpusha_global++;
		AotCodeChanged();
		return;
	}
	const uae_u32 block_start = ptr - 12u;
	const uae_u32 hdr0 = do_get_mem_long((uae_u32 *)(RAMBaseHost + block_start));
	if (((hdr0 >> 24) & 0x80u) == 0) {
		aot_cpusha_global++;
		AotCodeChanged();
		return;
	}
	const uae_u32 hdr_size = do_get_mem_long((uae_u32 *)(RAMBaseHost + block_start + 4u));
	if (hdr_size < 12u || hdr_size > RAMSize - block_start) {
		aot_cpusha_global++;
		AotCodeChanged();
		return;
	}
	aot_cpusha_narrowed++;
	AotCodeChangedGuestRange(block_start, hdr_size);
}

// Does guest RAM at `base` hold the module's code? Entry opcodes may be
// either the original instruction or the module's entry opcode.
IRAM_ATTR
static bool module_matches(uae_u32 index, uae_u32 base)
{
	const AotImage &image = aot_image;
	const AotModule *modules = rt_modules ? rt_modules : image.modules;
	const AotRange *ram_ranges = rt_ram_ranges ? rt_ram_ranges : image.ram_ranges;
	const AotEntry *ram_entries = rt_ram_entries ? rt_ram_entries : image.ram_entries;
	const uae_u16 *patch_opcodes = rt_patch_opcodes ? rt_patch_opcodes : image.patch_opcodes;
	const AotModule &module = modules[index];
	aot_stats.module_checks++;
	if ((base & 1) || base >= RAMSize || module.span > RAMSize - base)
		return false;
	const uae_u8 *host = RAMBaseHost + base;
	uae_u32 sum = 0x811C9DC5u;
	for (uae_u32 i = 0; i < module.range_count; ++i) {
		const AotRange &range = ram_ranges[module.first_range + i];
		sum = aot_sum16(sum, host + range.offset, range.length);
	}
	if (sum != module.checksum)
		return false;
	for (uae_u32 i = 0; i < module.entry_count; ++i) {
		const AotEntry &entry = ram_entries[module.first_entry + i];
		const uae_u32 word = host_word(host + entry.offset);
		if (word != entry.original &&
		    word != patch_opcodes[aot_ram_opcode_base + module.first_entry + i])
			return false;
	}
	return true;
}

__attribute__((noinline))
static void module_install(uae_u32 index, uae_u32 base)
{
	const AotImage &image = aot_image;
	const AotModule *modules = rt_modules ? rt_modules : image.modules;
	const AotEntry *ram_entries = rt_ram_entries ? rt_ram_entries : image.ram_entries;
	const uae_u16 *patch_opcodes = rt_patch_opcodes ? rt_patch_opcodes : image.patch_opcodes;
	const AotModule &module = modules[index];
	uae_u8 *host = RAMBaseHost + base;
	for (uae_u32 i = 0; i < module.entry_count; ++i) {
		const AotEntry &entry = ram_entries[module.first_entry + i];
		const uae_u16 opcode = patch_opcodes[aot_ram_opcode_base + module.first_entry + i];
		host[entry.offset] = (uae_u8)(opcode >> 8);
		host[entry.offset + 1] = (uae_u8)opcode;
	}
	image.module_state[index].base = base;
	image.module_state[index].epoch = aot_flush_epoch;
	aot_stats.module_installs++;
	aot_bitmap_set_range(base, module.span);
}

bool AotRamInstallAt(uae_u32 index, uae_u32 base)
{
	if (!aot_ram_enabled || index >= aot_image.module_count || !module_matches(index, base))
		return false;
	module_install(index, base);
	return true;
}

void AotRamForgetAll(void)
{
	aot_bitmap_clear();
	for (uae_u32 m = 0; m < aot_image.module_count; ++m) {
		aot_image.module_state[m].base = AOT_NO_BASE;
		aot_image.module_state[m].epoch = 0;
	}
}

IRAM_ATTR
uae_u32 AotRamEntrySlow(uae_u32 index, uae_u32 offset)
{
	const AotImage &image = aot_image;
	AotModuleState &state = image.module_state[index];
	const uae_u32 ram_offset = (uae_u32)(regs.pc_p - RAMBaseHost);
	if (!aot_ram_enabled || ram_offset >= RAMSize || ram_offset < offset ||
	    (regs.spcflags & (SPCFLAG_TRACE | SPCFLAG_DOTRACE)))
		return AOT_NO_BASE;
	const uae_u32 base = ram_offset - offset;
	if (module_matches(index, base)) {
		const AotModule *modules = rt_modules ? rt_modules : image.modules;
		if (state.base != base) {
			// The code moved, or a copy of it runs here: make every entry
			// of this copy an entry point.
			module_install(index, base);
		} else {
			aot_bitmap_set_range(base, modules[index].span);
		}
		state.epoch = aot_flush_epoch;
		return base;
	}
	aot_stats.module_rejects++;
	if (state.base == base)
		state.base = AOT_NO_BASE;
	// The word being executed is this module's entry opcode, but the code
	// around it is no longer the module's. It is an instruction, because it
	// is being executed: put the original back.
	const AotModule *modules = rt_modules ? rt_modules : image.modules;
	const AotEntry *ram_entries = rt_ram_entries ? rt_ram_entries : image.ram_entries;
	const AotModule &module = modules[index];
	for (uae_u32 i = 0; i < module.entry_count; ++i) {
		const AotEntry &entry = ram_entries[module.first_entry + i];
		if (entry.offset == offset) {
			regs.pc_p[0] = (uae_u8)(entry.original >> 8);
			regs.pc_p[1] = (uae_u8)entry.original;
			break;
		}
	}
	return AOT_NO_BASE;
}

void AotRamSample(void)
{
	const AotImage &image = aot_image;
	if (!aot_ram_enabled)
		return;
	const uae_u32 pc = (uae_u32)(regs.pc_p - RAMBaseHost);
	if (pc >= RAMSize || RAMSize - pc < 8 || (pc & 1))
		return;
	const AotAnchor *anchors = rt_anchors ? rt_anchors : image.anchors;
	const uae_u32 hash = aot_sum16(0x811C9DC5u, regs.pc_p, 8);
	uae_u32 low = 0, high = image.anchor_count;
	while (low < high) {
		const uae_u32 middle = (low + high) / 2;
		if (anchors[middle].hash < hash)
			low = middle + 1;
		else
			high = middle;
	}
	for (; low < image.anchor_count && anchors[low].hash == hash; ++low) {
		const AotAnchor &anchor = anchors[low];
		if (pc < anchor.offset)
			continue;
		const uae_u32 base = pc - anchor.offset;
		AotModuleState &state = image.module_state[anchor.module];
		if (state.base == base)
			continue;
		if (module_matches(anchor.module, base))
			module_install(anchor.module, base);
	}
}
