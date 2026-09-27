# M5Tab Macintosh performance optimization report

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
