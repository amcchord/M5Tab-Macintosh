# M5Tab Macintosh performance optimization report

## Result

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

## Benchmark automation

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

## Profiling and diagnosis

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

## Native QuickDraw fast paths

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

## Other work evaluated

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

## Verification and evidence

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
