#include <cassert>
#include <cstdio>
#include <cstring>
#include <array>
#include <vector>
#include <random>
#include "sysdeps.h"
static uint8 ram[1 << 20], framebuffer[640 * 400];
static uint32 RAMSize = sizeof(ram), MacFrameSize = sizeof(framebuffer), MacFrameBaseMac = 0x10000000;
static uint8 *RAMBaseHost = ram, *MacFrameBaseHost = framebuffer;
static constexpr int FLAYOUT_DIRECT = 1;
static int MacFrameLayout = FLAYOUT_DIRECT;
static uint8 get_byte(uint32 a) { assert(a < sizeof(ram)); return ram[a]; }
static uint16 get_word(uint32 a) { return (get_byte(a) << 8) | get_byte(a + 1); }
static uint32 get_long(uint32 a) { return (uint32(get_word(a)) << 16) | get_word(a + 2); }
static void put_byte(uint32 a, uint8 v) { assert(a < sizeof(ram)); ram[a] = v; }
static void put_word(uint32 a, uint16 v) { put_byte(a, v >> 8); put_byte(a + 1, v); }
static void put_long(uint32 a, uint32 v) { put_word(a, v >> 16); put_word(a + 2, v); }
static struct { uint32 a[8], pc; } regs;
#define m68k_areg(r,n) ((r).a[n])
static void m68k_incpc(int n) { regs.pc += n; }
static void VideoMarkDirtyRange(uint32, uint32) {}
#define QUICKDRAW_HOST_TEST
#include "../src/basilisk/quickdraw_accel.cpp"

static void rect(uint32 a, int top, int left, int bottom, int right) {
    put_word(a, top); put_word(a+2, left); put_word(a+4, bottom); put_word(a+6, right);
}
static NativeBitmap bitmap(uint32 a, uint32 base, uint16 depth, uint32 table = 0x2000) {
    put_long(a, base); put_word(a+4, 0x8000 | 128);
    rect(a+6, 0, 0, 4, depth == 8 ? 128 : 1024);
    put_word(a+16, 0); put_word(a+30, 0); put_word(a+32, depth);
    put_word(a+34, 1); put_word(a+36, depth); put_long(a+42, table);
    put_long(table, table+0x100);
    NativeBitmap b; assert(readBitmap(a, b)); return b;
}
static void port(uint32 p, uint32 pixmap) {
    regs.a[5]=0x100; put_long(0x100,0x104); put_long(0x104,p);
    put_long(p+2,0x200); put_long(0x200,pixmap); put_word(p+6,0xc000);
    rect(p+16,0,0,4,128);
    put_long(p+24,0x300); put_long(0x300,0x400); put_word(0x400,10); rect(0x402,0,0,4,128);
    put_long(p+28,0x304); put_long(0x304,0x410); put_word(0x410,10); rect(0x412,0,0,4,128);
}
int main() {
    const QDRect area={0,0,4,100};
    auto src=bitmap(0x1000,0x10000,8), dst=bitmap(0x1100,0x20000,8);
    uint64 copied=0;
    memset(ram+dst.base,0xaa,512);
    assert(copyRectBytes(src,dst,area,area,copied));
    put_long(dst.record+42,0x2200); // different palette requires color translation
    std::array<uint8,sizeof(ram)> before; memcpy(before.data(),ram,sizeof(ram));
    assert(!copyRectBytes(src,dst,area,area,copied));
    assert(memcmp(before.data(),ram,sizeof(ram))==0);
    put_long(dst.record+42,0x2000);
    dst.base=src.base+128; // overlapping, distinct bitmap views
    assert(!copyRectBytes(src,dst,area,area,copied));
    assert(memcmp(before.data()+src.base,ram+src.base,512)==0);
    dst=src; assert(copyRectBytes(src,dst,{0,0,3,100},{1,0,4,100},copied));
    NativeBitmap invalid;
    for (auto field : {16,30,34,36}) {
        const auto saved=get_word(src.record+field); put_word(src.record+field,99);
        assert(!readBitmap(src.record,invalid)); put_word(src.record+field,saved);
    }
    // Native MoveTo must not bypass picture/region/poly recording or GrafProcs.
    auto mono=bitmap(0x1200,0x30000,1); port(0x800,mono.record);
    put_word(0x500,2); put_word(0x502,5); regs.a[7]=0x500;
    for (auto field : {92,96,100,104}) {
        put_long(0x800+field,0x9999); memcpy(before.data(),ram,sizeof(ram));
        assert(!tryMoveTo(0x500)); assert(memcmp(before.data(),ram,sizeof(ram))==0);
        assert(regs.a[7]==0x500 && regs.pc==0); put_long(0x800+field,0);
    }
    assert(tryMoveTo(0x500)); assert(get_word(0x832)==5);
    // Nonrectangular ScrollRect cannot return a bounding-box approximation.
    put_word(0x400,28);
    const uint16 stream[]={0,0,20,0x7fff,4,0,20,0x7fff,0x7fff};
    for (int i=0;i<9;++i) put_word(0x40a+i*2,stream[i]);
    put_long(0x600,0x700); put_long(0x700,0x720); put_word(0x602+4,1);
    put_long(0x608,0x740); rect(0x740,0,0,4,100);
    memcpy(before.data(),ram,sizeof(ram)); assert(!tryScrollRect(0x600));
    assert(memcmp(before.data(),ram,sizeof(ram))==0);
    // Existing monochrome overlap algorithm: independent bitwise snapshot oracle.
    std::mt19937 randomizer(8714);
    NativeBitmap bits={0,0x40000,128,{0,0,4,1024},1};
    for (int trial=0;trial<10000;++trial) {
        for(int i=0;i<512;++i)ram[bits.base+i]=randomizer();
        std::array<uint8,512> original,expected;
        memcpy(original.data(),ram+bits.base,512);expected=original;
        int sx=randomizer()%850,dx=randomizer()%850,width=1+randomizer()%170;
        int sy=randomizer()%4,dy=randomizer()%4;
        for(int x=0;x<width;++x){
            bool bit=(original[sy*128+(sx+x)/8] & (0x80>>((sx+x)%8)))!=0;
            auto &byte=expected[dy*128+(dx+x)/8];auto mask=0x80>>((dx+x)%8);
            byte=bit?byte|mask:byte&~mask;
        }
        assert(copyPixelRange(bits,bits,sx,sy,dx,dy,width,sy==dy&&dx>sx,copied));
        assert(memcmp(expected.data(),ram+bits.base,512)==0);
    }
    puts("PASS: QuickDraw format/palette/alias/recording guards and 10000 overlapping monochrome copies");
}
