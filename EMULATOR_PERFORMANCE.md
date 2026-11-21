# Emulator performance design

This guide explains the interpreter and display fast paths added in 5.0.1
and the translated code added after it: how each one works, what it assumes,
how it was measured and what to recheck when changing it. [PERFORMANCE_REPORT.md](PERFORMANCE_REPORT.md) keeps the
build-by-build measurements. [AUTOMATION.md](AUTOMATION.md) documents the
benchmark and diagnostic commands.

## Results

Speedometer 4.02, **Tests > Performance Rating** (Quadra 605 = 1.0), all four
tests, on a Tab5 with ESP32-P4 revision 1.3 at 360 MHz running Mac OS 8:

| Score | 5.0 | 5.0.1 | Translated ROM and system code | With Speedometer's own code translated |
|---|---:|---:|---:|---:|
| CPU | 0.545 | 0.803–0.804 | 0.804 | 1.603–1.604 |
| Graphics | 0.242–0.244 | 0.495–0.497 | 1.023–1.025 | 1.012–1.014 |
| Disk | 1.02 | 1.029–1.033 | 1.027–1.028 | 1.024–1.030 |
| Math | 6.68 | 9.43–9.48 | 10.48 | 11.11–11.12 |
| Performance Rating | 0.458 | 0.765–0.766 | 1.006 | 1.334–1.335 |

The two translated columns differ in one thing. The CPU test is a sort loop
inside Speedometer itself; it runs no ROM or system code, so translating
those leaves it where the interpreter puts it. The last column also
translates the application code that ran while the profile was taken, which
includes that loop. It shows what profiling a program buys that program, not
what an unprofiled program gets: for that, read the third column.

Color QuickDraw (the same program's **Color** tests) went from 0.549 to
1.00 in monochrome and from 0.509 to 0.87 in 8-bit colour (1.007 and 0.869
in the third column's build, 1.001 and 0.868 in the fourth's). A cold start
reaches the Finder in 49.6 to 49.7 s, against 49.5 s for 5.0.1.

Native QuickDraw acceleration stays disabled, as in 5.0.

## What limits speed on the ESP32-P4

- **Everything large lives in PSRAM.** Guest RAM, ROM, the guest frame
  buffer, the panel frame buffer, the 256 KB opcode table (`cpufunctbl`) and
  the 256 KB memory-bank table do not fit contiguously in internal SRAM on the
  Tab5, so they fall back to PSRAM, as in 5.0. A missed PSRAM cache line costs
  on the order of 180 CPU cycles, so staying in cache matters more than
  instruction count.
- **Both cores share the data caches.** The display task on core 0 competes
  with the emulator on core 1 for L1D, L2 and PSRAM bandwidth. Halving the
  display refresh rate, as a diagnostic, raised Graphics by about 15%.
- **No byte-swap instruction.** Revision 1.3 silicon lacks the RISC-V Zbb
  extension, so every big-endian guest access has to be assembled from bytes
  or shifts.
- **Mac OS calls the Toolbox constantly.** Before 5.0.1, about 11% of sampled
  emulator time was spent in the ROM's A-line trap dispatcher.
- **The Graphics test runs in 1-bit mode.** Dirty tracking and rendering
  costs at 1 bit per pixel dominate that score.

## Measuring

Use the same procedure for every comparison and keep the firmware ELF of
every build you measure.

```sh
# Unattended Performance Rating (all tests, or a subset)
python3 tools/speedometer_benchmark.py --no-reset --suite rating --label pr
python3 tools/speedometer_benchmark.py --no-reset --suite rating --tests graphics --label gfx

# Profile while the test runs, then symbolize with the exact ELF
python3 tools/speedometer_benchmark.py --no-reset --suite rating --tests cpu \
  --profile 3000 --label cpu-profile
python3 tools/perf_report.py artifacts/performance-runs/<run> \
  .pio/build/esp32p4_pioarduino_debug/firmware.elf
```

- Run each variant three times. CPU and Graphics repeat within 0.001 between
  runs, so a change of 1% is real. Math-only runs repeat within 0.2%; full
  ratings vary more on Math, so compare Math only within one procedure.
- The `PERF` sampler interrupts one core at a fixed rate. Each sample records
  the host PC and return address plus the current 68k PC and opcode. It also
  reads cycle, instruction and cache counters for both cores. The "miss"
  counters advance once per stall cycle, so treat them as stall proxies, not
  line counts. `perf_report.py` prints per-function, per-opcode and per-68k-page
  histograms, plus MIPS and instructions per 68k instruction.
- `PANEL VERIFY` decodes the guest frame buffer through the live palette and
  compares all 921,600 panel pixels. Run it after benchmarks and depth changes.
  Any mismatch is a display bug.
- `PEEK ADDR LEN` reads guest RAM or ROM without side effects. For example,
  `PEEK 20C 4` reads the Mac clock and `PEEK 16A 4` reads Ticks.

## CPU interpreter

### Guest memory access

Files: `src/basilisk/uae_cpu/memory.h`, `memory.cpp`, `src/basilisk/sysdeps.h`.

Every 68k load and store in the generated handlers goes through
`longget_fastpath`, `wordput_fastpath` and so on:

1. **RAM is handled inline.** One unsigned compare against `RAMSize`, then a
   byte-wise big-endian access at `RAMBaseHost + addr`.
   `mem_ram_size()` and `mem_ram_base()` read these globals through an
   input-free asm statement. GCC therefore treats them as constants and loads
   each once per handler, even across guest stores that could alias them.
   Both globals are in `.sdata`, so each load is a single gp-relative
   instruction.
2. **Everything else goes to an out-of-line thunk.** A call such as
   `mem_slow_lget_thunk` is emitted as `auipc t0` / `jalr t0`, with the
   argument in `a0` (and the value in `a1` for stores). The thunk preserves
   every integer register except `a0` and `t0`. The inline path therefore
   contains no real C call, and handlers do not spill callee-saved registers.
3. **Thunks finish the common slow cases with four scratch registers.** Loads
   check the ROM, then the direct-layout frame buffer at `0xA0000000`.
   Stores handle only the frame buffer: write the pixels, `fence w, w`, then
   set one damage byte per 32-pixel span touched (see
   [Dirty spans](#dirty-spans)). Anything else saves the caller-saved
   registers and calls the C `mem_slow_*` function, which dispatches through
   the bank table.

Byte-wise access (`do_get_mem_long` and friends) is shorter than a word
access plus a software swap. It also never makes a misaligned host access,
since 68k word and long operands only need 2-byte alignment. GCC's bswap pass
would rebuild the byte loads into a word load plus a long swap sequence;
`UAE_OPAQUE` is an empty asm that hides the bytes' origin and prevents this at
no cost.

**Invariants**

- `RAMBaseHost` and `RAMSize` must not change after the first guest
  instruction; the "constant" asm relies on it.
- A thunk may use only `t1`–`t4` on its fast path, and must save them. Its full
  path must save every caller-saved integer register. Call sites declare the
  floating-point temporaries clobbered, because bank handlers are ordinary C.
- Define `MEM_SLOW_DIRECT_CALLS` to replace the thunks with plain C calls when
  debugging a suspected thunk problem.

### PC, prefetch and effective addresses

`m68k_getpc`, `m68k_setpc`, `next_iword` and related helpers in `newcpu.h` are
forced inline, with RAM and ROM fast paths for branch targets.
`get_disp_ea_020` handles the common brief-extension-word form inline and calls
`get_disp_ea_020_full` (in IRAM) for full extension words.

### Native A-line trap dispatch

File: `src/basilisk/uae_cpu/newcpu.cpp` (`CPU_NATIVE_TRAP_DISPATCH`, default 1).

Every Toolbox or OS call is an A-line instruction. The ROM handles it with an
exception frame, then a dispatcher that saves registers, looks up the trap
table and jumps to the routine. `op_illg` now performs the dispatcher's work
directly:

- **Toolbox traps (bit 11 set):** read the routine from the Toolbox table at
  `$0E00`, push the return address unless the trap auto-pops (bit 10), and set
  the condition codes as the dispatcher's final `MOVE.L (SP)+,D2` would.
- **OS traps:** build the stack, registers and CCR that the ROM prologue
  leaves at its `JSR`, read the routine from the OS table at `$0400`, and
  return into the ROM's own epilogue, so the epilogue restores registers and
  tests D0 exactly as before. There are two epilogues, depending on whether
  trap bit 8 asks to preserve A0.

The shortcut is taken only when both conditions hold:

1. The code at the line-A vector (`$28`) matches the ROM dispatcher's 66-word
   signature, `kTrapDispatcher`. The comparison is cached per vector address;
   the ROM copy is read-only, so its contents cannot change underneath the
   cache.
2. The CPU is already in the state that exception entry would produce:
   supervisor mode, interrupt stack, no trace.

Anything else takes the architectural exception path. That includes a
debugger's own trap handler, a different ROM, or the first call after the
vector moves. Because patches installed through the trap tables are read from
those tables, Mac OS patches still run.

Measured effect: trap dispatch shipped in the same build as the IRAM
placement below. Together they removed the dispatcher's ~11% share of samples
and raised 68k MIPS in the CPU profile from 4.85 to 5.34. The CPU score did
not move; Graphics rose from 0.338 to 0.38.

### Generated condition codes

Files: `tools/cpu_gen/gencpu.c`, generated `src/basilisk/uae_cpu/generated/cpuemu.cpp`.

For ADD, SUB and CMP, the generator shifts the source, destination and result
so each operand's sign bit lands in bit 31 (`x << (32 - size)`):

- Z = result == 0, N = result >> 31
- V from the sign-bit formula on the shifted values
- C = result < destination for ADD, source > destination for SUB and CMP
- X copies C, except for CMP

Logical operations use the same shifted N/Z. This removes the per-size sign
extension of the classic UAE formulas. The ADD/SUB/CMP change raised CPU from
about 0.78 to 0.804. The logical-operation change measured neutral and was
kept so all generated flags follow one form. `test/test_cpu_flags.cpp` checks
the new forms against the classic ones: exhaustively for bytes, and with
structured and random operands for words and longs.

Regenerate the handlers with `tools/cpu_gen/generate_cpu_tables.sh`. Its
output `cpuemu.cpp` must be byte-identical to the committed file. The
checked-in `cpustbl.cpp` and `cpudefs.cpp` carry older hand edits and differ
from raw generator output; 5.0.1 does not change them.

### Placement in internal RAM

- `tools/cpu_gen/iram_handlers.txt` lists 295 opcode handlers chosen from
  CPU and Graphics profiles. `apply_iram_handlers.py`, run as step 6 of the
  generator script, marks them `IRAM_ATTR`.
- The exception, status-register, extension-word and memory slow-path
  functions are also in IRAM. The small MOVEM, immediate and address-register
  lookup tables are in DRAM.
- `regs`, `regflags` and the memory-map globals use `SDATA_ATTR`, which gives
  them gp-relative addressing. `video_dirty_shift` and `video_dirty_limit` do
  too.

Cost: about 46 KB more IRAM. On the Tab5, internal SRAM free after init falls
from 231 KB (5.0) to 177 KB. After changing either list, compare the boot log
line `Internal SRAM after init` and re-measure. More IRAM is not automatically
faster, because it shrinks the heap that other drivers use.

## Translated code

Files: `tools/aot/`, `src/basilisk/aot/`, `test/aot_host/`, `test/test_aot.py`.
Procedure: [tools/aot/README.md](tools/aot/README.md).

The interpreter pays for every 68k instruction separately: fetch the opcode,
look up and call a handler, decode operands, compute every condition code,
store the PC. Most of that is the same each time an instruction runs. For
code whose bytes are known in advance it can be done once, at build time.

`aot_translate.py` takes the instructions a profiling build saw executed and
writes one C++ function per *region* of them. The body of each instruction is
the text of the interpreter's own handler from `cpuemu.cpp`, with the opcode,
extension words and PC replaced by constants. A translated instruction
therefore computes what the interpreter computes, by construction. The
compiler then removes what the constants make unnecessary: operand decoding,
and the condition codes that the following 68k code overwrites without
reading (they live in locals inside a region). Branches inside a region are
gotos.

This is not a JIT. Nothing is generated, written or made executable while the
emulator runs, and internal RAM stays under the P4's write/execute
protection. The firmware only decides which of its own functions to call.

### What is translated

| | ROM | RAM |
|---|---|---|
| Code | Quadra ROM routines | Mac OS and application code in guest RAM |
| Unit | Region, at a fixed address | Module of adjacent instructions, position independent |
| Installed | Once, by `AotInstall()` at start-up | Where the code is seen running (`AotRamSample()`) |
| Bytes checked | CRC-32 of every region, before any is installed | All of the module at installation, and again after every guest cache flush |
| Off switch | `-DAOT_ROM=0` | `-DAOT_RAM=0` |

An entry point is installed by replacing the first opcode of its 68k code
with an opcode that no 68040 instruction, Mac trap or Basilisk `EMUL_OP`
uses. The opcode table maps it to the region function. 3,840 such opcodes
exist (`0x73xx`…`0x7Fxx` with an odd high byte, and `0xF8xx`…`0xFFxx`); one
region's are consecutive and share their high byte, so the entry number is a
shift and a subtraction.

### Rules

These are what make the translation safe. Each has a test.

1. **Only counted addresses are instructions.** The profiling build counts
   every instruction it dispatches. A *sampled* PC is not evidence: handlers
   advance the PC while they run, so a sample can lie inside an instruction.
   (The first translator used samples and patched an extension word.)
2. **No entry point inside another instruction.** The bytes after the first
   word of any executed instruction are never patched, even where another
   path executes them as an instruction of their own.
3. **Anything unrecognized ends the region.** A handler body may only call
   what the translator lists (`ALLOWED_CALLS`). Traps, `EMUL_OP`s, FPU,
   privileged and exception-raising instructions are left to the
   interpreter. Division and `MULL` are translated with their exception and
   flag handling routed back through the interpreter's functions.
4. **An entry opcode is only honoured in place.** A region checks that the
   PC is where it was installed. Anywhere else (the guest copied the code),
   and in trace mode, it runs the original instruction.
5. **RAM modules follow the 68040's own contract.** Code that changes must
   not be executed until the instruction cache has been flushed; Mac OS on a
   68040 keeps to that. `CINV`/`CPUSH` and the emulator's own writes to code
   (`FlushCodeCache`) advance an epoch; a module entered in a new epoch has
   all its bytes compared first. If they differ, the entry puts the original
   instruction back and the interpreter carries on.
6. **A module is installed only where it is executing.** `AotRamSample()`
   looks at the PC between batches of instructions. Memory that merely holds
   the same bytes (a file being copied) is never touched.
7. **Regions give way.** Every backward branch polls the special flags and an
   instruction budget, so interrupts are taken as promptly as between
   interpreted instructions.
8. **Arithmetic wraps.** The handlers compute flags from sums that overflow
   a signed `int`. With operands folded to constants the compiler used that
   to drop a flag computation; the firmware is built with `-fwrapv`.

### Where the code runs

Translated code is 500 to 710 KB, depending on the profile. Internal RAM has room for about 50 KB of
emulator code, so it cannot live there. It is linked into flash and copied to
PSRAM at start-up (`AOT_PSRAM`): both are reached through the same caches,
but a cache miss is filled from PSRAM (16 lines, 200 MHz) several times
faster than from flash (4 lines, 80 MHz). That was worth 4.6% on Graphics.

The copy runs unchanged because the code holds no PC-relative reference to
anything outside itself and no absolute address of itself: memory helpers
and the three interpreter functions it needs are called at absolute
addresses (`aot_far`), and the file is compiled with `-fno-jump-tables
-fno-toplevel-reorder` (`scripts/aot_build_flags.py`).
`tools/aot/check_blob.py` checks the linked firmware for exactly this, from
the disassembly and the object file's relocation records; run it on every
build that contains translated code.

Guest memory is reached through out-of-line helpers (`mem_lget_thunk` and
friends in `memory.cpp`) rather than the inline sequences the handlers use.
Behind a 16 KB instruction cache the smaller code is faster: the change took
translated code from 640 KB to 343 KB and recovered 5% on Graphics. The
helpers finish RAM, ROM and direct-layout frame-buffer accesses themselves.

Regions that the profile shows executing 16 or more instructions per entry
keep the guest registers they use in locals (`--cache-run`).

### Testing

`AotSelfTest()` (`src/basilisk/aot/aot_selftest.cpp`) runs every entry point
from randomized CPU and memory states and compares the result with the
interpreter executing the same number of instructions from the same state:
registers, condition codes, PC, special flags, all of guest RAM, the frame
buffer and the stores outside both. It enters each region directly, through
the installed opcode, from a relocated copy of the opcode, and for RAM
modules at a different address each time, after a cache flush, and after a
flush with one byte of the module changed.

- **Host:** `test/test_aot.py` translates generated 68k code (every handler
  the translator accepts, with and without cached registers) and runs the
  self-test. `test/aot_host/build.sh` builds the same test for a real
  translation.
- **Device:** the `esp32p4_aot_selftest` profile runs it on the P4 instead of
  starting the Mac. Translated code depends on how the compiler treats the
  handlers' C, so the host result does not stand in for this one.

### What it costs

- Flash: about 0.6 MB. PSRAM: about 0.5 MB.
- The guest can read an entry opcode where it expects an instruction, if it
  reads code as data. Nothing in Mac OS 8 or Speedometer was seen to.
- A guest that changes code and executes it without flushing the cache runs
  the old code, as it could on a 68040.
- Translation is tied to a ROM and to the system software that was profiled.
  Other ROMs and systems fail the byte checks and are interpreted.
- `aot_rom_generated.cpp` is a translation of Apple's ROM and system
  software and is not part of the repository.

## Display pipeline

Files: `src/basilisk/video_esp32.cpp`, `src/board/panel_surface.h`,
`src/board/board_display_surface.cpp`, `src/board/mini_gfx/`,
`src/board/board_config.h`.

### Dirty spans

A guest frame-buffer store sets one flag byte per 32-pixel span it touches.
The span size in bytes follows the depth: `video_dirty_shift` is 2, 3, 4 or 5
for 1, 2, 4 or 8 bits per pixel. `video_dirty_limit` stops tracking below the
displayed rows. The producer writes the pixels, fences, then stores the flag.
The CPU core does no read-modify-write. The video task drains a word of flags
at a time with an atomic exchange and maps spans to tiles.

Tiles are 32×40 pixels: a 20×9 grid on the Tab5 and 20×10 on Waveshare. A span
is therefore exactly one tile column wide at every depth. The old scheme
tracked 64-byte ranges, which is 512 pixels at 1 bpp, so a one-pixel store
redrew large areas in exactly the mode the Graphics test uses.

The renderer records the changed guest rows in a 64-bit mask per tile.
Uncovered indexed tiles submit each consecutive run. A single clean row
between runs is included to keep alternating scan lines in one submission.
A write to the first and last rows of a 32×40 tile at 2× scale submits
512 bytes instead of the previous 10,240. Removing an overlay invalidates
the entire touched tile. Depth and palette changes invalidate the screen.
Failed publication retains the row masks for the retry. The masks use
1,440 bytes of internal RAM on Tab5, up from 360 bytes for the old bounds.

A native benchmark exercises the production renderer with a 220×30 moving
outline and a full-screen fill for 1,000 frames at each depth. Compared with
`82dcfe9`, the outline fell from 60,380 to 18,361 panel bytes per frame at
both 1 and 8 bpp. On an Apple Silicon Mac, median time across three runs
fell from 4.18 to 1.92 µs/frame at 1 bpp and from 4.11 to 2.27 at 8 bpp.
Full-screen writes stayed at 1,843,200 bytes/frame. Their median times fell
from 108.84 to 96.34 µs and from 113.91 to 99.86 µs respectively. All four
final panel images had matching before/after hashes. Use the device
procedure below to measure the impact on ESP32-P4 guest workloads.

To reproduce from the project root, build the same benchmark with each
version of `video_esp32.cpp`. For the baseline, put a copy from `82dcfe9`
in a directory and add that directory as the first include path.

```sh
c++ -std=c++17 -O3 -pthread -DBOARD_M5STACK_TAB5=1 -Itest/display_stubs \
  -Isrc/board -Isrc/basilisk -Isrc/basilisk/include test/bench_display_damage.cpp \
  src/board/board_display_surface.cpp src/board/mini_gfx/mini_gfx.cpp -o /tmp/bench-display
/tmp/bench-display
```

Every 30 seconds the serial `[VIDEO PERF]` report gives total `panel_bytes`
submitted, including retries, and average `detect` and `render` time in
microseconds per frame opportunity, including skips. Use a settled screen
with touch overlays hidden for the pixel check after a timing run.
[Host rendering tests](test/test_display_pipeline.cpp) · [Device comparison](TAB5_HARDWARE_HANDOFF.md#2-check-colors-rotation-and-redraws)

### Packed indexed tile writer

`writePanelIndexedTile2x` reads packed 1-, 2-, 4- or 8-bit guest rows and
writes the 2×-scaled, rotated RGB565 tile straight into the panel frame
buffer. A 256-entry table maps each palette index to a 32-bit pair of
identical RGB565 pixels. Tiles covered by a touch overlay keep the previous
compositing path.

### Uncached scanout writes

When the panel frame buffer is in PSRAM, tiles are written through its
non-cacheable alias (`SOC_NON_CACHEABLE_OFFSET`; see
`MiniGfx::scanoutFb()`). Rendering then neither evicts the emulator's cache
lines nor needs a cache writeback per tile. `flushRows` runs only when
`scanoutIsCached()` is true. This step alone raised Graphics from 0.398 to
0.492.

## Guest clock

`emul_op.cpp` now writes `TimerDateTime()` to the Mac `Time` global (`$20C`) on
each 1 Hz interrupt, as upstream Basilisk II's `one_second()` does. The VIA
one-second interrupt that advances `Time` is not emulated. Until 5.0.1 the
menu-bar clock therefore stayed at the time the machine booted, although RTC
reads were correct. The write runs on the emulator thread, so it cannot race
guest code. In a Math-only A/B it cost 0.6% (9.533 → 9.475). CPU, Graphics
and Disk did not change.

## Measured and rejected

| Idea | Result |
|---|---|
| Threaded or chained handler dispatch | CPU 0.728, slower |
| Removing the frame-buffer store fence | No measurable change; the fence stays |
| 64-pixel tiles | No gain over 32 |
| A-line QuickDraw acceleration | Unsafe: Mac OS 8 replaces most QuickDraw traps and routes the ROM `Std*` procedures through private vectors; see [QUICKDRAW_REGRESSION.md](QUICKDRAW_REGRESSION.md) |
| Dispatch loop, 55 hottest handlers and the long-word thunks in the P4's 8 KB of tightly-coupled memory | CPU 0.360, Graphics 0.266: code in TCM runs far slower than code in cached internal RAM |
| Translated regions entered from profiler samples | Unsafe: a sampled PC can lie inside an instruction (see [Translated code](#translated-code), rule 1) |
| Inline guest-memory access in translated code, separate entry functions | 640 KB of code; Graphics 0.80 against 0.85 for 343 KB with out-of-line helpers |
| Hottest translated regions and a smaller handler set in internal RAM, chosen by profile | Graphics 0.925 against 0.932, and the 11 KB low internal heap was used up |
| Whole firmware from PSRAM (`CONFIG_SPIRAM_XIP_FROM_PSRAM`) | Not measured: needs the Arduino libraries rebuilt, which failed in the build tooling. Copying only the translated code gave the gain |

Earlier releases also rejected a PSRAM-resident RV32 translator, internal JIT
arenas (blocked by the P4's write/execute protection) and a threaded trace
cache. See the older sections of PERFORMANCE_REPORT.md.

## Maintenance checklist

- **Changing `gencpu.c`, `cpuemu.cpp` or the memory helpers with translated
  code in the build:** translate again, run `test/test_aot.py` and the host
  self-test on the real translation, flash the `esp32p4_aot_selftest` build
  and wait for `RESULT PASS`, run `tools/aot/check_blob.py` on the firmware
  ELF, then measure.

- **Changing `gencpu.c` or `iram_handlers.txt`:** regenerate, run
  `test_cpu_flags`, build, check internal SRAM after init, and run a
  three-run CPU and Graphics A/B.
- **Changing memory mapping, frame-buffer layout or depth handling:** check the
  thunk range tests and `video_dirty_limit`. Run Color QuickDraw at 1, 2, 4 and
  8 bits, then `PANEL VERIFY`.
- **Changing display code:** run `test_display_pipeline` (rotation, spans,
  indexed tiles), then `PANEL VERIFY` with rotation 0 and 180.
- **Using a different ROM:** trap dispatch disables itself if the dispatcher
  code differs. Confirm in a profile that dispatcher samples reappear rather
  than assuming it.
- **Before a release:** run the host tests and build all five profiles with
  PlatformIO Core 6.1.19 or newer. Then soak at least three full ratings with
  `PANEL VERIFY` after each, and check that the boot session did not change and
  the serial log has no crash markers.

## Known limits

- 5.0.1 was hardware-tested on a pre-v3 Tab5 only. The Waveshare and Rev3
  images share this code and build cleanly, but have not run it.
- IRAM headroom is smaller than in 5.0; see
  [Placement in internal RAM](#placement-in-internal-ram).
- Native QuickDraw acceleration remains disabled. A safe version would need
  hooks below Mac OS 8's patches, inside the ROM, for each ROM version.
