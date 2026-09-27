// Condition codes emitted by tools/cpu_gen/gencpu.c.
//
// The generator computes N, Z, V and C from operands and results shifted to
// the top of a 32-bit word. This test checks that form against the classic
// UAE formulas (per-size sign extension) it replaced: ADD/SUB/CMP
// exhaustively for bytes, every source against structured and random
// destinations for words, and edge cases plus random pairs for longs; the
// logical-operation N/Z for every byte and word and many longs.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

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
    printf("PASS: %llu flag cases match the classic formulas\n", (unsigned long long)checked);
    return 0;
}
