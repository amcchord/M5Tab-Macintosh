# Emulator performance design

This guide explains the interpreter and display fast paths added in 5.0.1:
how each one works, what it assumes, how it was measured and what to recheck
when changing it. [PERFORMANCE_REPORT.md](PERFORMANCE_REPORT.md) keeps the
build-by-build measurements. [AUTOMATION.md](AUTOMATION.md) documents the
benchmark and diagnostic commands.

## Results

Speedometer 4.02, **Tests > Performance Rating** (Quadra 605 = 1.0), all four
tests, on a Tab5 with ESP32-P4 revision 1.3 at 360 MHz running Mac OS 8:

| Score | 5.0 | 5.0.1 | Change |
|---|---:|---:|---:|
| CPU | 0.545 | 0.803–0.804 | +47% |
| Graphics | 0.242–0.244 | 0.495–0.497 | +104% |
| Disk | 1.02 | 1.029–1.033 | unchanged |
| Math | 6.68 | 9.43–9.48 | +42% |
| Performance Rating | 0.458 | 0.765–0.766 | +67% |

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

Earlier releases also rejected a PSRAM-resident RV32 translator, internal JIT
arenas (blocked by the P4's write/execute protection) and a threaded trace
cache. See the older sections of PERFORMANCE_REPORT.md.

## Maintenance checklist

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
