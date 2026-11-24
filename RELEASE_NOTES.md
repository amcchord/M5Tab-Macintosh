# 5.5

5.5 speeds up the 68k interpreter, indexed display pipeline, and ahead-of-time (AOT) translated code runtime while keeping all v5.4 installer, A/B Wi-Fi update, and remote console features intact. The device and remote console identify this release as 5.5. [Downloads](https://github.com/amcchord/M5Tab-Macintosh/releases/tag/v5.5) · [Performance report](PERFORMANCE_REPORT.md) · [Setup and imaging screenshots](screenshots.md)

## What is faster in 5.5

- **68k interpreter (`861d984`):** Packs arithmetic condition codes (`c`/`z`/`n`/`v`) into a single 32-bit `regflags.cznv` word with single-store flag updates and word `cctrue()` tests (`m68k.h`), moves leaf `RTS` (`op_4e75_0_ff`) and split A-line dispatchers (`op_aline_os`, `op_aline_toolbox`) into IRAM (`newcpu.cpp`), and adds pipelined `mem_setpc_slow_thunk` (`memory.cpp`) alongside narrow register stores and sign-extended immediate fetches (`gencpu.c`).
- **Display pipeline (`6f3e770`):** Adds 32-bit aligned burst indexed tile writers (`ScaleTraits`, `emit_u32_row`, `emit_u16_row`, `writeSingleRowIndexed` in `src/board/panel_surface.h`), caches palette `pairs[]` across frames via `pairs_dirty`, and replaces byte-by-byte dirty clears with row-wise 32-bit `video_dirty_chunks` scans (`src/basilisk/video_esp32.cpp`).
- **Translated-code runtime & translator (`2a68ad3`, `cd2d430`):** Adds a 4 KB page occupancy bitmap (`aot_page_bitmap`) so clean guest writes skip AOT invalidation in O(1), narrows Quadra 650 `_HLock` `CPUSHA` flushes (`AotHandleCpusha`, narrowing 99.77% of flushes in Stage 2b soak), adds backward-dataflow flag liveness DCE (`compute_region_liveness`), batched `aot_load_cznv`/`aot_store_cznv`, single-entry specialization, direct block chaining, hot-inline region placement, `aot_setpc_thunk`, and extended `AOT STATS` telemetry (`cpusha`, `rskip`, `rinv`).

Measured on a Tab5 (ESP32-P4 rev 1.3 at 360 MHz, Mac OS 8 on `/Macintosh8.dsk`, 24 MB guest RAM, Speedometer 4.02):

- **Open-source interpreter + display build (no AOT):** CPU `0.840` (`+7.7%` over v5.4 interpreter `0.780`), Graphics `0.461` (`+2.2%` over `0.451`), full-suite Performance Rating `0.828` (`+9.8%` over `0.754`).
- **Shipping system-AOT build (`stage2-sys`, ROM + system RAM translated):** Graphics **`1.002`** (`+122%` over interpreter baseline, `+19.9%` over v5.4 system AOT `0.836` on the same disk), full-suite Performance Rating **`1.130`** (CPU `0.840`, Disk `1.973`, Graphics `1.008`, Math `10.804`), Color QuickDraw Monochrome **`6.618 s` (`1.044`)** / 8-bit Color **`11.599 s` (`0.913`)**, Graphics throughput **`12.913` 68k MIPS**, and `30.3 s` cold boot (`22.0 s` since emulator start).
- **Optional application-profiled build (`stage2-full`):** Reaches **`2.204`** CPU (`+183%` over interpreter baseline, `+37.4%` over Pass 2 `1.604`), **`1.691`** Performance Rating, and **`19.039` 68k MIPS**.

[Full Pass 3 measurements and `PERF` profiles](PERFORMANCE_REPORT.md#pass-3-interpreter-display-and-aot-optimization--2026-11-2324)

## Hardware and host checks

- Both pre-v3 Tab5 applications (`M5Tab-Macintosh-USB-Debug-v5.5-app.bin` in `ota_0` and `M5Tab-Macintosh-v5.5-app.bin` in `ota_1`) were installed over Wi-Fi on an ESP32-P4 v1.3 with its stock ESP32-C6 radio, verified in flash, booted on `/Macintosh8.dsk` to Mac OS 8.1 Finder, passed `PANEL VERIFY` (`0` mismatched out of `921,600` pixels), passed `AOT STATS` (`530/530` ROM regions running from PSRAM, `0` rejects), and reported firmware **`5.5`** (`ota_slots=2`) via `/desk/status`.
- All five board/silicon release profiles (`esp32p4_pioarduino`, `esp32p4_pioarduino_debug`, `esp32p4_pioarduino_rev3`, `waveshare_p4_101`, `waveshare_p4_101_rev3`) compiled and passed `check_blob.py` (`573,964` bytes, `1,282` functions, `169,208` instructions checked). The Rev3 Tab5 and both Waveshare profiles are build-tested.
- Host validation passed: `121` Python unit tests (`2` skipped), `43,308,858` native C++ CPU flag/helper checks (`test/test_cpu_flags.cpp`), and host differential AOT verification (`aot_diff --trials 12`: `51,173` runs, `193,464,986` instructions compared, `0` failures, `41` skipped).

## Choosing a download

Choose the firmware for your board and ESP32-P4 revision. The release includes separate images for pre-v3 and v3.1-or-newer Tab5 and Waveshare boards, plus a pre-v3 Tab5 USB Debug build.

| Installation | File | Use |
| --- | --- | --- |
| First USB flash or standard reinstall | Matching merged firmware `.bin` | Flash at offset zero. This installs the standard single-slot layout. |
| Existing two-slot installation | Matching `-app.bin` | Give this file to the Wi-Fi updater. The updater chooses the inactive slot. |
| Host utilities | Host Tools ZIP | USB pairing, screenshots, input, benchmarking and updates. |
| Guest utilities | Mac Extras ZIP | Macintosh Control Strip module and guest utilities. |

Check `SHA256SUMS` and `BUILD-MANIFEST.json` for download hashes, SDK versions, chip limits, and the exact source revision. [USB flashing instructions](README.md#flashing-the-firmware)

The previous stable release was v5.4. [Download v5.4](https://github.com/amcchord/M5Tab-Macintosh/releases/tag/v5.4)
