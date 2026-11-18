#pragma once
#include "sysdeps.h"
#include <cassert>
#include <cstring>
#include <ctime>

#ifdef __APPLE__
using loff_t = int32_t;
#endif

// Stand-ins for guest memory, the Slot Manager and NewPtrSysClear. The display
// tests run the real Video Driver Control and Status routines against this RAM.
inline uint8 test_ram[0x10000] = {};
inline uint8 test_slot_rom[256] = {};
inline uint8 *ROMBaseHost = test_slot_rom;
inline constexpr uint32 ROMBaseMac = 0;
struct M68kRegisters { uint32 d[8], a[8]; };
inline uint8 *Mac2HostAddr(uint32 p) { assert(p < sizeof(test_ram)); return test_ram + p; }
inline uint8 ReadMacInt8(uint32 p) { return *Mac2HostAddr(p); }
inline uint16 ReadMacInt16(uint32 p) { return (uint16(ReadMacInt8(p)) << 8) | ReadMacInt8(p + 1); }
inline uint32 ReadMacInt32(uint32 p) { return (uint32(ReadMacInt16(p)) << 16) | ReadMacInt16(p + 2); }
inline void WriteMacInt8(uint32 p, uint8 value) { *Mac2HostAddr(p) = value; }
inline void WriteMacInt16(uint32 p, uint16 value) {
    WriteMacInt8(p, value >> 8);
    WriteMacInt8(p + 1, value);
}
inline void WriteMacInt32(uint32 p, uint32 value) {
    WriteMacInt16(p, value >> 16);
    WriteMacInt16(p + 2, value);
}
inline void Mac2Mac_memcpy(uint32 dst, uint32 src, uint32 bytes) {
    assert(bytes <= sizeof(test_ram) && dst <= sizeof(test_ram) - bytes && src <= sizeof(test_ram) - bytes);
    memmove(Mac2HostAddr(dst), Mac2HostAddr(src), bytes);
}
inline void Execute68kTrap(uint16 trap, M68kRegisters *regs) {
    static uint32 next = 0x4000;
    if (trap == 0xa71e) {
        regs->a[0] = next;
        next += regs->d[0];
        assert(next <= sizeof(test_ram));
    }
}
inline void ChecksumSlotROM() {}
