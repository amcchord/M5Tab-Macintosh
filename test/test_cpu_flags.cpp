// Condition codes emitted by tools/cpu_gen/gencpu.c.
//
// The generator computes N, Z, V and C from operands and results shifted to
// the top of a 32-bit word. This test checks that form against the classic
// UAE formulas (per-size sign extension) it replaced: ADD/SUB/CMP
// exhaustively for bytes, every source against structured and random
// destinations for words, and edge cases plus random pairs for longs; the
// logical-operation N/Z for every byte and word and many longs.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

// Minimal host stubs so newcpu.h and m68k.h compile directly without -I flags.
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint8_t uae_u8;
typedef int8_t uae_s8;
typedef uint16_t uae_u16;
typedef int16_t uae_s16;
typedef uint32_t uae_u32;
typedef int32_t uae_s32;
typedef uint64_t uae_u64;
typedef int64_t uae_s64;
typedef uint32_t uaecptr;
typedef uae_u32 memptr;

#define REGPARAM
#define REGPARAM2
#define ALWAYS_INLINE inline __attribute__((always_inline))
#define UAE_OPAQUE(x) do { __asm__("" : "+r"(x)); } while (0)
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define ENUMDECL typedef enum
#define ENUMNAME(name) name
#define HAVE_GET_WORD_UNSWAPPED 1
#define ENABLE_EXCLUSIVE_SPCFLAGS 0

static uae_u8 dummy_ram[64] = {};
static uae_u8 dummy_rom[32] = {};
static inline uae_u32 mem_ram_size() { return sizeof(dummy_ram); }
static inline uae_u8 *mem_ram_base() { return dummy_ram; }

static uae_u8 *ROMBaseHost = dummy_rom;
static uae_u32 ROMSize = sizeof(dummy_rom);
static const uaecptr ROMBaseMac = 0x40800000u;

static inline uae_u32 do_get_mem_long(uae_u32 *a) {
    uint32_t v;
    std::memcpy(&v, a, sizeof(v));
    return __builtin_bswap32(v);
}
static inline uae_u16 do_get_mem_word(uae_u16 *a) {
    uint16_t v;
    std::memcpy(&v, a, sizeof(v));
    return __builtin_bswap16(v);
}
static inline uae_u8 do_get_mem_byte(uae_u8 *a) { return *a; }
static inline uae_u32 get_long(uaecptr addr) {
    assert(addr + 4 <= sizeof(dummy_ram));
    return do_get_mem_long(reinterpret_cast<uae_u32 *>(dummy_ram + addr));
}
static inline void put_long(uaecptr addr, uae_u32 val) {
    assert(addr + 4 <= sizeof(dummy_ram));
    uint32_t be = __builtin_bswap32(val);
    std::memcpy(dummy_ram + addr, &be, sizeof(be));
}
static inline uae_u8 *get_real_address(uaecptr addr) {
    if (addr < sizeof(dummy_ram)) return dummy_ram + addr;
    if (addr >= ROMBaseMac && addr < ROMBaseMac + ROMSize) return ROMBaseHost + (addr - ROMBaseMac);
    return nullptr;
}

#include "../src/basilisk/uae_cpu/newcpu.h"

struct regstruct regs{};
struct flag_struct regflags{};
void m68k_set_s(int) {}
void Exception(int, uaecptr) {}
void flush_icache(int) {}
void MakeSR(void) {}
void MakeFromSR(void) {}

enum Op { ADD, SUB, CMP };
struct Flags { uint32_t n, z, v, c; bool operator!=(const Flags &o) const { return n != o.n || z != o.z || v != o.v || c != o.c; } };

static int32_t sext(uint32_t value, int bits) { return bits == 32 ? (int32_t)value : (int32_t)(value << (32 - bits)) >> (32 - bits); }
static uint32_t mask(uint32_t value, int bits) { return bits == 32 ? value : value & ((1u << bits) - 1); }

static Flags classic(Op op, int bits, uint32_t src, uint32_t dst)
{
    // Wrapping arithmetic on the sign-extended operands (as the hardware does).
    const uint32_t sd = (uint32_t)sext(dst, bits), ss = (uint32_t)sext(src, bits);
    const uint32_t newv = op == ADD ? sd + ss : sd - ss;
    const bool flgs = sext(src, bits) < 0, flgo = sext(dst, bits) < 0, flgn = sext(newv, bits) < 0;
    Flags f;
    f.z = sext(newv, bits) == 0;
    f.n = flgn;
    if (op == ADD) {
        f.v = (flgs ^ flgn) & (flgo ^ flgn);
        f.c = mask(~dst, bits) < mask(src, bits);
    } else if (op == SUB) {
        f.v = (flgs ^ flgo) & (flgn ^ flgo);
        f.c = mask(src, bits) > mask(dst, bits);
    } else {
        f.v = (flgs != flgo) && (flgn != flgo);
        f.c = mask(src, bits) > mask(dst, bits);
    }
    return f;
}

// Mirrors the generated code: src/dst hold the sign-extended operand values
// that the handlers declare as uae_s8/uae_s16/uae_s32.
static Flags shifted(Op op, int bits, uint32_t src_raw, uint32_t dst_raw)
{
    const uint32_t src = (uint32_t)sext(src_raw, bits), dst = (uint32_t)sext(dst_raw, bits);
    const uint32_t newv = op == ADD ? dst + src : dst - src;
    const int shift = 32 - bits;
    const uint32_t fs = src << shift, fd = dst << shift, fr = newv << shift;
    Flags f;
    f.z = fr == 0;
    if (op == ADD) {
        f.v = ((fs ^ fr) & (fd ^ fr)) >> 31;
        f.c = fr < fd;
    } else {
        f.v = ((fs ^ fd) & (fr ^ fd)) >> 31;
        f.c = fs > fd;
    }
    f.n = fr >> 31;
    return f;
}

static uint64_t checked = 0;
static void check(int bits, uint32_t src, uint32_t dst)
{
    for (Op op : {ADD, SUB, CMP}) {
        if (classic(op, bits, src, dst) != shifted(op, bits, src, dst)) {
            printf("FAIL op=%d bits=%d src=%08x dst=%08x\n", op, bits, src, dst);
            assert(false);
        }
        ++checked;
    }
}

static void check_logical(int bits, uint32_t raw)
{
    // Classic: SET_ZFLG (((uae_sN)(v)) == 0); SET_NFLG (((uae_sN)(v)) < 0);
    // Generated values may carry any upper bits (e.g. uae_u32 results).
    for (uint32_t upper : {0u, 0xffffffffu, 0x5a5a5a5au}) {
        const uint32_t value = bits == 32 ? raw : (raw & ((1u << bits) - 1)) | (upper << bits);
        const uint32_t z = sext(value, bits) == 0, n = sext(value, bits) < 0;
        const int shift = 32 - bits;
        assert(z == ((value << shift) == 0));
        assert(n == ((value << shift) >> 31));
        ++checked;
    }
}

static void test_packed_flag_struct_and_macros()
{
#ifndef OPTIMIZED_FLAGS
    static_assert(sizeof(flag_struct) == 8, "flag_struct should be 8 bytes (cznv + x + padding)");
    static_assert(offsetof(flag_struct, cznv) == 0, "cznv at offset 0");
    static_assert(offsetof(flag_struct, c) == 0, "c at offset 0");
    static_assert(offsetof(flag_struct, z) == 1, "z at offset 1");
    static_assert(offsetof(flag_struct, n) == 2, "n at offset 2");
    static_assert(offsetof(flag_struct, v) == 3, "v at offset 3");
    static_assert(offsetof(flag_struct, x) == 4, "x at offset 4");
#endif

    // Verify CLEAR_CZNV, SET_CZNV_ZN, SET_CZNV_ALL preserve X and pack bytes cleanly.
    SET_XFLG(1);
    SET_CZNV_ALL(1, 1, 1, 1);
    assert(GET_CFLG == 1 && GET_ZFLG == 1 && GET_NFLG == 1 && GET_VFLG == 1 && GET_XFLG == 1);
#if !defined(OPTIMIZED_FLAGS) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    assert(regflags.cznv == 0x01010101u);
#endif

    CLEAR_CZNV;
    assert(GET_CFLG == 0 && GET_ZFLG == 0 && GET_NFLG == 0 && GET_VFLG == 0 && GET_XFLG == 1);
#ifndef OPTIMIZED_FLAGS
    assert(regflags.cznv == 0u);
#endif

    SET_XFLG(0);
    for (int z = 0; z <= 1; ++z) {
        for (int n = 0; n <= 1; ++n) {
            SET_CZNV_ALL(1, 1 - z, 1 - n, 1);
            SET_XFLG(z ^ n);
            SET_CZNV_ZN(z, n);
            assert(GET_CFLG == 0);
            assert(GET_ZFLG == z);
            assert(GET_NFLG == n);
            assert(GET_VFLG == 0);
            assert(GET_XFLG == (z ^ n));
        }
    }

    for (int c = 0; c <= 1; ++c) {
        for (int z = 0; z <= 1; ++z) {
            for (int n = 0; n <= 1; ++n) {
                for (int v = 0; v <= 1; ++v) {
                    SET_XFLG(c ^ 1);
                    SET_CZNV_ALL(c, z, n, v);
                    assert(GET_CFLG == c);
                    assert(GET_ZFLG == z);
                    assert(GET_NFLG == n);
                    assert(GET_VFLG == v);
                    assert(GET_XFLG == (c ^ 1));
                    COPY_CARRY;
                    assert(GET_XFLG == c);
                    ++checked;

                    for (int cc = 0; cc < 16; ++cc) {
                        bool expected = false;
                        switch (cc) {
                        case 0: expected = true; break;                  // T
                        case 1: expected = false; break;                 // F
                        case 2: expected = !c && !z; break;              // HI
                        case 3: expected = c || z; break;                // LS
                        case 4: expected = !c; break;                    // CC
                        case 5: expected = c; break;                     // CS
                        case 6: expected = !z; break;                    // NE
                        case 7: expected = z; break;                     // EQ
                        case 8: expected = !v; break;                    // VC
                        case 9: expected = v; break;                     // VS
                        case 10: expected = !n; break;                   // PL
                        case 11: expected = n; break;                    // MI
                        case 12: expected = (n == v); break;             // GE
                        case 13: expected = (n != v); break;             // LT
                        case 14: expected = !z && (n == v); break;       // GT
                        case 15: expected = z || (n != v); break;        // LE
                        }
                        assert((cctrue(cc) != 0) == expected);
                        ++checked;
                    }
                }
            }
        }
    }
}

static void test_get_iword_s16_and_dreg_set()
{
    // Test get_iword_s16 exhaustively for all 65536 big-endian 16-bit bit patterns
    // at aligned and odd byte offsets.
    uint8_t buf[12] = {};
    regs.pc_p = buf;
    for (uint32_t w = 0; w <= 0xFFFFu; ++w) {
        const int16_t expected = (int16_t)(uint16_t)w;
        for (int off : {0, 1, 2, 3, 4}) {
            buf[off] = (uint8_t)(w >> 8);
            buf[off + 1] = (uint8_t)(w & 0xFFu);
            const uae_s32 got = get_iword_s16(off);
            assert(got == (uae_s32)expected);
            ++checked;
        }
    }

    // Test m68k_dreg_w_set and m68k_dreg_b_set across all 8 D-registers,
    // including high-bit garbage in both register and value.
    const uint32_t init_patterns[] = {0x00000000u, 0xFFFFFFFFu, 0x12345678u, 0xA5B6C7D8u};
    const uint32_t values[] = {0u, 1u, 0x7Fu, 0x80u, 0xFFu, 0x7FFFu, 0x8000u, 0xFFFFu,
                               0xFFFF8000u, 0xDEAD1234u, 0xCAFE0080u};
    for (int r = 0; r < 8; ++r) {
        for (uint32_t init : init_patterns) {
            for (uint32_t val : values) {
                m68k_dreg(regs, r) = init;
                m68k_dreg_w_set(regs, r, val);
                assert(m68k_dreg(regs, r) == ((init & 0xFFFF0000u) | (val & 0xFFFFu)));

                m68k_dreg(regs, r) = init;
                m68k_dreg_b_set(regs, r, val);
                assert(m68k_dreg(regs, r) == ((init & 0xFFFFFF00u) | (val & 0xFFu)));
                checked += 2;
            }
        }
    }
}

static void test_rts_jsr_bsr()
{
    m68k_areg(regs, 7) = 48;

    // Fast RAM m68k_setpc
    m68k_setpc(16);
    assert(regs.pc == 16 && regs.pc_p == dummy_ram + 16 && regs.pc_oldp == dummy_ram + 16);

    // Cold ROM m68k_setpc fallback via get_real_address
    m68k_setpc(ROMBaseMac + 8);
    assert(regs.pc == ROMBaseMac + 8 && regs.pc_p == dummy_rom + 8 && regs.pc_oldp == dummy_rom + 8);

    // JSR pushes oldpc and transfers PC to dest
    m68k_setpc(20);
    m68k_do_jsr(24, 32);
    assert(m68k_areg(regs, 7) == 44);
    assert(get_long(44) == 24);
    assert(m68k_getpc() == 32 && regs.pc_p == dummy_ram + 32);

    // BSR pushes oldpc and adds signed offset to old PC (20 -> 20 - 4 = 16)
    m68k_setpc(20);
    m68k_do_bsr(22, -4);
    assert(m68k_areg(regs, 7) == 40);
    assert(get_long(40) == 22);
    assert(m68k_getpc() == 16 && regs.pc_p == dummy_ram + 16);

    // RTS pops return addresses back in LIFO order
    m68k_do_rts();
    assert(m68k_areg(regs, 7) == 44);
    assert(m68k_getpc() == 22 && regs.pc_p == dummy_ram + 22);

    m68k_do_rts();
    assert(m68k_areg(regs, 7) == 48);
    assert(m68k_getpc() == 24 && regs.pc_p == dummy_ram + 24);
    checked += 6;
}

int main()
{
    for (uint32_t v = 0; v < 65536; ++v) { check_logical(8, v); check_logical(16, v); }
    for (uint32_t s = 0; s < 256; ++s)
        for (uint32_t d = 0; d < 256; ++d) check(8, s, d);

    std::mt19937 rng(0x68040);
    std::vector<uint32_t> edges;
    for (uint32_t base : {0u, 1u, 0x7fu, 0x80u, 0xffu, 0x100u, 0x7fffu, 0x8000u, 0xffffu,
                          0x7fffffffu, 0x80000000u, 0xffffffffu})
        for (int delta = -2; delta <= 2; ++delta) edges.push_back(base + delta);

    for (uint32_t s = 0; s < 65536; ++s) {
        for (uint32_t d : edges) check(16, s, d);
        for (int i = 0; i < 64; ++i) check(16, s, rng());
    }
    for (uint32_t s : edges) for (uint32_t d : edges) check(32, s, d);
    for (int i = 0; i < 5000000; ++i) check(32, rng(), rng());
    for (uint32_t v : edges) check_logical(32, v);
    for (int i = 0; i < 1000000; ++i) check_logical(32, rng());
    test_packed_flag_struct_and_macros();
    test_get_iword_s16_and_dreg_set();
    test_rts_jsr_bsr();
    printf("PASS: %llu flag and helper cases verified\n", (unsigned long long)checked);
    return 0;
}
