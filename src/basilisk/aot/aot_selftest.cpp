/*
 *  aot_selftest.cpp - Differential test of translated ROM code
 */

#include "aot_selftest.h"

#if AOT_SELFTEST || defined(AOT_HOST_TEST)

#include "aot_runtime.h"

#include <stdlib.h>
#include <string.h>

#ifdef ARDUINO
#include <esp_heap_caps.h>
#define TEST_ALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM)
#else
#define TEST_ALLOC(size) malloc(size)
#endif

namespace {

const uae_u32 kPadding = 64;
const uae_u32 kFrameSize = 0x10000;     // a frame buffer for the test, at the usual address
const uae_u32 kMaxReference = 400000;   // a region gives up after 65536 instructions plus one pass
const uae_u32 kMaxJournal = 4096;

struct OutsideWrite {
	uae_u32 address;
	uae_u32 value;
	uae_u32 size;
};

struct Journal {
	OutsideWrite *writes;
	uae_u32 count;
	bool overflow;
};

Journal *active_journal = NULL;
uae_u8 *wild_page = NULL;

// Everything outside test RAM and ROM: loads read zero, stores are recorded.
uae_u32 REGPARAM2 wild_get(uaecptr) { return 0; }
void record(uaecptr address, uae_u32 value, uae_u32 size)
{
	Journal *journal = active_journal;
	if (journal->count < kMaxJournal) {
		journal->writes[journal->count].address = address;
		journal->writes[journal->count].value = value;
		journal->writes[journal->count].size = size;
		journal->count++;
	} else {
		journal->overflow = true;
	}
}
void REGPARAM2 wild_lput(uaecptr address, uae_u32 value) { record(address, value, 4); }
void REGPARAM2 wild_wput(uaecptr address, uae_u32 value) { record(address, value & 0xffff, 2); }
void REGPARAM2 wild_bput(uaecptr address, uae_u32 value) { record(address, value & 0xff, 1); }
uae_u8 *REGPARAM2 wild_xlate(uaecptr address) { return wild_page + (address & 0xffff); }
addrbank wild_bank = {
	wild_get, wild_get, wild_get, wild_lput, wild_wput, wild_bput, wild_xlate,
};

struct Rng {
	uae_u64 state;
	uae_u32 next()
	{
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return (uae_u32)(state >> 16);
	}
	uae_u32 below(uae_u32 n) { return next() % n; }
};

struct CpuState {
	uae_u32 regs[16];
	unsigned int c, z, n, v, x;
	uae_u32 pc;
	uae_u32 spcflags;
	uae_u16 sr;
	uae_u8 s, m, t0, t1;
	int intmask;
	uae_u32 usp, isp, msp, vbr;
};

void apply(const CpuState &state)
{
	memcpy(regs.regs, state.regs, sizeof(regs.regs));
	regflags.c = state.c;
	regflags.z = state.z;
	regflags.n = state.n;
	regflags.v = state.v;
	regflags.x = state.x;
	SPCFLAGS_INIT(state.spcflags);
	regs.sr = state.sr;
	regs.s = state.s;
	regs.m = state.m;
	regs.t0 = state.t0;
	regs.t1 = state.t1;
	regs.intmask = state.intmask;
	regs.usp = state.usp;
	regs.isp = state.isp;
	regs.msp = state.msp;
	regs.vbr = state.vbr;
	regs.stopped = 0;
	m68k_setpc(state.pc);
}

void capture(CpuState *state)
{
	memset(state, 0, sizeof(*state));
	memcpy(state->regs, regs.regs, sizeof(regs.regs));
	state->c = regflags.c;
	state->z = regflags.z;
	state->n = regflags.n;
	state->v = regflags.v;
	state->x = regflags.x;
	state->pc = m68k_getpc();
	state->spcflags = regs.spcflags;
	state->sr = regs.sr;
	state->s = regs.s;
	state->m = regs.m;
	state->t0 = regs.t0;
	state->t1 = regs.t1;
	state->intmask = regs.intmask;
	state->usp = regs.usp;
	state->isp = regs.isp;
	state->msp = regs.msp;
	state->vbr = regs.vbr;
}

bool same(const CpuState &a, const CpuState &b)
{
	return memcmp(a.regs, b.regs, sizeof(a.regs)) == 0 && a.c == b.c && a.z == b.z &&
	       a.n == b.n && a.v == b.v && a.x == b.x && a.pc == b.pc && a.spcflags == b.spcflags &&
	       a.sr == b.sr && a.s == b.s && a.m == b.m && a.t0 == b.t0 && a.t1 == b.t1 &&
	       a.intmask == b.intmask && a.usp == b.usp && a.isp == b.isp && a.msp == b.msp;
}

void print_state(const char *title, const CpuState &s)
{
	write_log("  %s pc=%08x flags c%u z%u n%u v%u x%u spc=%x\n", title, (unsigned)s.pc, s.c, s.z, s.n, s.v, s.x,
	          (unsigned)s.spcflags);
	for (int row = 0; row < 2; ++row) {
		const uae_u32 *r = s.regs + 8 * row;
		const char name = row ? 'A' : 'D';
		write_log("   %c0=%08x %c1=%08x %c2=%08x %c3=%08x %c4=%08x %c5=%08x %c6=%08x %c7=%08x\n",
		          name, (unsigned)r[0], name, (unsigned)r[1], name, (unsigned)r[2], name, (unsigned)r[3],
		          name, (unsigned)r[4], name, (unsigned)r[5], name, (unsigned)r[6], name, (unsigned)r[7]);
	}
}

// Pointers stay out of [avoid_low, avoid_high): code under test there holds
// entry opcodes in one run and the original instructions in the other.
void random_state(Rng &rng, uae_u32 pc, uae_u32 ram_size, CpuState *state,
                  uae_u32 avoid_low = 0, uae_u32 avoid_high = 0)
{
	const AotImage &image = aot_image;
	memset(state, 0, sizeof(*state));
	for (int i = 0; i < 8; ++i) {
		switch (rng.below(4)) {
		case 0: state->regs[i] = rng.below(40); break;
		case 1: state->regs[i] = rng.below(0x400); break;
		case 2: state->regs[i] = (rng.next() & 0xffff0000u) | rng.below(24); break;
		default: state->regs[i] = rng.next(); break;
		}
	}
	for (int i = 8; i < 16; ++i) {
		uae_u32 address = 0x2000 + rng.below(ram_size - 0x4000);
		switch (rng.below(16)) {
		case 0: address |= 1; break;                                           // odd
		case 1: address = image.rom_base + rng.below(image.rom_size - 16); break;  // ROM
		case 2: address = 0xa0000000u + rng.below(0x10000); break;              // outside RAM and ROM
		case 3: address = ram_size - 2 - rng.below(8); break;                   // straddles the end of RAM
		default: address &= ~1u; break;
		}
		if (address + 0x200 > avoid_low && address < avoid_high + 0x200)
			address = (avoid_high + 0x400 < ram_size - 0x2000) ? avoid_high + 0x400 : 0x2000;
		state->regs[i] = address;
	}
	state->regs[15] = (0x4000 + rng.below(ram_size - 0x8000)) & ~1u;
	if (state->regs[15] + 0x400 > avoid_low && state->regs[15] < avoid_high + 0x400)
		state->regs[15] = (avoid_high + 0x800 < ram_size - 0x2000) ? avoid_high + 0x800 : 0x3000;
	state->c = rng.below(2);
	state->z = rng.below(2);
	state->n = rng.below(2);
	state->v = rng.below(2);
	state->x = rng.below(2);
	state->pc = pc;
	state->s = 1;
	state->intmask = 7;
	state->sr = 0x2700;
	state->isp = state->regs[15];
	state->usp = 0x3000;
	state->msp = 0x3800;
}

struct Outcome {
	CpuState state;
	Journal journal;
	uae_u32 instructions;
};

bool same_journal(const Journal &a, const Journal &b)
{
	return a.count == b.count && a.overflow == b.overflow &&
	       memcmp(a.writes, b.writes, a.count * sizeof(OutsideWrite)) == 0;
}

struct Tester {
	AotSelfTestOptions options;
	AotSelfTestResult result;
	const uae_u8 *rom_pristine;
	uae_u8 *rom_patched;
	uae_u8 *ram_master, *ram_reference, *ram_translated;
	uae_u8 *frame_master, *frame_reference, *frame_translated;
	Outcome reference, translated;
	Rng rng;

	void map(uae_u8 *ram, const uae_u8 *rom, Journal *journal)
	{
		RAMBaseHost = ram;
		ROMBaseHost = (uae_u8 *)rom;
		// Every run starts from the same frame buffer.
		uae_u8 *frame = ram == ram_reference ? frame_reference : frame_translated;
		memcpy(frame, frame_master, kFrameSize + kPadding);
		MacFrameBaseHost = frame;
		journal->count = 0;
		journal->overflow = false;
		active_journal = journal;
	}

	bool compare(const char *pass, uae_u32 entry, uae_u32 trial, const CpuState &initial)
	{
		return compare_at(pass, entry, aot_image.entries[entry].offset, trial, initial);
	}

	bool compare_at(const char *pass, uae_u32 entry, uae_u32 offset, uae_u32 trial, const CpuState &initial)
	{
		const bool state_ok = same(reference.state, translated.state);
		const bool ram_ok = memcmp(ram_reference, ram_translated, options.ram_size + kPadding) == 0;
		const bool writes_ok = same_journal(reference.journal, translated.journal);
		const bool frame_ok = memcmp(frame_reference, frame_translated, kFrameSize + kPadding) == 0;
		if (state_ok && ram_ok && writes_ok && frame_ok)
			return true;
		if (options.verbose || result.failures < 5) {
			write_log("[AOT TEST] MISMATCH %s entry %u (+%x) trial %u after %u instructions:%s%s%s%s\n", pass,
			          (unsigned)entry, (unsigned)offset, (unsigned)trial,
			          (unsigned)translated.instructions, state_ok ? "" : " cpu-state", ram_ok ? "" : " ram",
			          writes_ok ? "" : " outside-writes", frame_ok ? "" : " frame-buffer");
			print_state("initial    ", initial);
			print_state("interpreter", reference.state);
			print_state("translated ", translated.state);
			int shown = 0;
			for (uae_u32 i = 0; !ram_ok && i < options.ram_size + kPadding && shown < 8; ++i) {
				if (ram_reference[i] != ram_translated[i]) {
					write_log("   ram[%06x] interpreter %02x translated %02x\n", (unsigned)i, ram_reference[i],
					          ram_translated[i]);
					++shown;
				}
			}
		}
		return false;
	}

	// One region call, entered the way `pass` says.
	// An entry opcode in the form handlers are called with. `index` counts
	// ROM entries first, then RAM entries.
	static uae_u32 handler_opcode(uae_u32 index)
	{
		const uae_u32 opcode = aot_image.patch_opcodes[index];
#ifdef HAVE_GET_WORD_UNSWAPPED
		return ((opcode & 0xff) << 8) | (opcode >> 8);
#else
		return opcode;
#endif
	}

	void run_translated(int pass, uae_u32 index, const AotEntry &entry, const CpuState &initial)
	{
		memcpy(ram_translated, ram_master, options.ram_size + kPadding);
		map(ram_translated, pass ? rom_patched : rom_pristine, &translated.journal);
		apply(initial);
		aot_extra_instructions = 0;
		const uae_u32 opcode = do_get_mem_word_unswapped(regs.pc_p);
		if (pass == 0)
			entry.handler(handler_opcode(index));
		else
			(*cpufunctbl[opcode])(opcode);
		translated.instructions = 1 + aot_extra_instructions;
		aot_extra_instructions = 0;
		capture(&translated.state);
	}

	// The interpreter, for as many instructions. Both runs must see the same
	// ROM contents as data, so after installation the interpreter runs on
	// the patched ROM and is handed the original opcode at entry points.
	void run_reference(int pass, const CpuState &initial)
	{
		memcpy(ram_reference, ram_master, options.ram_size + kPadding);
		map(ram_reference, pass ? rom_patched : rom_pristine, &reference.journal);
		apply(initial);
		for (uae_u32 i = 0; i < translated.instructions; ++i) {
			uae_u32 opcode = do_get_mem_word_unswapped(regs.pc_p);
			if (pass == 1) {
				const uae_u32 original = AotOriginalWord(m68k_getpc());
				opcode = ((original & 0xff) << 8) | (original >> 8);
			}
			if (options.trace)
				write_log("   step %u pc=%08x opcode=%04x\n", (unsigned)i, (unsigned)m68k_getpc(),
				          (unsigned)(((opcode & 0xff) << 8) | (opcode >> 8)));
			(*cpufunctbl[opcode])(opcode);
		}
		reference.instructions = translated.instructions;
		capture(&reference.state);
	}

	void entries(int pass)
	{
		const AotImage &image = aot_image;
		for (uae_u32 e = 0; e < image.entry_count; ++e) {
			if (options.only_entry >= 0 && (uae_u32)options.only_entry != e)
				continue;
			const AotEntry &entry = image.entries[e];
			for (uae_u32 trial = 0; trial < options.trials; ++trial) {
				CpuState initial;
				random_state(rng, image.rom_base + entry.offset, options.ram_size, &initial);
				run_translated(pass, e, entry, initial);
				if (translated.instructions > kMaxReference) {
					result.skipped++;
					continue;
				}
				run_reference(pass, initial);
				result.runs++;
				result.instructions += translated.instructions;
				if (!compare(pass ? "installed" : "direct", e, trial, initial))
					result.failures++;
			}
		}
	}

	// ---- RAM modules ----

	const uae_u8 *module_code(uae_u32 m) const
	{
		return aot_image.module_code + aot_image.module_code_offsets[m];
	}

	// What module_install() writes: the module's entry opcodes.
	void write_entry_opcodes(uae_u8 *ram, uae_u32 m, uae_u32 base)
	{
		const AotImage &image = aot_image;
		const AotModule &module = image.modules[m];
		for (uae_u32 i = 0; i < module.entry_count; ++i) {
			const AotEntry &entry = image.ram_entries[module.first_entry + i];
			const uae_u16 patch = image.patch_opcodes[image.entry_count + module.first_entry + i];
			ram[base + entry.offset] = (uae_u8)(patch >> 8);
			ram[base + entry.offset + 1] = (uae_u8)patch;
		}
	}

	// Did the run store into the module's own code?
	bool code_changed(const uae_u8 *ram, uae_u32 m, uae_u32 base)
	{
		const AotImage &image = aot_image;
		const AotModule &module = image.modules[m];
		for (uae_u32 i = 0; i < module.range_count; ++i) {
			const AotRange &range = image.ram_ranges[module.first_range + i];
			if (memcmp(ram + base + range.offset, module_code(m) + range.offset, range.length) != 0)
				return true;
		}
		for (uae_u32 i = 0; i < module.entry_count; ++i) {
			const AotEntry &entry = image.ram_entries[module.first_entry + i];
			const uae_u16 patch = image.patch_opcodes[image.entry_count + module.first_entry + i];
			const uae_u32 word = ((uae_u32)ram[base + entry.offset] << 8) | ram[base + entry.offset + 1];
			if (word != patch && word != entry.original)
				return true;
		}
		return false;
	}

	// The interpreter on memory that holds entry opcodes: it is handed the
	// instruction each one replaced, so both runs read the same memory.
	void run_interpreter(uae_u32 instructions, const CpuState &initial)
	{
		const AotImage &image = aot_image;
		map(ram_reference, rom_patched, &reference.journal);
		apply(initial);
		for (uae_u32 i = 0; i < instructions; ++i) {
			uae_u32 opcode = do_get_mem_word_unswapped(regs.pc_p);
			const uae_u32 word = ((opcode & 0xff) << 8) | (opcode >> 8);
			if ((uae_u32)(regs.pc_p - RAMBaseHost) < RAMSize) {
				// In RAM an entry opcode stands for the instruction it
				// replaced, wherever it came from.
				for (uae_u32 e = 0; e < image.entry_count + image.ram_entry_count; ++e) {
					if (image.patch_opcodes[e] == word) {
						const uae_u32 original = e < image.entry_count
							? image.entries[e].original
							: image.ram_entries[e - image.entry_count].original;
						opcode = ((original & 0xff) << 8) | (original >> 8);
						break;
					}
				}
			}
			if (options.trace)
				write_log("   step %u pc=%08x opcode=%04x\n", (unsigned)i, (unsigned)m68k_getpc(),
				          (unsigned)(((opcode & 0xff) << 8) | (opcode >> 8)));
			(*cpufunctbl[opcode])(opcode);
		}
		reference.instructions = instructions;
		capture(&reference.state);
	}

	// Every module entry, from code placed at a different address each time:
	// entered cold (the entry finds and installs the module), through the
	// opcode table once installed, and after the guest flushed its cache.
	void modules()
	{
		const AotImage &image = aot_image;
		if (image.module_count == 0 || image.module_code == NULL)
			return;
		for (uae_u32 m = 0; m < image.module_count; ++m) {
			const AotModule &module = image.modules[m];
			if (module.span + 0x6000 > options.ram_size) {
				result.skipped += module.entry_count * options.trials;
				continue;
			}
			for (uae_u32 i = 0; i < module.entry_count; ++i) {
				const uae_u32 e = module.first_entry + i;
				if (options.only_entry >= 0 && (uae_u32)options.only_entry != image.entry_count + e)
					continue;
				const AotEntry &entry = image.ram_entries[e];
				for (uae_u32 trial = 0; trial < options.trials; ++trial) {
					const uae_u32 room = options.ram_size - 0x5000 - module.span;
					const uae_u32 base = (0x2800 + rng.below(room)) & ~1u;
					const int mode = trial % 3;
					CpuState initial;
					random_state(rng, base + entry.offset, options.ram_size, &initial, base, base + module.span);

					memcpy(ram_translated, ram_master, options.ram_size + kPadding);
					memcpy(ram_translated + base, module_code(m), module.span);
					memcpy(ram_reference, ram_translated, options.ram_size + kPadding);
					write_entry_opcodes(ram_reference, m, base);

					map(ram_translated, rom_patched, &translated.journal);
					bool entered = true;
					if (mode == 0) {
						AotRamForgetAll();
					} else {
						entered = AotRamInstallAt(m, base);
						if (mode == 2) {
							if ((result.runs & 1u) != 0)
								AotCodeChangedGuestRange(base, 1);
							else
								AotCodeChanged();
						}
					}
					apply(initial);
					aot_extra_instructions = 0;
					const uae_u32 opcode = do_get_mem_word_unswapped(regs.pc_p);
					if (mode == 0)
						entry.handler(handler_opcode(image.entry_count + e));
					else
						(*cpufunctbl[opcode])(opcode);
					translated.instructions = 1 + aot_extra_instructions;
					aot_extra_instructions = 0;
					capture(&translated.state);
					if (translated.instructions > kMaxReference) {
						result.skipped++;
						continue;
					}
					run_interpreter(translated.instructions, initial);
					// Code that stores into itself without flushing the
					// cache is outside what translated code promises.
					if (code_changed(ram_reference, m, base)) {
						result.skipped++;
						continue;
					}
					result.runs++;
					result.instructions += translated.instructions;
					if (!entered || image.module_state[m].base != base ||
					    !compare_at("module", image.entry_count + e, entry.offset, trial, initial))
						result.failures++;
				}

				// Code that changed and was flushed must not run translated:
				// the entry runs the original instruction and puts it back.
				const uae_u32 base = 0x3000;
				CpuState initial;
				random_state(rng, base + entry.offset, options.ram_size, &initial, base, base + module.span);
				memcpy(ram_translated, ram_master, options.ram_size + kPadding);
				memcpy(ram_translated + base, module_code(m), module.span);
				map(ram_translated, rom_patched, &translated.journal);
				const bool installed = AotRamInstallAt(m, base);
				// Change a translated byte that is not an entry opcode.
				const AotRange &range = image.ram_ranges[module.first_range];
				ram_translated[base + range.offset + range.length - 1] ^= 0x04;
				memcpy(ram_reference, ram_translated, options.ram_size + kPadding);
				const bool selective = (result.runs & 1u) != 0;
				const uae_u32 epoch_before = aot_flush_epoch;
				if (selective) {
					// Flushing an unrelated 4 KiB page must not bump epoch or invalidate module m.
					const uae_u32 far_page = (base + module.span + 0x1fffu) & ~0xfffu;
					const uae_u32 far_addr = (far_page < options.ram_size) ? far_page : 0u;
					AotCodeChangedGuestRange(far_addr, 1);
					if (aot_flush_epoch != epoch_before || image.module_state[m].epoch == 0)
						result.failures++;
					// Flushing the modified byte invalidates module m selectively without a global flush.
					AotCodeChangedGuestRange(base + range.offset + range.length - 1, 1);
					if (aot_flush_epoch != epoch_before || image.module_state[m].epoch != 0)
						result.failures++;
				} else {
					AotCodeChanged();
				}
				apply(initial);
				aot_extra_instructions = 0;
				const uae_u32 opcode = do_get_mem_word_unswapped(regs.pc_p);
				(*cpufunctbl[opcode])(opcode);
				translated.instructions = 1 + aot_extra_instructions;
				aot_extra_instructions = 0;
				capture(&translated.state);
				// The entry put the instruction back before running it, so
				// that is what the interpreter starts from.
				ram_reference[base + entry.offset] = (uae_u8)(entry.original >> 8);
				ram_reference[base + entry.offset + 1] = (uae_u8)entry.original;
				run_interpreter(1, initial);
				result.runs++;
				if (!installed || translated.instructions != 1 ||
				    image.module_state[m].base != AOT_NO_BASE ||
				    !compare_at("changed-module", image.entry_count + e, entry.offset, 0, initial))
					result.failures++;
			}
		}
		AotRamForgetAll();
	}

	// An entry opcode that the guest copied out of the ROM must behave as
	// the instruction it replaced.
	void relocated()
	{
		const AotImage &image = aot_image;
		const uae_u32 length = 16;
		for (uae_u32 e = 0; e < image.entry_count && options.only_entry < 0; ++e) {
			const AotEntry &entry = image.entries[e];
			const uae_u32 address = 0x8000 + 2 * (e & 0xfff);
			CpuState initial;
			random_state(rng, address, options.ram_size, &initial, address, address + length);

			// Both runs execute from the same memory: the instruction
			// with the entry opcode in place of its first word.
			memcpy(ram_translated, ram_master, options.ram_size + kPadding);
			memcpy(ram_translated + address, rom_pristine + entry.offset, length);
			memcpy(ram_translated + address, rom_patched + entry.offset, 2);
			memcpy(ram_reference, ram_translated, options.ram_size + kPadding);

			map(ram_translated, rom_patched, &translated.journal);
			apply(initial);
			aot_extra_instructions = 0;
			const uae_u32 opcode = do_get_mem_word_unswapped(regs.pc_p);
			(*cpufunctbl[opcode])(opcode);
			translated.instructions = 1 + aot_extra_instructions;
			aot_extra_instructions = 0;
			capture(&translated.state);

			run_interpreter(1, initial);
			result.runs++;
			if (translated.instructions != 1 || !compare("relocated", e, 0, initial))
				result.failures++;
		}
	}
};

}  // namespace

bool AotSelfTest(const AotSelfTestOptions &options, const uae_u8 *pristine_rom, AotSelfTestResult *result)
{
	const AotImage &image = aot_image;
	Tester *tester = (Tester *)calloc(1, sizeof(Tester));
	const uae_u32 ram_bytes = options.ram_size + kPadding;
	uae_u8 *ram = (uae_u8 *)TEST_ALLOC(3 * ram_bytes);
	addrbank **banks = (addrbank **)TEST_ALLOC(65536 * sizeof(addrbank *));
	OutsideWrite *writes = (OutsideWrite *)TEST_ALLOC(2 * kMaxJournal * sizeof(OutsideWrite));
	wild_page = (uae_u8 *)TEST_ALLOC(0x10000 + kPadding);
	uae_u8 *frames = (uae_u8 *)TEST_ALLOC(3 * (kFrameSize + kPadding));
	memset(result, 0, sizeof(*result));
	if (!tester || !ram || !banks || !writes || !wild_page || !frames ||
	    image.entry_count + image.ram_entry_count == 0) {
		write_log("[AOT TEST] cannot run: no memory or no translated image\n");
		result->failures = 1;
		return false;
	}
	memset(wild_page, 0, 0x10000 + kPadding);

	tester->options = options;
	tester->rom_pristine = pristine_rom;
	tester->rom_patched = ROMBaseHost;
	tester->ram_master = ram;
	tester->ram_reference = ram + ram_bytes;
	tester->ram_translated = ram + 2 * ram_bytes;
	tester->reference.journal.writes = writes;
	tester->translated.journal.writes = writes + kMaxJournal;
	tester->rng.state = 0x9E3779B97F4A7C15ull ^ ((uae_u64)options.seed << 32) ^ options.seed;
	for (uae_u32 i = 0; i < ram_bytes; ++i)
		ram[i] = (uae_u8)tester->rng.next();
	tester->frame_master = frames;
	tester->frame_reference = frames + kFrameSize + kPadding;
	tester->frame_translated = frames + 2 * (kFrameSize + kPadding);
	for (uae_u32 i = 0; i < kFrameSize + kPadding; ++i)
		frames[i] = (uae_u8)tester->rng.next();
	// Exception vectors that lead somewhere inside test RAM.
	for (uae_u32 vector = 0; vector < 0x400; vector += 4) {
		const uae_u32 handler = 0x1000 + vector;
		ram[vector] = (uae_u8)(handler >> 24);
		ram[vector + 1] = (uae_u8)(handler >> 16);
		ram[vector + 2] = (uae_u8)(handler >> 8);
		ram[vector + 3] = (uae_u8)handler;
	}
	for (int i = 0; i < 65536; ++i)
		banks[i] = &wild_bank;

	// Replace the guest's memory map with the test's.
	uae_u8 *const saved_ram = RAMBaseHost;
	const uae_u32 saved_ram_size = RAMSize;
	uae_u8 *const saved_rom = ROMBaseHost;
	addrbank **const saved_banks = mem_banks;
	const int saved_layout = MacFrameLayout;
	uae_u8 *const saved_frame = MacFrameBaseHost;
	const uae_u32 saved_frame_size = MacFrameSize;
	const regstruct saved_regs = regs;
	const flag_struct saved_flags = regflags;
	RAMSize = options.ram_size;
	mem_banks = banks;
	// A direct-layout frame buffer, so that the frame-buffer paths of the
	// memory helpers are compared too.
	MacFrameLayout = FLAYOUT_DIRECT;
	MacFrameSize = kFrameSize;
	MacFrameBaseHost = tester->frame_translated;

	for (uae_u32 e = 0; e < image.entry_count; ++e) {
		if (AotOriginalWord(image.rom_base + image.entries[e].offset) != image.entries[e].original) {
			write_log("[AOT TEST] AotOriginalWord is wrong at ROM+%x\n", (unsigned)image.entries[e].offset);
			tester->result.failures++;
		}
	}
	tester->entries(0);
	write_log("[AOT TEST] direct: %u runs, %u failures so far\n", (unsigned)tester->result.runs,
	          (unsigned)tester->result.failures);
	tester->entries(1);
	write_log("[AOT TEST] installed: %u runs, %u failures so far\n", (unsigned)tester->result.runs,
	          (unsigned)tester->result.failures);
	tester->relocated();
	write_log("[AOT TEST] ROM done: %u runs, %u failures so far\n", (unsigned)tester->result.runs,
	          (unsigned)tester->result.failures);
	tester->modules();

	RAMBaseHost = saved_ram;
	RAMSize = saved_ram_size;
	ROMBaseHost = saved_rom;
	mem_banks = saved_banks;
	MacFrameLayout = saved_layout;
	MacFrameBaseHost = saved_frame;
	MacFrameSize = saved_frame_size;
	regs = saved_regs;
	regflags = saved_flags;
	aot_extra_instructions = 0;

	*result = tester->result;
	write_log("[AOT TEST] %s: %u runs, %llu instructions compared, %u failures, %u skipped\n",
	          result->failures ? "FAIL" : "PASS", (unsigned)result->runs,
	          (unsigned long long)result->instructions, (unsigned)result->failures, (unsigned)result->skipped);
	free(frames);
	free(wild_page);
	free(writes);
	free(banks);
	free(ram);
	free(tester);
	wild_page = NULL;
	return result->failures == 0;
}

#endif  // AOT_SELFTEST || AOT_HOST_TEST
