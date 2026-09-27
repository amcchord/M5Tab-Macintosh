# 5.0.1 - Performance

A performance release. It makes the 68040 interpreter and the display
pipeline faster without changing what Mac OS draws, and fixes the Mac clock.

Speedometer 4.02 Performance Rating on the development Tab5 (Quadra 605 = 1.0):

| Score | 5.0 | 5.0.1 | Change |
| --- | ---: | ---: | ---: |
| CPU | 0.545 | 0.804 | +48% |
| Graphics | 0.242 | 0.496 | +105% |
| Disk | 1.02 | 1.03 | unchanged |
| Math | 6.68 | 9.47 | +42% |
| **Performance Rating** | **0.458** | **0.765** | **+67%** |

5.0.1 values are medians of five full ratings on the final code.

## What changed

- **Faster guest memory access.** Guest RAM reads and writes are handled
  inline in each instruction handler. ROM reads and screen writes go to small
  register-preserving helpers in internal RAM, so the common instruction
  paths stay compact and cache-friendly.
- **Native Toolbox and OS trap dispatch.** Mac OS system calls skip the ROM
  dispatcher's exception frame when the dispatcher is the unmodified ROM code.
  Trap patches installed by Mac OS and extensions still run. A debugger or a
  different ROM automatically gets the original path.
- **Cheaper condition codes.** The generated instruction handlers compute
  arithmetic flags with fewer instructions. A new host test checks them
  against the original formulas.
- **Hot code in internal RAM.** The 295 most frequently executed instruction
  handlers and the core exception and addressing helpers now run from
  internal SRAM.
- **Finer, cheaper screen updates.** Screen changes are tracked in 32-pixel
  spans at every color depth. Tiles are 32 pixels wide. A new tile writer
  converts packed 1/2/4/8-bit pixels straight to the panel. Panel writes
  bypass the CPU cache, which the emulator core also uses. The black-and-white
  mode used by Speedometer's Graphics test benefits most.
- **Mac clock now advances.** Earlier versions left the menu-bar clock at the
  time the Mac booted, because the guest's `Time` value was never refreshed.
  It now updates every second, as in upstream Basilisk II. This costs about
  0.6% on Math and nothing elsewhere.
- **Benchmark and diagnostic tools.** `tools/speedometer_benchmark.py --suite
  rating` runs the Performance Rating unattended, including Speedometer's
  splash and registration screens. New serial commands add a sampling
  profiler (`PERF`), read-only guest memory inspection (`PEEK`) and a
  full-screen pixel check (`PANEL VERIFY`). `tools/perf_report.py` symbolizes
  profiles.

Native QuickDraw acceleration remains disabled, as in 5.0. The display
reliability, SD clock and input fixes from 5.0 are unchanged.

The [performance design guide](EMULATOR_PERFORMANCE.md) explains each change,
its safety conditions and how to maintain it. The
[performance report](PERFORMANCE_REPORT.md) has build-by-build measurements
and rejected experiments. The [serial control reference](AUTOMATION.md)
covers the benchmark runner and diagnostic commands.

## Downloads

| Hardware / use | Filename | Flash offset |
| --- | --- | --- |
| Tab5, pre-v3, standard SDK | `M5Tab-Macintosh-v5.0.1.bin` | `0x0` |
| Tab5, v3.1+ | `M5Tab-Macintosh-Rev3-v5.0.1.bin` | `0x0` |
| Waveshare 10.1, pre-v3 | `M5Tab-Macintosh-Waveshare-P4-10.1-v5.0.1.bin` | `0x0` |
| Waveshare 10.1, v3.1+ | `M5Tab-Macintosh-Waveshare-P4-10.1-Rev3-v5.0.1.bin` | `0x0` |
| Tab5, pre-v3, tested USB debug SDK | `M5Tab-Macintosh-USB-Debug-v5.0.1.bin` | `0x0` |
| Same debug application, existing compatible Tab5 installation | `M5Tab-Macintosh-USB-Debug-v5.0.1-app.bin` | `0x10000` |
| Python CLI, MCP server, and benchmark | `M5Tab-Macintosh-Host-Tools-v5.0.1.zip` | — |

Chip revision is separate from panel or PCB revision. Rev3 images require
ESP32-P4 revision **3.1 or newer**; revision 3.0 is not covered. Merged images
include bootloader, partitions and application, and replace an existing
launcher. The debug application-only image preserves bootloader, partition
table and NVS on a compatible installation. Neither package includes a
Macintosh ROM, disk images or credentials.

The optional pre-v3 Tab5 debug profile uses Arduino 3.3.8 / IDF 5.5.4 and
matches the development device used for hardware testing. Use the matching
board/silicon image; the debug image is not for Waveshare or production Rev3
hardware.

## Verification

- 52 host tests. New since 5.0: condition-code checks against the classic
  formulas (exhaustive for bytes), 32-pixel span, rotation and indexed-tile
  display cases, and benchmark-runner screen classification.
- All five firmware profiles build. Every merged image is checked against its
  bootloader, partition table and application at the expected flash offsets.
- Hardware checks on a pre-v3 ESP32-P4 revision 1.3 Tab5. The final
  performance build completed eight full Performance Ratings with identical
  CPU and Graphics results. Each run ended with a full-screen pixel check
  showing 0 of 921,600 mismatches, and there were no resets or crashes.
  Speedometer's Color QuickDraw test completed at 1, 2, 4 and 8 bits. The
  released debug application was flashed and read back, printed the 5.0.1
  banner, and booted to the Finder with saved settings intact. It then
  completed a Performance Rating (0.765) with a clean pixel check.
- `BUILD-MANIFEST.json` records exact source commits, component hashes and the
  hardware-tested application. `SHA256SUMS` covers downloadable assets.

## Limits and rollback

Waveshare and production-silicon (Rev3) images share this code and build
cleanly but were not run on hardware for this release. The optimized code
uses about 46 KB more internal RAM; on the Tab5, free internal SRAM after
startup is 177 KB, down from 231 KB.

The [v5.0 release](https://github.com/amcchord/M5Tab-Macintosh/releases/tag/v5.0)
remains available for rollback with a matching board/silicon image. Preserve
known-good firmware and SD backups. Firmware rollback does not revert settings
or guest disk contents.
