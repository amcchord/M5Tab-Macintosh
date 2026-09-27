# Perf Worklog

Start: 2026-02-03

Goal: Identify and implement 30%+ speedup for IPS or video rendering, with instrumentation that guides micro-optimizations.

## CPU Opcode Sampling

### 2026-02-03

Changes:
- Wired the existing CPU profiler into the emulator loop.
- Enabled opcode sampling and periodic reporting.

Code:
- `src/basilisk/uae_cpu/newcpu.cpp`
  - Added `PROF_OPCODE_SAMPLE(opcode)` in the hot CPU loop.
  - Uses `prof_record_opcode()` (sampled) to populate top-opcode stats.
- `src/basilisk/main_esp32.cpp`
  - Added `cpu_profiler_init(...)` during setup.
  - Added `cpu_profiler_report()` in `basilisk_loop()`.
  - Default enable macro: `CPU_PROFILER_DEFAULT_ENABLED` (defaults to `1`, can override via build flags).

Notes:
- The profiler already implements a sampled top-opcode list (32 slots) and prints the top 10 every 5 seconds when enabled.
- Overhead is expected (~5-10%). This is acceptable for profiling and can be disabled once optimizations are done.

Next:
- Capture first 60 seconds of boot with profiler enabled.
- Extract top opcodes and map them to their handlers in `src/basilisk/uae_cpu/generated/cpuemu.cpp`.
- Identify hot instructions with heavy memory or flag logic and micro-optimize them.

## Video Rendering

### 2026-02-03 (initial review)

Observations:
- Tile-based rendering with dirty tracking is already active.
- DMA-based display writes are used; streaming path is disabled.
- Perf counters exist for detect/render timing.

Hypotheses for wins:
- Reduce per-tile overhead in `renderTile*` paths (tighten loops, eliminate redundant bounds checks).
- Explore wider DMA chunking or batching to reduce `setAddrWindow` calls.
- Evaluate partial update policy thresholds for full vs partial refresh.
- Consider direct panel writes bypassing some M5GFX wrappers if they add overhead.

Next:
- Add a light breakdown of render cost (tile conversion vs DMA wait) if needed.
- Compare alternative tile sizes (larger tiles = fewer calls, smaller tiles = less overdraw).
- Investigate direct screen writes vs current library paths.

## Benchmarks

- Pending: CPU opcode sampling run (first 60 seconds from boot).
- Pending: Video perf run with the same window for apples-to-apples.

## Build/Run Notes

- Profiler default enabled: override with `-DCPU_PROFILER_DEFAULT_ENABLED=0` to disable.
- Logging target: capture serial for first 60 seconds from boot.


## WiFi Disabled for Perf Testing

### 2026-02-03

Reason:
- Device was auto-connecting to an SSID that is not available, which adds delays and noise to boot logs during perf runs.

Change:
- Added compile-time switch to skip WiFi auto-connect during boot GUI.

Code:
- `src/basilisk/boot_gui.cpp`
  - `BOOTGUI_DISABLE_WIFI_AUTOCONNECT` (default `1`).
  - If auto-connect was enabled in settings, it is now skipped with a log message.

Notes:
- Re-enable by setting `-DBOOTGUI_DISABLE_WIFI_AUTOCONNECT=0` in build flags.

### 2026-02-03 (SSID not available handling)

Change:
- If auto-connect is enabled and the saved SSID is not found, auto-connect is disabled and settings are saved to stop repeated attempts.

Code:
- `src/basilisk/boot_gui.cpp`
  - On `WL_NO_SSID_AVAIL`, set `wifi_auto_connect = false`, call `saveSettings()`, and log.


## Opcode Sampling (lightweight ring buffer)

### 2026-02-03

Observation:
- Enabling the existing `cpu_profiler` caused a repeatable FreeRTOS assert on Core 1: `assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))` immediately after CPU start.
- This prevented stable sampling runs.

Change:
- Replaced the hot-loop opcode sampling with a lightweight ring-buffer sampler that does not use the CPU profiler.

Code:
- `src/basilisk/opcode_sampler.h`
- `src/basilisk/opcode_sampler.cpp`
  - Samples every `OPCODE_SAMPLE_RATE` instructions.
  - Stores samples into a lock-free ring buffer.
  - Aggregates and prints Top 10 opcodes every 5 seconds in the main loop.
- `src/basilisk/uae_cpu/newcpu.cpp`
  - Replaced `PROF_OPCODE_SAMPLE` with `OPCODE_SAMPLE` using the new sampler.
- `src/basilisk/main_esp32.cpp`
  - Calls `opcode_sampler_report()` in `basilisk_loop()`.
  - Removed CPU profiler init/report calls to avoid the assert.

Next:
- Run a full 60-second capture and extract the top opcodes from the new sampler output.
- Map those opcodes to handlers in `src/basilisk/uae_cpu/generated/cpuemu.cpp` for micro-optimizations.

## Video Rendering: Row-Strip DMA Coalescing

### 2026-02-05

Goal:
- Reduce per-tile DMA overhead by coalescing contiguous dirty tiles into wider horizontal strips.
- This should lower `setAddrWindow` + `writePixelsDMA` call count, especially during large or full-screen updates.

Changes:
- Added optional row-strip buffers (double-buffered) and a strip-render path.
- Rendered tiles directly into a strided strip buffer, then pushed the entire strip with one DMA call.
- Kept per-tile DMA path as a fallback if row buffers cannot be allocated.

Code:
- `src/basilisk/video_esp32.cpp`
  - New config: `VIDEO_USE_ROW_STRIPS` (default `1`).
  - New buffers: `row_buffer_psram` + `row_buffer_enabled`.
  - New render helpers: `renderTileFromSnapshotStrided`, `renderTileFromFramebuffer8Strided`.
  - `renderAndPushDirtyTiles()` updated to coalesce dirty tiles into horizontal strips.
  - VideoInit allocates row-strip buffers and logs which memory they land in.

Notes:
- Strip buffer size: `DISPLAY_WIDTH * tile_pixel_height` pixels (1280 * 80) = 204,800 bytes.
- Double-buffered strip buffers total ~400KB; should fit in PSRAM.
- Expect biggest wins when many tiles in a row are dirty (e.g., large window moves, full-screen redraws).

Next:
- Upload and capture a 60s boot log to compare `[VIDEO PERF] avg render` vs baseline.
- If improvement is strong, consider increasing tile size or tuning row-strip vs per-tile threshold.

## CPU Loop Tuning: Larger Batch + Tick Quantum

### 2026-02-05

Goal:
- Reduce overhead from tick checks and special-flag polling by running larger instruction batches per loop.
- Decrease frequency of `basilisk_loop()` calls while still preserving overall timing via catch-up.

Changes:
- Increased `EXEC_BATCH_SIZE` in `newcpu.cpp` from 4096 to 8192.
- Increased `emulated_ticks_quantum` in `main_esp32.cpp` from 320000 to 640000.

Code:
- `src/basilisk/uae_cpu/newcpu.cpp`
- `src/basilisk/main_esp32.cpp`

Next:
- Build + upload and capture a 60s boot log.
- Compare IPS and video metrics to baseline row-strip off build.

## Video Rendering: Larger Tiles (160x40)

### 2026-02-05

Goal:
- Reduce per-tile overhead by halving the number of tiles per frame (72 -> 36).

Changes:
- Increased `TILE_WIDTH` from 80 to 160 while keeping `TILE_HEIGHT` at 40.
- Tile grid now 4x9 (36 tiles total).

Code:
- `src/basilisk/video_esp32.cpp`

Next:
- Build + upload and capture a 60s boot log.
- Compare `[VIDEO PERF] avg render` vs baseline (80x40 tiles) for improvement.

### 2026-02-05 (Result)

- 160x40 tile size increased average render time vs 80x40 (~30% worse in first 60s sample).
- Reverted to 80x40 tiles.

### 2026-02-05 (Follow-up)

- Increased `EXEC_BATCH_SIZE` to 16384.
- Increased `emulated_ticks_quantum` to 1280000.
- Objective: further reduce overhead; validate IPS/video impact.

### 2026-02-05 (Result)

- `EXEC_BATCH_SIZE=16384` / `quantum=1.28M` did not improve average MIPS and worsened render time.

## Video Rendering: DSI Direct Framebuffer Path

### 2026-02-05

Goal:
- Bypass `M5.Display.writePixelsDMA()` on ESP32-P4 DSI and write directly into the DSI framebuffer.
- Flush cache via `panel->display(...)` for just the dirty region.

Changes:
- Added DSI detection and framebuffer pointer/stride capture from `Panel_DSI`.
- Added a DSI direct path that converts tiles to RGB565 (nonswapped) and blits them into the framebuffer.
- Added internal-rotation-aware blit logic to match M5GFX coordinate transforms.

Code:
- `src/basilisk/video_esp32.cpp`

Result (first ~60s from boot, 12 samples):
- Baseline log: `/tmp/pio_perf_nosampler_batch8192.log`
  - Avg render: ~862us
  - Avg IPS: ~1.533 MIPS
- DSI direct log: `/tmp/pio_perf_dsi_direct_60s.log`
  - Avg render: ~374us
  - Avg IPS: ~1.537 MIPS

Net:
- Video render time improved by ~56% vs baseline (substantial win).
- IPS unchanged (as expected; CPU core on separate core).

Notes:
- DSI framebuffer reported: `panel=720x1280`, `display=1280x720`, `internal rotation=3`.
- If any orientation glitches appear on hardware, we can adjust the rotation mapping in `dsi_blit_tile(...)`.

## Video Rendering: Frame Skipping (Whole-Frame) + Dirty Merge

### 2026-02-05

Goal:
- Avoid long render bursts during heavy UI updates (dragging windows, scrolling).
- Let the system “skip frames” at a whole-frame level (no partial tile chunking).

Changes:
- Disabled tile-level render budgeting by default (`VIDEO_RENDER_BUDGET_US=0`).
- Added whole-frame skip policy for heavy redraws:
  - `VIDEO_FRAME_MIN_MS` (normal) and `VIDEO_FRAME_MIN_HEAVY_MS` (heavy redraws).
  - `VIDEO_HEAVY_DIRTY_TILES` threshold to select the heavy interval.
- Dirty tiles are now merged (`dirty_tiles |= write_dirty_tiles`) instead of replaced.
- Rendered tiles explicitly clear their dirty bit; unrendered tiles persist into the next frame.

Code:
- `src/basilisk/video_esp32.cpp`

Next:
- Subjective test: verify that UI feels less “stuck” during heavy redraws without chunked tile updates.
- Reverted to `EXEC_BATCH_SIZE=8192`, `quantum=640k`.

## Video vs CPU Tradeoff: cpufunctbl in PSRAM

### 2026-02-05

Goal:
- Free internal SRAM for the Mac frame buffer (and tile buffers) to improve video render speed.

Changes:
- Added `CPUFUNCTBL_FORCE_PSRAM` to allocate `cpufunctbl` in PSRAM, freeing internal SRAM.
- This should allow the Mac frame buffer allocation to land in internal SRAM (faster read/write).

Code:
- `src/basilisk/uae_cpu/basilisk_glue.cpp`
- `platformio.ini` (`-DCPUFUNCTBL_FORCE_PSRAM=1`)

Next:
- Build + upload and capture a 60s boot log.
- Compare video render times vs baseline (sampler off, batch8192/quantum640k).

## CPU Loop: Throttled taskYIELD

### 2026-02-05

Goal:
- Reduce context-switch overhead from yielding every `basilisk_loop()`.

Change:
- Added `BASILISK_YIELD_STRIDE` (default 16) to yield every N loop iterations instead of every loop.

Code:
- `src/basilisk/main_esp32.cpp`

Next:
- Build + upload and capture a 60s boot log to measure IPS/video impact.

### 2026-02-05 (Result)

- Forcing `cpufunctbl` into PSRAM allowed the Mac frame buffer to allocate in internal SRAM, but overall video render time and MIPS both regressed.
- Reverted to internal `cpufunctbl` (default).

### 2026-02-05 (Result)

- Throttling `taskYIELD()` (every 16 loops) slightly reduced MIPS and worsened render time.
- Reverted to yielding every loop.

## Opcode Sampling (Hot Opcode List)

### 2026-02-05

Run:
- Captured 60s boot log with opcode sampler enabled.
- Log: `/tmp/pio_perf_opcode_sampler_boot60s.log`

Top opcodes aggregated across samples:
- `4E75` (RTS)
- `B0B8` (CMP.L (xxx).W,Dn)
- `6EFA` (BGT short)
- `6704` (BEQ short)
- `642C` (BCC short)
- `60F0` (BRA short)
- `B3D6`
- `D3C1`
- `4A11`
- `2229`
- Plus frequent DBcc variants (`51C9`, `51CC`, `57CC`) and `MOVEQ` (`7000`)

Notes:
- Branch/DBcc opcodes dominate samples, suggesting branch handling overhead is significant.
- The sampler ring buffer dropped samples during heavy periods, so counts are approximate.

## CPU Fast-Path Attempt (Branch/DBcc/Link/Unlk)

### 2026-02-05

Goal:
- Avoid function-pointer dispatch for extremely hot branch and frame-setup opcodes.

Changes:
- Added optional fast-path handlers in `newcpu.cpp` for:
  - Short Bcc/BRA (existing)
  - DBcc (decrement-and-branch)
  - LINK/UNLK
  - RTS/NOP/MOVEQ (existing)
- Enabled via `FAST_OPCODE_PATH=1` in build flags.

Result:
- Perf regressed. 60s boot log (`/tmp/pio_perf_fastpath_boot60s.log`) averaged:
  - IPS: ~1.43 MIPS (vs ~1.92 MIPS baseline with sampler off)
  - Video render avg: ~0.78 ms (roughly unchanged)
- Conclusion: The per-opcode fast-path checks add more overhead than they remove on this target.

Action:
- Disabled `FAST_OPCODE_PATH` (kept the code gated for future experiments).

## Video: Direct Bus DMA Writes (Bypass M5GFX Pixelcopy)

### 2026-02-05

Goal:
- Reduce video render overhead by bypassing M5GFX `writePixelsDMA` (which copies
  into an internal DMA buffer via `pixelcopy_t`), and instead write our already
  swap565-formatted buffers directly to the bus DMA.

Changes:
- Added a direct-bus write path in `renderAndPushDirtyTiles()`:
  - Use `M5.Display.getPanel()->getBus()` and `bus->writeBytes(...)` for DMA-capable buffers.
  - Fallback to `M5.Display.writePixelsDMA()` if the buffer isn't DMA-capable.
  - `waitDisplayDMA()` now waits on the bus directly when available.

Results (60s boot window):
- Baseline (sampler off, batch 8192, quantum 640k, row strips off):
  - Render avg ~775 us
  - IPS avg ~1.92 MIPS
- Direct bus DMA path:
  - Render avg ~49 us
  - IPS avg ~1.95 MIPS

Log:
- `/tmp/pio_perf_directbus_boot60s.log`

Important correction:
- The ESP32-P4 display uses a DSI bus (`bus_dsi`). On DSI, `IBus::writeBytes()`
  is a no-op (Bus_ImagePush), so the direct-bus path produced a black screen.
- I added a guard: direct bus DMA is now used only for `bus_spi` and `bus_parallel*`.
  DSI falls back to `M5.Display.writePixelsDMA()` to keep the display working.
- The big render win above is therefore not applicable on DSI hardware.

## Waveshare perf pass (2026-04-19): what we learned

### Round 1 (sdram/fused-rotate/POSIX) - reverted
- Changed SD to HIGHSPEED (kept), rewrote Sys_* to POSIX pread/pwrite (reverted),
  fused the tile rotation/scale directly in the HAL (reverted), 8x8 block
  transpose fallback (reverted).
- Benchmark after build+flash (Mac Quadra 605 = 1.0):
  - CPU 0.528 -> 0.519, Disk 0.769 -> 0.792, Graphics 0.417 -> 0.416,
    Math 6.428 -> 6.243. All within noise.
- Read-through: Disk and Graphics benchmarks are Mac-side HFS and QuickDraw
  code running on the 68k interpreter. They track *emulated 68k speed*,
  not our real SD bandwidth or display pipeline. Math escapes the
  interpreter (host IEEE FPU) and is the only metric at 6x, confirming
  everything else is interpreter-bound.
- All three changes were outside the 68k hot path, which is why the scores
  did not move.

### Round 2 (400 MHz bump + shed streaming buffers for cpufunctbl)
- Captured a boot log. Two findings:
  1. `[MAIN] CPU Frequency: 360 MHz` - pioarduino's precompiled ESP-IDF
     accepts only 360 MHz in `setCpuFrequencyMhz`:
     `CPU clock could not be set to 400 MHz. Supported frequencies: 360 MHz`.
     Hitting 400 would require rebuilding ESP-IDF from source.
  2. `cpufunctbl (256KB) in PSRAM (fallback)` is structural, not BSS
     fragmentation. At boot the internal heap has 344 KB free total but
     the largest contiguous block is 253 KB because the P4 DIRAM is
     split into multiple discontiguous pools. Shedding 20 KB of BSS
     actually dropped the largest block to 245 KB (the removed BSS
     lived in a region that is not contiguous with the big pool).
  3. `Compact dispatch enabled: 1869 unique handlers, opcode index in
     internal SRAM` - the hot 68k dispatch is ALREADY in internal SRAM
     via the 128 KB compact index + ~8 KB handler table. Fitting
     cpufunctbl in internal SRAM wouldn't change the hot path; the
     compact_dispatch path is already optimal.
- Kept: SD HIGHSPEED (one-liner), streaming_row_buffer_a/b removal
  (saves ~20 KB DIRAM, eliminates the dead `renderFrameStreaming`
  path that was gated behind `DIRTY_THRESHOLD_PERCENT=101` and never
  called), `BoardDisplay_ClearScreen` HAL helper for the init clear.
- Reverted: `setCpuFrequencyMhz(400)` call (always fails, added noise
  to boot log).

### Takeaway
CPU / Disk / Graphics all move together with emulated 68k MIPS. The only
things that will lift them further are:
- Rebuilding ESP-IDF from source with 400 MHz PMU config (biggest lever,
  but requires abandoning pioarduino's prebuilt libs).
- Reducing time-per-opcode in `m68k_do_execute` (LTO, inlining the RAM
  fast path, per-opcode native fast paths, or ultimately a small JIT).

The SD HIGHSPEED and streaming-buffer cleanup stay. Next exploration will
focus on time-per-opcode.

### Round 3 (2026-04-19): tried to switch to pioarduino's postv3 variant - reverted

Pioarduino ships two ESP32-P4 prebuilt lib sets in
`framework-arduinoespressif32-libs`:

- `esp32p4_es/` - "Engineering Sample, pre-rev 3.00" - this is what our
  build has been using all along via `build.chip_variant=esp32p4_es` and
  `f_cpu=360000000L`. Clamped to 360 MHz.
- `esp32p4/` - "v3.00 or newer" - built against `CONFIG_ESP32P4_REV_MIN_301`,
  unlocks 400 MHz via `f_cpu=400000000L`.

Arduino IDE's "ChipVariant" menu flips between them. In platformio.ini
the same switch is `board_build.chip_variant = esp32p4` plus
`board_build.f_cpu = 400000000L`.

I flipped to the `esp32p4` variant and reflashed. The device panicked
immediately at the bootloader entry:

```
ESP-ROM:esp32p4-eco2-20240710
...
Guru Meditation Error: Core 0 panic'ed (Illegal instruction)
PC: 0x4ffac2c0   (inside the bootloader stub loaded at 0x4ffac2c0)
```

Despite the ESP-ROM banner calling our chip "esp32p4-eco2", the silicon
in this Waveshare P4 10.1 is genuinely pre-rev 3.00 at the ISA level.
The rev 3.01 bootloader in the `esp32p4/` lib pack uses instructions
our chip does not implement, so it panics before reaching `main()`.

Net: **400 MHz is not reachable on this specific board** without newer
silicon. Reverted all the switch-to-postv3 changes (`platformio.ini`,
`sdkconfig.waveshare`, `src/main.cpp`) and confirmed the device boots
fine at 360 MHz again. Leaving the `setCpuFrequencyMhz(400)` call out
entirely so the boot log isn't noisy with a `failed` message.

The final commit from this round keeps only:
- SD_MMC at HIGHSPEED (40 MHz negotiated)
- Removed 40 KB of streaming buffers + dead `renderFrameStreaming`
- New `BoardDisplay_ClearScreen` HAL helper

None of which actually move the benchmark. Real gains from here will
require either newer ESP32-P4 silicon (rev 3.01+) or per-opcode work
inside `m68k_do_execute`.


## Display reliability audit — 2026-09-26

Scope: review v3.4.2 through v4.7.1 and repair display/input correctness.
Candidate branch: `codex/display-reliability`, based on v4.7.1 / `11f4d60`.
Worktree: `<Codex managed worktree>/M5Tab-Macintosh`.
The control checkout's uncommitted performance branch was preserved and has
not been integrated. This pass is not a new performance benchmark.

Implemented shared direct panel publication, complete atomic damage tracking,
coherent palette/mode and overlay snapshots, publication retry, bounded splash
handoff, renderer shutdown handshake and capture lifetime protection. Removed
duplicate asynchronous DMA paths and unused buffers. Added conservative
QuickDraw fallbacks, report-ID-aware mouse decoding and serialized key claims.
See `DISPLAY_RELIABILITY_REVIEW.md` for findings and the ownership contract.

Verification after code freeze: 46 host tests passed in 7.038 seconds,
including six ASan/UBSan native targets; all five PlatformIO environments
passed in 206.077 seconds. `git diff --check` passed. Test/build logs are in
ignored `artifacts/display-reliability/` in this checkout; candidate images,
SHA256 manifest and the review are in the control checkout's ignored
`artifacts/display-reliability-20260926/`.

No flashing, device interaction, push or published release. Artifact names and the saved source SHA identify this pre-release
candidate, which predates the 5.0 startup version banner. Single-buffer tearing, hardware compatibility and performance
remain to be tested. Next: Austin flashes the matching board/silicon profile
and follows the hardware checklist, then integrates with performance work
if the result is accepted.


## Tab5 flash and dotted-border A/B — 2026-09-26

Austin explicitly authorized flashing the connected Tab5, then reported dotted
Finder borders around the scrollbars. The connected device is ESP32-P4 rev1.3.
The full existing 6 MiB application partition was backed up, partition layout
verified, and the 82e5266 pre-v3 debug app flashed at 0x10000. Full readback
SHA256 matched 951e58e01d66d65dea449db1993101779d7ca997a8abea3c985a35f4b470a287.
SD mounted, the emulator initialized and saved settings were unchanged.

Serial capture confirmed the border problem already exists in guest pixels.
A second build changed only CPU_NATIVE_QD_ACCEL from 1 to 0. It was flashed and
verified by esptool, booted the same disk/settings, and restored solid Finder
borders. The matched lower border changed from 160 black/160 white pixels to
320 black pixels. Native counters were all zero in the comparison. The enabled
run had shape and copy calls but no native lines; the exact faulty primitive
has not been isolated.

Current hardware app SHA256:
d79cac3ca0745b242d60d363701fc7e3ab1cb54af56191f6a762a51d550aee6d.
The branch now defaults native QuickDraw off on every profile, preserving the
display pipeline cleanup. This prioritizes guest drawing correctness and may
reduce graphics throughput; no new speed claim is made. All 46 host tests
passed after the flag change. See QUICKDRAW_REGRESSION.md for the final build
matrix, source/evidence mapping and rollback command. Evidence is retained
under artifacts/display-reliability-20260926/{hardware-flash,quickdraw-regression}
in the control checkout. Bootloader/partition table/NVS were not reflashed.
No push or published release. Next: Austin checks physical display behavior;
re-enable native drawing only after differential guest-pixel validation.


## v5.0 stable release preparation — 2026-09-26

Austin approved the corrected display and requested publication as
"5.0 - Stable". Release preparation stays on `codex/display-reliability`;
the original checkout's uncommitted performance work remains untouched.
Added a port-specific 5.0 startup banner and updated download/release notes.
Native QuickDraw remains disabled. Final validation passed 46 host tests
in 8.826 seconds and all five firmware builds; the four standard merged
images match their bootloader, partition and application components.

The final debug app SHA256 is
`485a9e3096f5e9a83754f4b957570ff83197e2305ac57bc99e6980601d6dc4b8`.
Only the application was flashed, and full readback matched. Saved settings
were preserved. Serial checks alone passed, but the screenshot showed a guest
illegal-instruction dialog after sustained SD CRC/status errors. One software
restart produced a damaged-System-file message. This supersedes the initial
serial-only health result: the guest desktop has not passed final verification.
A full user-assisted power cycle was requested; neither disk replacement nor
host-side repair was performed. The cause remains unconfirmed.

Evidence is in the control checkout's ignored `artifacts/release-v5.0/`,
including both failed screenshots, serial logs, readback, packaging script
and `release-status.json`. Packaging now requires an explicit verified guest
desktop result. Stable publication is pending this hardware check. Next:
inspect after cold start, resolve any remaining issue, then merge/publish
and record exact source/tag/asset identity in the release manifest.


## Tab5 SD mitigation and PR #16 review — 2026-09-26

The user power-cycled the Tab; startup still reported a damaged System file.
Austin then reported that another reboot got past it. An assistant BOOT ENTER
interrupted that successful start; subsequent tests were coordinated with
Austin, who explicitly approved flashing and running the SD diagnostic.

Changed only Tab5 SD SPI from 25 MHz to 10 MHz in the diagnostic. Full app
readback matched, original settings and disk selection were preserved, and
three starts avoided the earlier SD errors. Finder was visually verified on
starts one and three; start two reached the normal improper-shutdown notice.
The release retains the 10 MHz setting as a conservative mitigation. Exact
transport cause and throughput impact remain unmeasured. SD_STABILITY.md
records the evidence and limits. The hardware-tested app and final debug
build both hash to cfbc1d1592df1d49463434525a132d99592d71c19e56f410e9ab9ad5d93ee213.
All 46 host tests passed in 7.126 seconds; the final firmware matrix and
component checks are retained in artifacts/display-reliability/v5-sd-*.

Reviewed PR #16 at 54b0f9a. Its multi-block transfers improve only USB Disk
mode, not Mac OS disk access. Found undefined behavior in the derived-reference
cast used to expose the protected drive number. Both the PR unit and a
pointer-to-member replacement compile with the real Tab5 debug flags. Saved
review/patch under artifacts/release-v5.0/pr16-review; no external review
comment, merge, or contributor-branch change was made.

Next: merge PR #21 and publish the authorized v5.0 stable release with the
tested SD clock. The release manifest and local release-status.json retain
exact commit, tag, asset hashes and publication outcome.


## Speedometer interpreter and display pass — 2026-09-26/27

Austin asked for better Speedometer 4.02 Performance Rating CPU and Graphics
(v5.0: CPU 0.545, Graphics 0.242, Disk 1.02, Math 6.681) with no stability
regression, and authorized free use of the attached Tab5 test device. Work is
on local branch `claude/speedometer-perf` in `worktrees/speedometer-perf`,
based on v5.0 `1468a91`. It is uncommitted, not pushed, and has no release
actions. The control checkout's pre-existing uncommitted work is untouched.

Method: added a firmware `PERF` sampler, `PEEK` and `PANEL VERIFY`, plus a
symbolizing `tools/perf_report.py`, then A/B-tested each change with
repeated single-test ratings. Every build flashed (app only, 0x10000) is
saved with its patch under `artifacts/speedometer-perf/builds/`, and runs are
under `runs/` and `tool-runs/`.

Kept: inline guest-RAM access with IRAM slow-path thunks (CPU 0.545→0.773),
framebuffer stores in the thunk, 32-pixel dirty spans/tiles with a packed
indexed tile writer, native Toolbox trap dispatch behind a ROM-signature
check, 295 IRAM handlers, shifted-operand ALU flags (CPU 0.804),
non-cacheable scanout writes (Graphics 0.398→0.492), and `.sdata` hot
globals. Rejected: chained dispatch (0.728), fence removal, logical-flag
rewrite and 64-pixel tiles (neutral). Native A-line QuickDraw was shelved:
Mac OS 8 routes QuickDraw through RAM patches and private ROM vectors. The
prototype patch is in `artifacts/speedometer-perf/shelved/`.

Benchmark runner: `--suite rating` now runs Performance Rating unattended.
It recognizes the OCR-unreadable splash by pixels (Austin had been clicking
it by hand) and finds the desktop alias from its OCR label. A stale icon
coordinate had opened "Browse the Internet" instead. A text-only splash
fallback also looped forever when OCR dropped "File" beside an open result
window; it now requires the Tests/Analysis titles to be unreadable.
`test_speedometer_benchmark.py` covers both classifiers.

Found and fixed a pre-existing bug: the guest `Time` global never advanced,
so the menu-bar clock stayed at its boot value. The emulator thread now
refreshes it on the 1 Hz interrupt, as upstream Basilisk II does. A
Math-only A/B (3 runs each) measured 9.533 on final-23 and 9.475 on
clock-24, a 0.6% cost. Math is now 9.45–9.48 in every procedure, against
9.30–9.68 before.

Verification: 52 host tests pass; all five profiles build with PlatformIO
Core 6.2.0 (`.build/release-tools/bin/pio`); the debug build is
byte-identical to the flashed clock-24 image
(`2e1074ced92fb37b500d53dd4e26e0453666dd9f46ab11c0154d2f9c2e100de9`).
Soaks gave 5 full ratings on final-23 (CPU 0.803, Graphics 0.496–0.497,
PR 0.761–0.766) and 3 on clock-24 (CPU 0.804, Graphics 0.496–0.497,
PR 0.765–0.766). Each run had 0/921,600 panel mismatches, no reboot and no
crash markers. Color QuickDraw passed at 1/2/4/8-bit depth.

Risks: Waveshare has not run this branch. Tab5 internal SRAM free after init
fell from 231 KB to 177 KB because of the IRAM handlers.

Next: Austin reviews the diff and decides whether to commit/PR. Waveshare
hardware validation comes before release.


## v5.0.1 release preparation — 2026-09-27

Austin approved releasing the performance work as 5.0.1, explicitly waiving
Waveshare hardware testing. Bumped `M5TAB_FIRMWARE_VERSION` to 5.0.1, rewrote
RELEASE_NOTES.md and the README release/architecture sections, and added
EMULATOR_PERFORMANCE.md as the design and maintenance guide. Before release,
fixed the Speedometer runner's text splash fallback (it looped when OCR
dropped "File") and added `test_speedometer_benchmark.py`; 52 host tests pass.

Built the four release images with `scripts/build_release.sh v5.0.1`
(PlatformIO Core 6.2.0) and the debug profile. The debug bootloader and
partition table are byte-identical to v5.0's. Hardware gate
(`artifacts/release-v5.0.1/hardware_gate.py`): app-only flash of SHA256
`9dcf67f6…`, readback match, `Firmware: 5.0.1` banner, settings unchanged,
Finder desktop. The gate rating scored CPU 0.804, Graphics 0.495, Disk 1.03,
Math 9.431 and PR 0.765, with `PANEL VERIFY` 0/921,600.

Next: merge, tag v5.0.1, publish the verified assets, and record the outcome
in `artifacts/release-v5.0.1/release-status.json`.
