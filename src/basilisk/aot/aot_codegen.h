/*
 *  aot_codegen.h - Definitions for generated region functions
 *
 *  Included only by aot_rom_generated.cpp, after AOT_ROM_BASE is defined.
 *  The instruction bodies in that file are the interpreter's handler bodies,
 *  so this header redirects what they touch:
 *
 *   - Condition codes live in locals for the length of a region call. The
 *     compiler can then discard flag results that the following 68k code
 *     overwrites, which the interpreter has to compute and store every time.
 *   - The emulated PC is not maintained inside a region. Leaving a region
 *     sets it; PC-relative operands were folded to constants by the
 *     translator. The generated file defines AOT_PC(address) and
 *     AOT_EXIT_REL(address) for the code that follows: plain constants for
 *     the ROM, relative to the module's base for RAM.
 *   - Guest registers are accessed exactly as the interpreter accesses them.
 *     Guest memory is too, through out-of-line helpers.
 */

#ifndef AOT_CODEGEN_H
#define AOT_CODEGEN_H

#ifndef AOT_GENERATED_CODE
#error "aot_codegen.h is for generated region code only"
#endif

#include <string.h>

#define SET_CFLG_ALWAYS(x) SET_CFLG(x)
#define SET_NFLG_ALWAYS(x) SET_NFLG(x)

#if defined(__riscv) && !defined(MEM_SLOW_DIRECT_CALLS)
static ALWAYS_INLINE void aot_setpc(uaecptr address)
{
	register uae_u32 aot_a0 __asm__("a0") = address;
	__asm__ volatile("lui t0, %%hi(aot_setpc_thunk)\n\tjalr t0, %%lo(aot_setpc_thunk)(t0)"
	                 : "+r"(aot_a0) : : "t0", "t1", "t2", "memory", MEM_SLOW_FP_CLOBBERS);
}
#else
static ALWAYS_INLINE void aot_setpc(uaecptr address) { m68k_setpc(address); }
#endif

#undef CFLG
#undef ZFLG
#undef NFLG
#undef VFLG
#undef XFLG
#undef SET_CFLG
#undef SET_ZFLG
#undef SET_NFLG
#undef SET_VFLG
#undef SET_XFLG
#undef GET_CFLG
#undef GET_ZFLG
#undef GET_NFLG
#undef GET_VFLG
#undef GET_XFLG
#undef COPY_CARRY
#undef CLEAR_CZNV
#undef SET_CZNV_ZN
#undef SET_CZNV_ALL
#define CFLG aot_c
#define ZFLG aot_z
#define NFLG aot_n
#define VFLG aot_v
#define XFLG aot_x
#define SET_CFLG(x) (aot_c = (x))
#define SET_ZFLG(x) (aot_z = (x))
#define SET_NFLG(x) (aot_n = (x))
#define SET_VFLG(x) (aot_v = (x))
#define SET_XFLG(x) (aot_x = (x))
#define GET_CFLG aot_c
#define GET_ZFLG aot_z
#define GET_NFLG aot_n
#define GET_VFLG aot_v
#define GET_XFLG aot_x
#define COPY_CARRY (aot_x = aot_c)
#define CLEAR_CZNV do { aot_c = 0; aot_z = 0; aot_n = 0; aot_v = 0; } while (0)
#define SET_CZNV_ZN(zval, nval) do { \
	const unsigned int aot_zv_ = (unsigned int)(zval); \
	const unsigned int aot_nv_ = (unsigned int)(nval); \
	aot_c = 0; aot_z = aot_zv_; aot_n = aot_nv_; aot_v = 0; \
} while (0)
#define SET_CZNV_ALL(cval, zval, nval, vval) do { \
	const unsigned int aot_cv_ = (unsigned int)(cval); \
	const unsigned int aot_zv_ = (unsigned int)(zval); \
	const unsigned int aot_nv_ = (unsigned int)(nval); \
	const unsigned int aot_vv_ = (unsigned int)(vval); \
	aot_c = aot_cv_; aot_z = aot_zv_; aot_n = aot_nv_; aot_v = aot_vv_; \
} while (0)

// Same expressions as cctrue() in m68k.h, on the local flags.
static ALWAYS_INLINE int aot_cctrue(const int cc, unsigned int c, unsigned int z,
                                    unsigned int n, unsigned int v)
{
	switch (cc) {
	 case 0: return 1;
	 case 1: return 0;
	 case 2: return !c && !z;
	 case 3: return c || z;
	 case 4: return !c;
	 case 5: return c;
	 case 6: return !z;
	 case 7: return z;
	 case 8: return !v;
	 case 9: return v;
	 case 10:return !n;
	 case 11:return n;
	 case 12:return n == v;
	 case 13:return n != v;
	 case 14:return !z && (n == v);
	 case 15:return z || (n != v);
	}
	return 0;
}
#define cctrue(cc) aot_cctrue((cc), aot_c, aot_z, aot_n, aot_v)

// Batch cznv load/store when flag_struct packs c,z,n,v into a 32-bit cznv
// word, with clean fallback for unpacked flag_struct.
template <typename...> struct aot_void { using type = void; };
template <typename T, typename = void> struct aot_has_cznv { static constexpr bool value = false; };
template <typename T> struct aot_has_cznv<T, typename aot_void<decltype(((T *)0)->cznv)>::type> {
	static constexpr bool value = true;
};
template <typename F>
static ALWAYS_INLINE void aot_store_cznv(F &rf, unsigned int c, unsigned int z, unsigned int n, unsigned int v)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	if constexpr (aot_has_cznv<F>::value) {
		rf.cznv = ((uae_u32)(uae_u8)(c)) | (((uae_u32)(uae_u8)(z)) << 8) |
		          (((uae_u32)(uae_u8)(n)) << 16) | (((uae_u32)(uae_u8)(v)) << 24);
		return;
	}
#endif
	rf.c = c; rf.z = z; rf.n = n; rf.v = v;
}
template <typename F>
static ALWAYS_INLINE void aot_load_cznv(const F &rf, unsigned int &c, unsigned int &z, unsigned int &n, unsigned int &v)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	if constexpr (aot_has_cznv<F>::value) {
		const uae_u32 cznv = rf.cznv;
		c = cznv & 1u;
		z = (cznv >> 8) & 1u;
		n = (cznv >> 16) & 1u;
		v = (cznv >> 24) & 1u;
		return;
	}
#endif
	c = rf.c; z = rf.z; n = rf.n; v = rf.v;
}

#define AOT_FLAGS_OUT() do { \
	aot_store_cznv(regflags, aot_c, aot_z, aot_n, aot_v); \
	regflags.x = aot_x; \
} while (0)
#define AOT_FLAGS_IN() do { \
	aot_load_cznv(regflags, aot_c, aot_z, aot_n, aot_v); \
	aot_x = regflags.x; \
} while (0)
#define AOT_MULL_FLAGS_IN() aot_load_cznv(regflags, aot_c, aot_z, aot_n, aot_v)

// Guest memory is reached through the out-of-line helpers in memory.cpp.
// Translated code runs behind a 16 KB instruction cache, where the inline
// sequences the interpreter's handlers use cost more in fetch stalls than
// the call does. The helpers are called at their absolute address: the code
// may run from a copy in PSRAM (AOT_PSRAM), so it must not reach anything
// outside itself relative to the PC. tools/aot/check_blob.py enforces that.
#define AOT_CAT2(a, b) a##b
#define AOT_CAT(a, b) AOT_CAT2(a, b)

#if defined(__riscv) && !defined(MEM_SLOW_DIRECT_CALLS)
extern "C" {
void mem_slow_lget_thunk(void);
void mem_slow_wget_thunk(void);
void mem_slow_bget_thunk(void);
void mem_slow_lput_thunk(void);
void mem_slow_wput_thunk(void);
void mem_slow_bput_thunk(void);
}
#define AOT_MEM_GET(thunk, addr) ({ \
	register uae_u32 aot_a0 __asm__("a0") = (addr); \
	__asm__ volatile("lui t0, %%hi(" #thunk ")\n\tjalr t0, %%lo(" #thunk ")(t0)" \
	                 : "+r"(aot_a0) : : "t0", "t1", "t2", "t3", "memory", MEM_SLOW_FP_CLOBBERS); \
	aot_a0; })
#define AOT_MEM_PUT(thunk, addr, value) do { \
	register uae_u32 aot_a0 __asm__("a0") = (addr); \
	register uae_u32 aot_a1 __asm__("a1") = (value); \
	__asm__ volatile("lui t0, %%hi(" #thunk ")\n\tjalr t0, %%lo(" #thunk ")(t0)" \
	                 : "+r"(aot_a0), "+r"(aot_a1) : : "t0", "t1", "t2", "t3", "memory", MEM_SLOW_FP_CLOBBERS); \
} while (0)
#define AOT_MEM_SLOW_GET(thunk, addr) ({ \
	register uae_u32 aot_a0 __asm__("a0") = (addr); \
	__asm__ volatile("lui t0, %%hi(" #thunk ")\n\tjalr t0, %%lo(" #thunk ")(t0)" \
	                 : "+r"(aot_a0) : : "t0", "memory", MEM_SLOW_FP_CLOBBERS); \
	aot_a0; })
#define AOT_MEM_SLOW_PUT(thunk, addr, value) do { \
	register uae_u32 aot_a0 __asm__("a0") = (addr); \
	register uae_u32 aot_a1 __asm__("a1") = (value); \
	__asm__ volatile("lui t0, %%hi(" #thunk ")\n\tjalr t0, %%lo(" #thunk ")(t0)" \
	                 : : "r"(aot_a0), "r"(aot_a1) : "t0", "memory", MEM_SLOW_FP_CLOBBERS); \
} while (0)

static ALWAYS_INLINE uae_u32 aot_get_long_0(uaecptr a) { return AOT_MEM_GET(mem_lget_thunk, a); }
static ALWAYS_INLINE uae_u32 aot_get_word_0(uaecptr a) { return AOT_MEM_GET(mem_wget_thunk, a); }
static ALWAYS_INLINE uae_u32 aot_get_byte_0(uaecptr a) { return AOT_MEM_GET(mem_bget_thunk, a); }
static ALWAYS_INLINE void aot_put_long_0(uaecptr a, uae_u32 v) { AOT_MEM_PUT(mem_lput_thunk, a, v); }
static ALWAYS_INLINE void aot_put_word_0(uaecptr a, uae_u32 v) { AOT_MEM_PUT(mem_wput_thunk, a, v); }
static ALWAYS_INLINE void aot_put_byte_0(uaecptr a, uae_u32 v) { AOT_MEM_PUT(mem_bput_thunk, a, v); }

// Inline RAM fast-path with absolute slow-thunk fallback (safe for PSRAM blob).
static ALWAYS_INLINE uae_u32 aot_get_long_1(uaecptr a) {
	if (likely(a < mem_ram_size())) return do_get_mem_long((uae_u32 *)(mem_ram_base() + a));
	return AOT_MEM_SLOW_GET(mem_slow_lget_thunk, a);
}
static ALWAYS_INLINE uae_u32 aot_get_word_1(uaecptr a) {
	if (likely(a < mem_ram_size())) return do_get_mem_word((uae_u16 *)(mem_ram_base() + a));
	return AOT_MEM_SLOW_GET(mem_slow_wget_thunk, a);
}
static ALWAYS_INLINE uae_u32 aot_get_byte_1(uaecptr a) {
	if (likely(a < mem_ram_size())) return *(uae_u8 *)(mem_ram_base() + a);
	return AOT_MEM_SLOW_GET(mem_slow_bget_thunk, a);
}
static ALWAYS_INLINE void aot_put_long_1(uaecptr a, uae_u32 v) {
	if (likely(a < mem_ram_size())) { do_put_mem_long((uae_u32 *)(mem_ram_base() + a), v); return; }
	AOT_MEM_SLOW_PUT(mem_slow_lput_thunk, a, v);
}
static ALWAYS_INLINE void aot_put_word_1(uaecptr a, uae_u32 v) {
	if (likely(a < mem_ram_size())) { do_put_mem_word((uae_u16 *)(mem_ram_base() + a), v); return; }
	AOT_MEM_SLOW_PUT(mem_slow_wput_thunk, a, v);
}
static ALWAYS_INLINE void aot_put_byte_1(uaecptr a, uae_u32 v) {
	if (likely(a < mem_ram_size())) { *(uae_u8 *)(mem_ram_base() + a) = v; return; }
	AOT_MEM_SLOW_PUT(mem_slow_bput_thunk, a, v);
}

#ifdef AOT_INLINE_MEMORY
#define AOT_INLINE_MEM_FLAG 1
#else
#define AOT_INLINE_MEM_FLAG 0
#endif
#define get_long(a) AOT_CAT(aot_get_long_, AOT_INLINE_MEM_FLAG)(a)
#define get_word(a) AOT_CAT(aot_get_word_, AOT_INLINE_MEM_FLAG)(a)
#define get_byte(a) AOT_CAT(aot_get_byte_, AOT_INLINE_MEM_FLAG)(a)
#define put_long(a, v) AOT_CAT(aot_put_long_, AOT_INLINE_MEM_FLAG)((a), (v))
#define put_word(a, v) AOT_CAT(aot_put_word_, AOT_INLINE_MEM_FLAG)((a), (v))
#define put_byte(a, v) AOT_CAT(aot_put_byte_, AOT_INLINE_MEM_FLAG)((a), (v))
#else
#define AOT_INLINE_MEM_FLAG 0
#endif

// Guest registers. The generated file defines AOT_REGS for each region:
// the interpreter's registers, or a local copy in regions that run long
// enough per entry to repay loading and storing it (AOT_REGS_IN/_OUT).
#undef m68k_dreg
#undef m68k_areg
#undef m68k_dreg_w_set
#undef m68k_dreg_b_set
#define m68k_dreg(r, num) (AOT_REGS[(num)])
#define m68k_areg(r, num) (AOT_REGS[(num) + 8])

#define AOT_REGS_IS_LOCAL 0
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
static ALWAYS_INLINE void aot_dreg_w_set_0(uae_u32 *r, uae_u32 num, uae_u32 val)
{
	const uae_u16 v = (uae_u16)val;
	memcpy((uae_u8 *)&r[num], &v, sizeof(v));
}
static ALWAYS_INLINE void aot_dreg_b_set_0(uae_u32 *r, uae_u32 num, uae_u32 val)
{
	const uae_u8 v = (uae_u8)val;
	memcpy((uae_u8 *)&r[num], &v, sizeof(v));
}
#else
static ALWAYS_INLINE void aot_dreg_w_set_0(uae_u32 *r, uae_u32 num, uae_u32 val)
{
	r[num] = (r[num] & ~0xffffu) | (val & 0xffffu);
}
static ALWAYS_INLINE void aot_dreg_b_set_0(uae_u32 *r, uae_u32 num, uae_u32 val)
{
	r[num] = (r[num] & ~0xffu) | (val & 0xffu);
}
#endif
static ALWAYS_INLINE void aot_dreg_w_set_1(uae_u32 *r, uae_u32 num, uae_u32 val)
{
	r[num] = (r[num] & ~0xffffu) | (val & 0xffffu);
}
static ALWAYS_INLINE void aot_dreg_b_set_1(uae_u32 *r, uae_u32 num, uae_u32 val)
{
	r[num] = (r[num] & ~0xffu) | (val & 0xffu);
}
#define m68k_dreg_w_set(r, num, val) AOT_CAT(aot_dreg_w_set_, AOT_REGS_IS_LOCAL)(AOT_REGS, (num), (val))
#define m68k_dreg_b_set(r, num, val) AOT_CAT(aot_dreg_b_set_, AOT_REGS_IS_LOCAL)(AOT_REGS, (num), (val))

// get_disp_ea_020() of newcpu.h on the region's registers.
static ALWAYS_INLINE uae_u32 aot_disp_ea_020(const uae_u32 *r, uae_u32 base, uae_u32 dp)
{
	uae_s32 regd = r[(dp >> 12) & 15];
	if ((dp & 0x800) == 0)
		regd = (uae_s32)(uae_s16)regd;
	regd <<= (dp >> 9) & 3;
	return base + (uae_s32)((uae_s8)dp) + regd;
}

// get_disp_ea_020_full() with the displacements it would read through the
// PC passed in. The translator calls this form for full extension words.
static ALWAYS_INLINE uae_u32 aot_disp_ea_020(const uae_u32 *r, uae_u32 base, uae_u32 dp, uae_s32 bd, uae_s32 od)
{
	uae_s32 regd = r[(dp >> 12) & 15];
	if ((dp & 0x800) == 0)
		regd = (uae_s32)(uae_s16)regd;
	regd <<= (dp >> 9) & 3;
	if (dp & 0x80) base = 0;
	if (dp & 0x40) regd = 0;
	base += bd;
	if ((dp & 0x4) == 0) base += regd;
	if (dp & 0x3) base = get_long(base);
	if (dp & 0x4) base += regd;
	return base + od;
}
#define get_disp_ea_020(...) aot_disp_ea_020(AOT_REGS, __VA_ARGS__)

// The entry number from an entry opcode in the form handlers are called
// with. A region's entry opcodes are consecutive and share their high byte;
// `first_opcode` is the first of them as the guest sees it.
#ifdef HAVE_GET_WORD_UNSWAPPED
#define AOT_ENTRY_NUMBER(opcode, first_opcode) (((opcode) >> 8) - ((first_opcode) & 0xff))
#define AOT_SWAPPED_OPCODE(op) ((((op) & 0xffu) << 8) | (((op) >> 8) & 0xffu))
#else
#define AOT_ENTRY_NUMBER(opcode, first_opcode) (((opcode) & 0xff) - ((first_opcode) & 0xff))
#define AOT_SWAPPED_OPCODE(op) (op)
#endif

#define AOT_LOCALS \
	unsigned int aot_c, aot_z, aot_n, aot_v, aot_x; \
	uae_u32 aot_icount = 0; \
	uae_u32 aot_target

#define AOT_RUN_ORIGINAL(info) do { \
	(*cpufunctbl[(info).original])((info).original); \
	return; \
} while (0)

#define AOT_RUN_ORIGINAL_IMM(original_imm) do { \
	const uae_u32 aot_orig = (original_imm); \
	(*cpufunctbl[aot_orig])(aot_orig); \
	return; \
} while (0)

// Single-entry regions keep offset and original opcode as immediate constants:
// zero entry-table memory traffic and zero flash reads.
#define AOT_ROM_ENTER_1(offset_imm, original_imm) \
	AOT_LOCALS; \
	(void)opcode; \
	if (unlikely(regs.pc_p != ROMBaseHost + (offset_imm) || \
	             (regs.spcflags & (SPCFLAG_TRACE | SPCFLAG_DOTRACE)))) \
		AOT_RUN_ORIGINAL_IMM(original_imm); \
	AOT_FLAGS_IN()

#define AOT_RAM_ENTER_1(module, offset_imm, original_imm) \
	AOT_LOCALS; \
	(void)opcode; \
	AotModuleState *const aot_state = &aot_module_state[module]; \
	uae_u32 aot_found = aot_state->base; \
	if (unlikely(regs.pc_p != RAMBaseHost + aot_found + (offset_imm) || \
	             aot_state->epoch != aot_flush_epoch || \
	             (regs.spcflags & (SPCFLAG_TRACE | SPCFLAG_DOTRACE)))) { \
		aot_found = aot_far.ram_entry_slow((module), (offset_imm)); \
		if (aot_found == AOT_NO_BASE) \
			AOT_RUN_ORIGINAL_IMM(original_imm); \
	} \
	const uae_u32 aot_base = aot_found; \
	(void)aot_base; \
	AOT_FLAGS_IN()

// Multi-entry region functions index the PSRAM copy of AotEntryInfo.
#define AOT_ROM_ENTER(first_opcode, table_base) \
	AOT_LOCALS; \
	const uae_u32 aot_entry = AOT_ENTRY_NUMBER(opcode, first_opcode); \
	do { \
		const AotEntryInfo &aot_info = aot_entry_infos_runtime[(table_base) + aot_entry]; \
		if (unlikely(regs.pc_p != ROMBaseHost + aot_info.offset || \
		             (regs.spcflags & (SPCFLAG_TRACE | SPCFLAG_DOTRACE)))) \
			AOT_RUN_ORIGINAL(aot_info); \
	} while (0); \
	AOT_FLAGS_IN()

#define AOT_RAM_ENTER(module, first_opcode, table_base) \
	AOT_LOCALS; \
	const uae_u32 aot_entry = AOT_ENTRY_NUMBER(opcode, first_opcode); \
	AotModuleState *const aot_state = &aot_module_state[module]; \
	uae_u32 aot_found = aot_state->base; \
	do { \
		const AotEntryInfo &aot_info = aot_entry_infos_runtime[(table_base) + aot_entry]; \
		if (unlikely(regs.pc_p != RAMBaseHost + aot_found + aot_info.offset || \
		             aot_state->epoch != aot_flush_epoch || \
		             (regs.spcflags & (SPCFLAG_TRACE | SPCFLAG_DOTRACE)))) { \
			aot_found = aot_far.ram_entry_slow((module), aot_info.offset); \
			if (aot_found == AOT_NO_BASE) \
				AOT_RUN_ORIGINAL(aot_info); \
		} \
	} while (0); \
	const uae_u32 aot_base = aot_found; \
	(void)aot_base; \
	AOT_FLAGS_IN()

// Every way out of a region ends here. The exits set aot_target and jump.
#define AOT_EPILOGUE \
	aot_exit_rom: { \
		uae_u8 *aot_host = ROMBaseHost + aot_target; \
		regs.pc_p = aot_host; \
		regs.pc_oldp = aot_host; \
		regs.pc = AOT_ROM_BASE + aot_target; \
		goto aot_exit; \
	} \
	aot_exit_ram: { \
		uae_u8 *aot_host = RAMBaseHost + aot_target; \
		regs.pc_p = aot_host; \
		regs.pc_oldp = aot_host; \
		regs.pc = aot_target; \
		goto aot_exit; \
	} \
	aot_exit_dyn: \
		aot_setpc(aot_target); \
	aot_exit: \
		AOT_REGS_OUT(); \
		AOT_FLAGS_OUT(); \
		aot_extra_instructions += (int32)aot_icount - 1; \
		aot_region_calls++

#define AOT_EXIT_ROM(off) do { aot_target = (off); goto aot_exit_rom; } while (0)
// A RAM address the translated code itself lies at.
#define AOT_EXIT_RAM(address) do { aot_target = (address); goto aot_exit_ram; } while (0)
#define AOT_EXIT_DYN(address) do { aot_target = (address); goto aot_exit_dyn; } while (0)

// A region must hand control back for pending interrupts and must not run
// unbounded, so every backward branch polls. The special flags are written
// by other tasks; the volatile read keeps the poll inside guest loops that
// make no store.
#define AOT_POLL() \
	((*(volatile spcflags_t *)&regs.spcflags) | (aot_icount >> 16))
#define AOT_CHAIN_POLL() \
	((*(volatile spcflags_t *)&regs.spcflags) | (((uae_u32)aot_extra_instructions + aot_icount) >> 14))
#define AOT_BACK(label, address) do { \
	if (unlikely(AOT_POLL())) AOT_EXIT_REL(address); \
	goto label; \
} while (0)

#define AOT_PUSH_RETURN(address) do { \
	const uaecptr aot_sp = m68k_areg(regs, 7) - 4; \
	m68k_areg(regs, 7) = aot_sp; \
	put_long(aot_sp, (address)); \
} while (0)

#define AOT_RTS() do { \
	const uaecptr aot_sp = m68k_areg(regs, 7); \
	const uaecptr aot_return = get_long(aot_sp); \
	m68k_areg(regs, 7) = aot_sp + 4; \
	AOT_EXIT_DYN(aot_return); \
} while (0)

// Handler bodies for JMP, JSR and RTD end in these.
#undef m68k_setpc_rte
#define m68k_setpc(address) AOT_EXIT_DYN(address)
#define m68k_setpc_rte(address) AOT_EXIT_DYN(address)
#define m68k_do_jsr(oldpc, dest) do { \
	const uaecptr aot_dest = (dest); \
	AOT_PUSH_RETURN(oldpc); \
	AOT_EXIT_DYN(aot_dest); \
} while (0)

// m68k_mull() is ordinary interpreter code working on the global flags.
#define AOT_MULL(opcode, dst, extra) do { \
	const uae_u32 aot_operand = (dst); \
	AOT_REGS_OUT(); \
	AOT_FLAGS_OUT(); \
	aot_far.mull((opcode), aot_operand, (extra)); \
	AOT_MULL_FLAGS_IN(); \
	AOT_REGS_IN(); \
} while (0)

// Exception() builds its stack frame from the global flags and the emulated
// PC, and leaves the PC at the exception handler. `remaining` is the number
// of instructions of the current block that follow the faulting one; they
// were counted on entry to the block and do not run.
#define AOT_EXCEPTION(nr, oldpc, pc, remaining) do { \
	AOT_REGS_OUT(); \
	AOT_FLAGS_OUT(); \
	aot_setpc(pc); \
	aot_far.exception((nr), (oldpc)); \
	aot_extra_instructions += (int32)aot_icount - 1 - (remaining); \
	aot_region_calls++; \
	return; \
} while (0)

// Optional inline A-line Toolbox trap (0xA800..0xAFFF) fast path when the
// Line-A vector holds the native Mac OS dispatcher.
#if (!defined(CPU_TRAP_PROFILE) || !CPU_TRAP_PROFILE) && \
    (!defined(CPU_NATIVE_QD_ACCEL) || !CPU_NATIVE_QD_ACCEL) && \
    (!defined(CPU_NATIVE_IS_LAYER) || !CPU_NATIVE_IS_LAYER)
#define AOT_TOOLBOX_TRAP(trap_addr, autopop, here_pc, next_pc, remaining) do { \
	const bool aot_trap_ok = (likely(regs.vbr == 0 && RAMSize >= 0x2cu && \
	    do_get_mem_long((uae_u32 *)(RAMBaseHost + 0x28)) == aot_trap_vec_cached)) \
	    ? (aot_trap_vec_native && regs.s && !regs.m && !regs.t0 && !regs.t1) \
	    : aot_far.trap_verify(); \
	if (unlikely(!aot_trap_ok)) \
		AOT_EXCEPTION(0xA, 0, (here_pc), (remaining)); \
	const uaecptr aot_routine = get_long((trap_addr)); \
	if (!(autopop)) \
		AOT_PUSH_RETURN((next_pc)); \
	const uae_u32 aot_d2 = m68k_dreg(regs, 2); \
	SET_CZNV_ZN(aot_d2 == 0, ((uae_s32)aot_d2) < 0); \
	AOT_EXIT_DYN(aot_routine); \
} while (0)
#else
#define AOT_TOOLBOX_TRAP(trap_addr, autopop, here_pc, next_pc, remaining) \
	AOT_EXCEPTION(0xA, 0, (here_pc), (remaining))
#endif

// All region functions lie together, between aot_blob_begin and
// aot_blob_end, in the order the generated file defines them.
#if defined(__riscv)
#ifndef AOT_REGION_ATTR
#define AOT_REGION_ATTR __attribute__((section(".text.aot_regions"), noinline))
#endif
#if defined(__GNUC__) && !defined(__clang__)
#define AOT_HOT_REGION_ATTR AOT_REGION_ATTR __attribute__((optimize("O2", "no-jump-tables", "wrapv", "no-strict-aliasing")))
#else
#define AOT_HOT_REGION_ATTR AOT_REGION_ATTR
#endif
#define AOT_BLOB_MARK(name) \
	__asm__(".pushsection .text.aot_regions,\"ax\",@progbits\n" \
	        ".balign 4\n.globl " #name "\n" #name ":\n.popsection\n")
#else
#ifndef AOT_REGION_ATTR
#define AOT_REGION_ATTR __attribute__((noinline))
#endif
#define AOT_HOT_REGION_ATTR AOT_REGION_ATTR
#define AOT_BLOB_MARK(name)
#endif

#endif /* AOT_CODEGEN_H */
