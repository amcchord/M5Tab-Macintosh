# M5Tab Macintosh performance optimization report

## Pass 3: interpreter, display, and AOT optimization — 2026-11-23/24

Measured on a Tab5 (ESP32-P4 rev 1.3 at 360 MHz, Mac OS 8 on `/Macintosh8.dsk`,
24 MB guest RAM, `esp32p4_pioarduino_debug` in `ota_1`, Speedometer 4.02 ratings
and Color QuickDraw tests):

| Metric | v5.4 interp baseline | Stage 1 (interp + display) | Stage 2a (ROM + sys RAM) | Stage 2b (+ Speedometer RAM) |
|---|---:|---:|---:|---:|
| CPU (3-run median) | 0.780 (0.777–0.781) | 0.840 (0.836–0.841) | 0.840 (0.833–0.848) | 2.204 (2.204–2.227) |
| Graphics (3-run median) | 0.451 (0.450–0.453) | 0.461 (0.460–0.464) | 1.002 (0.997–1.002) | 1.028 (1.008–1.029) |
| Full-suite PR (`all`) | 0.754 | 0.828 | 1.130 | 1.691 |
| Full-suite breakdown (CPU / Disk / Gfx / Math) | 0.735 / 1.793 / 0.455 / 9.316 | 0.837 / 2.775 / 0.463 / 10.001 | 0.840 / 1.973 / 1.008 / 10.804 | 2.230 / 1.820 / 1.021 / 12.091 |
| Color QuickDraw: Monochrome | 13.764 s (`0.502`) | 13.405 s (`0.515`) | 6.618 s (`1.044`) | 6.464 s (`1.069`) |
| Color QuickDraw: 8-bit Color | 22.689 s (`0.467`) | 22.004 s (`0.481`) | 11.599 s (`0.913`) | 11.134 s (`0.951`) |
| `PERF` CPU (`68k MIPS` / cyc / insns / IPC) | 5.946 / 60.5 / 42.3 / 0.699 | 6.365 / 56.6 / 39.2 / 0.693 | 7.252 / 49.6 / 27.1 / 0.546 | 19.039 / 18.9 / 16.2 / 0.858 |
| `PERF` Graphics (`68k MIPS` / cyc / insns / IPC) | 6.399 / 56.3 / 41.7 / 0.741 | 6.573 / 54.8 / 39.8 / 0.726 | 12.913 / 27.9 / 17.6 / 0.632 | 13.497 / 26.7 / 17.7 / 0.662 |
| Internal SRAM free after boot | 87,792 B | 88,312 B | 81,448 B | 81,140 B |

On the same `/Macintosh8.dsk` disk (with 24 MB guest RAM and `v5.4` system AOT
scoring `0.836` Graphics / `1.009` PR), Stage 1 raises open-source interpreter
Performance Rating by `+9.8%` (`0.754` to `0.828`, CPU `+7.7%`, Graphics
`+2.2%`), Stage 2a raises Graphics by `+122%` over the interpreter baseline
(`+19.9%` over `v5.4` system AOT) with `30.3 s` cold boot (`22.0 s` since
emulator start), and Stage 2b reaches `2.204` CPU (`+183%` over the interpreter
baseline, `+37.4%` over Pass 2 `full-23`'s `1.604`) and `1.691` PR (`+124%`
over baseline, `+26.7%` over `full-23`'s `1.335`) with `32.1 s` cold boot
(`23.8 s` since emulator start). Note that Pass 3 profiles ran with
`--profile-capacity 40000` (640 KB) because the default `200000` (3.2 MB)
sampler buffer exceeds free PSRAM when 24 MB guest RAM and ~0.6 MB of AOT
code/tables are both allocated.

### Optimization summary

- **Display pipeline & build polish** (`src/board/panel_surface.h`, `src/basilisk/video_esp32.cpp`, `main/CMakeLists.txt`, `scripts/pre_build.py`, `src/basilisk/uae_cpu/fpu/fpu_ieee.cpp`, `tools/mac_control.py`):
  - Replaced per-pixel rotated indexed writes with 32-bit aligned burst tile writers (`ScaleTraits`, `emit_u32_row`, `emit_u16_row`, `writeSingleRowIndexed`, chunked stack buffering) eliminating multiply/byte-swap library calls on inner loops; cached palette `pairs[]` across frames via `pairs_dirty` rather than rebuilding every frame.
  - Replaced byte-by-byte dirty-span clearing in `VideoRefresh` with row-wise 32-bit word OR/exchange scans over `video_dirty_chunks` (`__atomic_exchange_n`).
  - Added ESP-IDF `main/CMakeLists.txt` warning/path cleanup, `fpu_ieee.cpp` inline portability fix, and higher USB-JTAG screenshot block retry resilience in `tools/mac_control.py`.
- **68k interpreter fast paths** (`src/basilisk/uae_cpu/{m68k.h,newcpu.h,newcpu.cpp,memory.h,memory.cpp}`, `tools/cpu_gen/gencpu.c`, `cpuemu.cpp`, `compemu.h`):
  - Packed `c`/`z`/`n`/`v` into a 32-bit `regflags.cznv` word in `flag_struct` with `CLEAR_CZNV`, `SET_CZNV_ZN`, and `SET_CZNV_ALL` single-store updates plus packed `cctrue()` checks; added `m68k_dreg_w_set`, `m68k_dreg_b_set`, and `get_iword_s16` in `gencpu.c`.
  - Split A-line dispatch into `IRAM_ATTR` handlers `op_aline_os` (`0xA000..0xA7FF`) and `op_aline_toolbox` (`0xA800..0xAFFF`) with shared `native_trap_verify_vector` checks, moved `op_4e75_0_ff` (leaf `RTS`) into IRAM without frame setup, and added pipelined `mem_setpc_slow_thunk` / `mem_slow_setpc_xlate` and improved `mem_slow_*` thunks.
- **AOT runtime & translator** (`src/basilisk/aot/{aot_runtime.h,aot_runtime.cpp,aot_codegen.h,aot_selftest.cpp}`, `tools/aot/aot_translate.py`, `scripts/aot_build_flags.py`, `src/basilisk/automation.cpp`):
  - **4 KB page bitmap & narrowed `CPUSHA` invalidation:** `aot_page_bitmap` (8,192 pages covering 32 MB) makes `AotCodeChangedRange` / `AotCodeChangedGuestRange` skip unrelated writes in O(1), while `AotHandleCpusha` narrows Quadra 650 `_HLock` flushes to the locked handle block before falling back to `AotCodeChanged()`. On the full Stage 2b benchmark soak, `AOT STATS` recorded `cpusha=1829364/4289` (99.77% narrowed), `rskip=1848076`, `rinv=3888`, `rejects=0`, and `3,203,266,312` of `3,707,514,216` total 68k instructions (86.4%) executed in translated regions.
  - **Codegen & translator optimizations:** backward-dataflow flag liveness and dead-flag elimination (`FlagEffect`, `compute_region_liveness`), batched `aot_load_cznv` / `aot_store_cznv`, single-entry specialization (skipping `AotEntryInfo` lookup when a region has one entry), direct intra-module / hot-edge block chaining, hot-inline region selection (`13` regions / `12,288 B` in Stage 2), `aot_setpc_thunk`, `-fno-stack-protector` in `scripts/aot_build_flags.py`, and PSRAM-resident `aot_entry_infos_runtime`.

### Verification and safety

- **Differential & AOT checks:**
  - Host `aot_diff --trials 12`: Stage 2a `51,173` runs, `193,464,986` instructions compared, `0` failures (`41` skipped); Stage 2b `52,407` runs, `194,766,210` instructions compared, `0` failures (`42` skipped).
  - On-device `esp32p4_aot_selftest` (Stage 2a): `RESULT PASS runs=26894 failures=0 skipped=56 instructions=95667839`.
  - `tools/aot/check_blob.py`: `0` issues on both Stage 2a (`573,964 B`, `1,282` funcs, `169,208` insns) and Stage 2b (`598,540 B`, `1,344` funcs, `176,665` insns).
- **Host unit & display suites:** `test_cpu_flags` (`43,308,858` flag and helper cases PASS), `bench_display_damage` (`1bpp outline 2.05 us`, `1bpp dense 73.19 us`, `8bpp outline 1.44 us`, `8bpp dense 79.97 us`, bit-exact panel hashes), and `unittest discover -s test -p 'test_*.py'`.
- **On-device panel & stability:** `PANEL VERIFY` found `0` mismatches out of `921,600` pixels after Baseline A, Stage 1, Stage 2a, and Stage 2b runs (in both 1-bit monochrome and 256-color modes); all 5 open-source PlatformIO environments (`esp32p4_pioarduino`, `esp32p4_pioarduino_debug`, `esp32p4_pioarduino_rev3`, `waveshare_p4_101`, `waveshare_p4_101_rev3`) compile cleanly without `aot_rom_generated.cpp`.

## Translated code pass — 2026-09-27/28

Branch `claude/perf-pass-2`, based on v5.0.1 `94af0b8`. The goal was every
Speedometer 4.02 Performance Rating score near or above 1.0, without a JIT.
[EMULATOR_PERFORMANCE.md](EMULATOR_PERFORMANCE.md#translated-code) explains
the design; [tools/aot/README.md](tools/aot/README.md) is the procedure.
Measured on the Tab5 (ESP32-P4 rev 1.3, 360 MHz, Mac OS 8, 16 MB guest RAM).

| Metric | v5.0.1 | ROM and system code translated | Also Speedometer's own code |
|---|---:|---:|---:|
| CPU | 0.804 | 0.804 | 1.603–1.604 |
| Graphics | 0.490–0.494 | 1.023–1.025 | 1.012–1.014 |
| Disk | 1.031 | 1.027–1.028 | 1.024–1.030 |
| Math | 9.42 | 10.48 | 11.11–11.12 |
| PR | 0.764 | 1.006 | 1.334–1.335 |

v5.0.1 was re-measured from this branch's base. The middle column is what a
program that was never profiled gets. The CPU test is a sort loop in
Speedometer's own code and runs no ROM or system code; it moves only when
that loop is translated, as in the last column.

### Where the time went

The profile of 5.0.1 is flat: about 6,000 hot 68k addresses, no handler above
3.6%, 42 host instructions and 60 to 64 cycles for every 68k instruction.
Two thirds of that is work the interpreter repeats each time an instruction
runs: dispatch, decoding, every condition code, the PC. For code whose bytes
are known beforehand it can be done at build time.

Measured with `--profile-seconds`, which keeps the idle desktop out of the
counters, the Graphics test went from 6.1 to 11.4 68k MIPS, from 41 to 18
host instructions and from 59 to 32 cycles per 68k instruction. Counted from
start-up through the boot, the ratings and the Color tests, 84% of all 68k
instructions ran in translated code with ROM and system code translated and
90% with Speedometer's own code as well.

### Steps, in the order they were measured

| Build | Change | CPU | Graphics |
|---|---|---:|---:|
| base-00 | v5.0.1 source | 0.804 | 0.490–0.494 |
| aot-06 | Hot ROM code translated at build time, entered through patched opcodes | 0.805 | 0.838 |
| aot-11 | Hot system code in RAM translated as position-independent modules | 0.805 | 0.804 |
| aot-13 | Compact code: memory helpers out of line, entries merged into regions, shared exits (640 KB to 343 KB) | 0.804 | 0.847 |
| aot-14 | Full-format addressing translated; instruction lengths inferred from the counts, which frees entry points | 0.805 | 0.932 |
| aot-17 | Lower thresholds, regions from two instructions, cheaper entry number | 0.805 | 0.941 |
| aot-18 | Guest registers in locals in regions that run 16 or more instructions per entry | 0.805 | 0.948 |
| aot-20 | Translated code copied to PSRAM at start-up and run there | 0.806 | 0.992 |
| aot-21 | ROM and frame-buffer accesses finished inside the memory helpers | 0.806 | 1.020 |
| sys-22 | Profile extended with the Color tests and Finder use | 0.804 | 1.023–1.025 |
| full-23 | sys-22 with the application code of the profile translated too | 1.603–1.604 | 1.012–1.014 |

Every build from aot-06 to sys-22 leaves Speedometer's own code to the
interpreter except aot-10 (CPU 2.000, Graphics 0.798), which showed what the
CPU test is. full-23 translates the same loop and reaches 1.60, not 2.00;
why the later translator does less well on it was not investigated.

Measured and rejected: the dispatch loop and hottest handlers in the P4's
tightly-coupled memory (CPU 0.360); profile-guided placement of translated
regions in internal RAM (Graphics 0.925 against 0.932, and the low internal
heap was used up); running the whole firmware from PSRAM through a rebuilt
SDK (the rebuild failed in the build tooling; copying only the translated
code is what aot-20 does).

### Safety

- **Same semantics by construction.** Region bodies are the interpreter's
  handler bodies. What the translator adds (branches, entries, exits,
  register and flag locals) is what the differential test is for.
- **Differential test.** Every entry point, from random states, against the
  interpreter: 27,526 runs and 92.1 million instructions compared on the
  device for the build that is flashed (57 trials skipped because they
  overwrote the code under test), the
  same test on the host for every translation, and `test/test_aot.py` on
  generated code for every handler the translator accepts. No mismatch
  remains. The ones found on the way were a translator bug (register use of
  `MOVE16` and `CAS2` when caching registers), signed overflow in the
  handlers that the compiler exploited once operands were constants
  (`-fwrapv`), and four cases where the test itself compared unlike things.
- **Byte checks.** A ROM region is installed only if the ROM holds the bytes
  it was translated from. A RAM module is installed only where its code is
  executing and matches byte for byte, and is compared again after every
  guest cache flush.
- **Display.** `PANEL VERIFY` found 0 of 921,600 pixels wrong after every
  set of runs. Color QuickDraw completed in monochrome and 8-bit colour.
- **No boot regression.** 49.6 to 49.7 s to the Finder against 49.5 s.

### Limits

- The translation is tied to the Quadra 650 ROM and to the Mac OS 8 system
  on the test disk. Anything else fails the byte checks and is interpreted at
  5.0.1 speed.
- `aot_rom_generated.cpp` is derived from Apple's ROM and system software. It
  is not in the repository, and a build without it interprets everything.
- Guest code that reads instructions as data can see an entry opcode.
- Only the Tab5 ran this. The Waveshare boards build the same code.
- Internal RAM is unchanged from 5.0.1. PSRAM use grows by the size of the
  translated code, 0.7 MB.


## v5.0.1 interpreter and display pass — 2026-09-27

Released as v5.0.1 from branch `claude/speedometer-perf`, based on v5.0
`1468a91`. [EMULATOR_PERFORMANCE.md](EMULATOR_PERFORMANCE.md) explains how
each change works and how to maintain it. Measured on the Tab5 (ESP32-P4
rev 1.3, 360 MHz, Mac OS 8, Speedometer 4.02 **Tests > Performance Rating**,
all four tests, 1 iteration). Native QuickDraw stays disabled.

| Metric | v5.0 | v5.0.1 | Change |
|---|---:|---:|---:|
| CPU | 0.545 | 0.803–0.804 | **+47%** |
| Graphics | 0.244 | 0.495–0.497 | **+104%** |
| Disk | 1.02 | 1.029–1.033 | unchanged |
| Math | 6.68 | 9.43–9.48 | +42% |
| PR | 0.458 | 0.765–0.766 | **+67%** |

These are the final code: three soak ratings and one unattended runner rating
on clock-24, plus the release-gate rating on the 5.0.1 image. Five consecutive ratings on final-23, before the clock fix,
also varied by at most 0.001 in CPU and Graphics. Color QuickDraw completed at
every depth (mono 0.549, 2-bit 0.525, 4-bit 0.535, 8-bit 0.509).

### Where the time went

The `PERF` sampler (a timer interrupt on either core recording host PC, return
address, 68k PC/opcode and CPU cache counters) showed three dominant costs:

1. **Guest memory access.** Every 68k load/store called through bank tables
   with generic byte swapping, and GCC's bswap pass turned byte-wise loads
   back into a word load plus a long swap sequence (the P4 has no Zbb).
2. **Toolbox trap dispatch.** ~11% of sampled time sat in the ROM's A-line
   dispatcher.
3. **Display rendering on core 0.** It competes with the emulator for the
   shared L1D/L2 and PSRAM. The Graphics test runs in 1-bit mode, where the
   old 64-pixel dirty granularity and cached scanout writes were most wasteful.

### Changes, in the order they paid off

| Build | Change | Measured |
|---|---|---|
| inline-01 | Inline RAM fast path; byte-wise big-endian access kept opaque to GCC | CPU 0.645 |
| thunk-02 | ROM/framebuffer/MMIO accesses go to small out-of-line IRAM thunks, so handlers stay compact | CPU 0.773 |
| fbpath-10 | Framebuffer stores handled inside the thunk; per-span dirty marking | PR 0.548 |
| packed-15 | Depth-aware 32-pixel dirty spans, 32-pixel tiles, packed indexed tile writer (palette pairs, no per-tile LUT) | Graphics 0.338, PR 0.623 |
| iram-16 | Native Toolbox trap dispatch when the line-A vector still matches the ROM dispatcher; 295 hot handlers in IRAM | Graphics 0.38 |
| flags-18 | Add/sub/cmp flags from shifted operands (fewer instructions per ALU op) | CPU 0.804 |
| scanout-21 | Tiles written through the non-cacheable PSRAM alias; no cache writeback per tile | Graphics 0.492 |
| final-23 | Hot globals in `.sdata` (gp-relative), cleanup | CPU 0.803, Graphics 0.497, PR 0.765 |

iram-16 (trap dispatch plus IRAM placement) raised 68k throughput ~10%
(4.85→5.34 MIPS in the CPU profile) but left the CPU score flat; its benefit
appeared in Graphics.

Measured and rejected: threaded/chained dispatch (CPU 0.728), removing the
framebuffer store fence (neutral), 64-pixel tiles (no gain). The
logical-operation flag rewrite was also neutral; it was kept so all generated
flags follow one form. Halving the display refresh was a diagnostic only: it raised
Graphics ~15%, confirming that core 0 competes for the caches. Native A-line
QuickDraw is unsafe on Mac OS 8, which patches most QuickDraw traps and the ROM
`Std*` procedures; a correct accelerator would need ROM-internal hooks and was
shelved.

### Safety

- The trap fast path runs only while the code at line-A vector `$28` matches
  the ROM dispatcher's 66-word signature (rechecked whenever the vector
  changes), in supervisor mode on the interrupt stack without trace. Anything
  else, including a patched dispatcher, takes the original ROM path.
- `PANEL VERIFY` decodes the guest framebuffer through the live palette and
  compares every panel pixel: 0 of 921,600 mismatched after every soak run.
- Host tests add `test_cpu_flags`, which checks the new ADD/SUB/CMP and
  logical-operation flags against the classic UAE formulas (exhaustive for
  bytes, structured and random for words and longs), and 32-pixel span,
  rotation and indexed-tile cases in `test_display_pipeline`.
- Hot handlers and thunks add ~46 KB of IRAM. On the Tab5, internal SRAM free
  after init fell from 231 KB (v5.0) to 177 KB; `cpufunctbl` and `mem_banks`
  already lived in PSRAM. All five profiles build, but the Waveshare boards
  have not run this branch.
- `emul_op.cpp` now refreshes the guest `Time` global once a second, as
  upstream Basilisk II does. Previously the Mac clock stayed at its boot
  value, a pre-existing v5.0 bug found while benchmarking. In a Math-only A/B
  (3 runs each) it cost 0.6% (9.533 → 9.475). CPU, Graphics and Disk did not
  change. Math also became steadier: 9.45–9.48 in every procedure, against
  9.30–9.68 before.

### Tools

- `tools/speedometer_benchmark.py --suite rating` runs the Performance Rating
  unattended, including the splash and registration prompts, and finds the
  desktop alias by its OCR label (`test/test_speedometer_benchmark.py`).
- `--profile HZ` plus `tools/perf_report.py` give per-function, per-opcode and
  per-68k-page profiles. See AUTOMATION.md.

## Earlier QuickDraw acceleration work

The results below are historical measurements with native QuickDraw enabled.
The current development default disables that acceleration after a hardware
A/B test isolated a Finder border regression. See
[QUICKDRAW_REGRESSION.md](QUICKDRAW_REGRESSION.md). These graphics scores do
not describe the corrected default, whose throughput has not been measured.

### Result

The optimization target was met in Speedometer 4.02's headline Performance
Rating workload:

| Metric | Baseline | Final release run | Change |
|---|---:|---:|---:|
| CPU | 0.553 | 0.542 | -2.0% |
| Graphics | 0.310 | 0.534 | **+72.3%** |
| Disk | 1.738 | 1.774 | +2.1% |
| Math | 6.448 | 6.400 | -0.7% |

The required Graphics threshold was 0.465. The clean, profiler-free firmware
reached 0.534. Speedometer displayed that value after the Graphics iteration
completed in the full run; subsequent tests do not change a completed component
score.

Two focused workloads provided faster feedback while developing the same paths:

| Focused test | Before | Best measured | Change |
|---|---:|---:|---:|
| Color QuickDraw, monochrome | 0.336 | 0.601 | +78.9% |
| Color QuickDraw, 8-bit | 0.307 | 0.592 | +92.8% |

The final build has the trap profiler disabled and the unsuccessful native
`IsLayer` experiment disabled.

### Benchmark automation

`tools/speedometer_benchmark.py` turns the existing `@B2` control protocol into
a repeatable benchmark loop. It:

- keeps one serial connection open across boot, launch, control, and capture;
- waits for the Finder desktop and launches Speedometer with an ADB chord;
- dismisses the splash screen with a human-duration held click;
- dismisses both Speedometer registration variants;
- configures focused Color tests by reading the actual checkbox pixels, so the
  selection is absolute rather than dependent on persisted state;
- handles the Run All confirmation and disk-selection dialogs;
- captures timestamped PNG evidence and OCRs results;
- ignores live subtotal windows until the explicit tests-complete dialog;
- emits machine-readable scoring/profile JSON beside each run; and
- treats acknowledgement loss carefully so benchmark shortcuts and Command-W
  are never repeated against the next window.

The host screenshot implementation uses CRC-checked compressed logical
framebuffer snapshots. Panel rendering pauses briefly during capture so the
display task does not compete for PSRAM bandwidth. Firmware input state is
idempotent and recoverable with `RELEASE_ALL`.

The complete protocol and runner commands are documented in `AUTOMATION.md`.

### Profiling and diagnosis

A compile-time A-line trap histogram and QuickDraw-specific counters were added
for development builds. Short paged records keep responses reliable on the
shared diagnostic/automation serial channel. Static analysis of Speedometer's
68k Color test code confirmed the timed workload: large `CopyBits` transfers,
whole-window `ScrollRect` operations, repeated rectangle/oval painting, and
thousands of `MoveTo`/`LineTo` calls.

The important discovery was that headline Graphics temporarily switches the
screen to monochrome. The first native implementation accelerated the separate
8-bit Color workload dramatically but did not improve headline Graphics.
Adding packed 1-bit operations, then replacing per-pixel updates with byte-wide
bitblits and fills, addressed the actual scored path.

### Native QuickDraw fast paths

`quickdraw_accel.cpp` intercepts selected A-line traps before the System 7
QuickDraw implementation. It resolves the current `CGrafPort` through the
process's A5 QuickDraw globals and validates all guest pointers and drawing
state before writing anything.

Implemented paths:

- `CopyBits`: 1-bit and 8-bit `srcCopy`, equal-sized rectangles, overlap-safe
  copy direction, current-port visibility/clipping, and masked edge bytes for
  unaligned monochrome spans.
- `ScrollRect`: exact source/destination membership through complex regions,
  overlap-safe movement, 8x8 background-pattern exposure fill, and update-region
  bounds.
- `FrameRect`, `PaintRect`, `FrameOval`, and `PaintOval`: pen pattern, pen size,
  indexed color resolution, integer scanline ellipses, and complex clipping.
- `MoveTo` and `LineTo`: monochrome-only fast paths for the scored workload,
  including byte-wide horizontal/vertical fills, Bresenham diagonals, pen
  patterns, pen sizes, and clipping.

Complex QuickDraw regions are parsed as XOR inversion-boundary streams and
intersected scanline-by-scanline with the port rectangle, visible region, and
clip region. Framebuffer writes continue to mark the appropriate display range
dirty. Unsupported modes, scaled copies, masks, custom `GrafProcs`, picture /
region / polygon recording, unexpected pixel depths, malformed regions, or
unmapped memory fall through to the untouched System handler before guest state
is changed.

### Other work evaluated

The following ideas were measured and rejected or left disabled:

- Raising the ESP32-P4 to 400 MHz was not supported reliably on this rev-1.3
  device; 360 MHz remains the stable setting.
- A PSRAM-resident RV32 translator regressed Speedometer, while internal JIT
  arenas could not satisfy the P4's locked write/execute protection.
- Compact opcode dispatch formats regressed throughput or destabilized the
  internal heap.
- A threaded trace cache regressed throughput because validation overhead
  exceeded the saved dispatch work.
- Disabling dirty tracking, lowering refresh cadence, and forcing a 1-bit
  maximum display depth established that panel rendering was not the dominant
  scored bottleneck.
- Native System `BlockMove` was safe but changed CPU by less than 1%.
- Native Layer Manager `IsLayer` did not improve the score and is disabled.

These negative results are reflected in the release flags: interpreter mode,
direct dispatch, no trace cache, 360 MHz, 8-bit display support, and no trap
profiler.

### Verification and evidence

- Host automation unit tests: 7 passed.
- Release firmware: PlatformIO build and upload succeeded.
- Final image size: approximately 3.10 MiB; reported RAM use 17.3%.
- Focused release Color test: 8-bit score 0.559 with profiler disabled.
- Profiled monochrome test: score 0.601, 3,903/4,486 `LineTo` calls and
  3,935/4,657 `MoveTo` calls handled natively; unsupported calls fell back.
- Full release run: CPU 0.542, Graphics 0.534, Disk 1.774, Math 6.400.

Evidence is stored under `artifacts/performance-runs/`, notably:

- `20260812-070729-final-full-native-quickdraw-r59/sample-02.png` — full
  Performance Rating window after Graphics completed, showing 0.534.
- `20260812-070152-native-mono-lines-r58/color-result.png` — focused
  monochrome score of 0.601 and visually intact result.
- `20260812-061326-final-clean-color-r52/color-result.png` — profiler-free
  8-bit Color score of 0.559.

One final-run acknowledgement was lost while closing stacked detail windows;
the command had already executed, and its retry closed the final aggregate
window before JSON capture. The full-run screenshot taken immediately after
the Graphics iteration preserves the accepted 0.534 score. The runner was then
changed to send each Command-W exactly once, so a lost acknowledgement cannot
repeat the destructive UI action.
