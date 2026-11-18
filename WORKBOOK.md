# M5Tab-Macintosh workbook

## Tab5 hardware handoff and v5.4 beta 2 — 2026-11-17

Release branch `codex/tab5-hardware-release` starts at the four handoff commits through `cb3a03a`, on top of v5.4 beta 1. The release keeps the v5.3 translated code (`7fb5dfee…95e3dc`). The control checkout's existing local workbook edit was preserved.

The attached pre-v3 Tab5 (ESP32-P4 v1.3) ran both release applications. USB Debug SHA-256 starts with `b970bb3e`, standard with `3ee30ccb`. Both flash verifications passed. Hardware coverage, the unclean-disk upgrade procedure and remaining limits are in [the handoff](TAB5_HARDWARE_HANDOFF.md) and [release notes](RELEASE_NOTES.md). The preboot screenshot tool now follows the USB Disk restart even if its final touch acknowledgment is lost.

All 73 host tests passed. The release translation matched the interpreter in 131,824 trials over 506,880,417 instructions. All five release environments passed build, translated-code and merged-image checks. Waveshare and production silicon were build-tested. The release includes the standard board images, USB Debug merged and application-only images, host tools, Mac extras, build manifest and checksums.

Private test evidence is under `worktrees/tab5-hardware-release/artifacts/handoff-release/`. The original 16 MB flash backup is under the control checkout's `artifacts/tab5-hardware-release/`. The attached device started in v5.3 Startup Settings. A temporary SD preparation copy was interrupted. Its allocation was recovered, the Mac reader verified the FAT filesystem, and the complete test copy matched the original after a cold read. The Mac OS tests used the duplicate. Speedometer 4.02 completed on the standard build with a 0.908 Performance Rating. After testing, the temporary SD files were removed, the original settings and PRAM were restored, and all 16 MB of flash matched the original backup. The Tab5 returned to v5.3 Startup Settings with the original disk selected.

## PR #29 and v5.4 beta 1 — 2026-10-18

Review/release work is in `worktrees/scaling-review-release`, branch
`codex/scaling-review-release`, based on Raf's `acbf65a`. The control
checkout's existing workbook edit was preserved. User authorized the review,
fixes, integration and a new release. No USB-connected display was available;
the release is a prerelease and v5.3 remains stable.

The initial GitHub review reproduced an ASan source-buffer overread for a
1× indexed tile exceeding its row stride. Commit `faf43e0`, pushed to Raf's
PR branch, validates both 1× and 2× source ranges with widened arithmetic,
keeps source/output pointer advancement within arrays and adds missing 1×
preboot and overlay/fallback coverage. Packed decoding and destination
rotation match the reference renderer; existing 2× configurations and
platform pins are retained.

Validation: 68 host tests, including ASan/UBSan display and preboot tests at
all three geometries; full fallback rendering at every indexed depth and
rotation; odd single-pixel and last-pixel preboot updates; failed publication
retry. All five firmware profiles build and pass their translated-code blob checks.
Tab5 data/BSS section sizes match v5.3. The local generated ROM/system translation is identical
to v5.3 (`7fb5dfee…95e3dc`); no CPU or guest changes were made.

Evidence and release packaging: `artifacts/review-pr29/` in this worktree.
The manifest records exact commits, SDK/component hashes, checks and the
absence of hardware validation. No device was flashed. Roll back with the
matching v5.3 board/silicon image; preserve settings and SD backups.

Next: Tab5 startup/Finder/depth/rotation/overlay/panel regression before
stable promotion. Olimex (#27), other proposed boards and color mapping
(#28) remain separate open investigations; this scaling release does not
claim to fix or validate them.

## v5.3 release — 2026-10-11

Austin asked to release the battery, sleep and backlight work as **5.3**.
Branch `claude/battery-power` (`worktrees/battery-power`) was rebased onto
the 5.2 merge (`0f73b21`); it adds the feature commits, the version, release
notes, README and this section, and a fix so `mac_control.py boot-get` works
against firmware without `power_manager`. The translation is unchanged
(`sys-22`, SHA256 `7fb5dfee…`). Release scripts and evidence are in the
control checkout's ignored `artifacts/release-v5.3/`; the gate adds the
power checks and the shifted Special-menu positions.

Checks: 64 host tests; all five profiles build and pass `check_blob.py`;
537/537 regions install over the patched ROM. Austin asked to skip the
device self-test (stopped after 8,130 trials, 0 failures). The gate flashed
the USB-Debug app (SHA256 `60c4c3cf…`) and read it back, but in the reset
that followed the Tab5 left USB and did not return; Austin asked to merge and
publish without the remaining gate steps. Hardware evidence is the
pre-release testing below (same power code on that Tab5). The Tab5 needs a
physical check: it was last seen running the 5.3 debug app, not 5.2.
Publication details are in `artifacts/release-v5.3/release-status.json`.

## Battery, sleep and backlight for Mac OS — 2026-10-10

Austin asked whether Mac OS 8 could see the Tab5 battery (Control Strip
Battery Monitor), then to build it with documentation for future battery
and sleep work, and to make the backlight controllable from the Mac. Work
is on branch `claude/battery-power` (`worktrees/battery-power`, from
`2b72477`, rebased onto 5.2 for release). [POWER_MANAGEMENT.md](POWER_MANAGEMENT.md)
describes the design, the guest-visible interface and how to add a board.

- **Board layer** (`src/board/board_power.*`, one hooks file per board):
  battery level from the INA226 voltage with IR compensation and an
  open-circuit curve, smoothing and monotonic display, charger state and
  time estimates, backlight with dim and off states. Waveshare: no
  battery, backlight only.
- **Guest layer** (`src/basilisk/power_manager.cpp`): high-level emulation
  with the machine still a Quadra. ROM Gestalt `'powr'` and Mac OS 8.1's
  Gestalt (`ptch` 5, patched as it loads) report $1F; `$A09E`, `$A09F`,
  `$A085` (and its `IdleUpdate`/`IdleState`/`SerialPower` variants) and
  `$A08A` go to EMUL_OP stubs; sleep turns the backlight off and parks the
  CPU until touched; Power Settings' timeouts dim and blank the display.
- **Power services**, Gestalt `'M5pw'`, on every board: backlight, battery
  record, display off, log. Re-registered on INIT loads because `ptch` 5
  rebuilds the Gestalt table.
- **Brightness Control Strip module** (`guest/control-strip-brightness`,
  Retro68), prebuilt in `dist/`.
- `power_manager` setting (settings file, `boot-set`), `POWER` serial
  commands and `mac_control.py power`.

Found on hardware and fixed: Mac OS 8.1 replaced `'powr'` without the
dispatch bit; the rebuilt Gestalt table dropped `'M5pw'`; the INA226 reports
charging current as negative and CHG_STAT stays "charging" with the charger
off; the Display Manager's sleep procedure crashed the firmware, so sleep
no longer calls sleep-queue procedures.

Verification on the second Tab5 (USB-Debug image, 24 MB, Mac OS 8.1 with
networking): Battery Monitor and the menu bar clock show level, charging,
time remaining and pack removed (live and `POWER SIM`); Sleep Now and
Finder Special > Sleep sleep and wake by mouse; idle dim at 60 s and off at
120 s with Better Conservation; Brightness menu, icon refresh, Turn Off
Display and saved level across restarts; Open Transport worked after a
sleep; all four trap entries still point at the stubs at the Finder;
537/537 translated regions; no crash markers. Speedometer Graphics follows
the Control Strip's width (stock 3 modules 1.016, 9 modules 0.945,
collapsed 1.406); CPU, Disk and Math are unchanged and the Power Manager
itself makes only a few hundred calls per rating. 63 host tests pass; all
five profiles build and pass `check_blob.py`.

Device state: the 5.2 USB-Debug app (`7400c294…`) was restored and boots
with its settings. Guest disk: the Brightness module is in Control Strip
Modules (it declines to load on 5.2), the damaged Control Strip Preferences
file (empty resource fork after repeated hard resets) is in the Trash, and
`basilisk_settings.txt` has `power_manager=yes`. Evidence: the worktree's
ignored `artifacts/battery-power/` (test drivers, logs, screenshots).

Not tested: battery-only operation (needs the USB cable out), a removed
pack, the shared-folder install route, Waveshare (built only). With the
Power Manager on, Special-menu automation coordinates shift by one row.
Released in 5.3 (section above).

## v5.2 release — 2026-10-10

Austin approved the guest RAM work, asked to merge it (PR #24, merge commit
`2b72477`) and to release it as **5.2**. Release branch `claude/release-v5.2`
(`worktrees/release-v5.2`) adds only the version, release notes, README and
this section. The translation is unchanged from 5.1 (`sys-22`, SHA256
`7fb5dfee…`). Evidence, gate and packaging scripts are in the control
checkout's ignored `artifacts/release-v5.2/`.

Pre-release checks: host AOT differential PASS (51,102 trials, 0 failures);
on-device self-test PASS (26,866 trials, 0 failures, 38 min). The first
hardware gate panicked on Special > Shut Down: `ether_exit()` deleted the
network receive task after it had deleted itself. The bug dates from v3.0;
5.2's power-on Wi-Fi fix made networking live at Shut Down. Commit
`Stop the network task once on Shut Down` on the release branch fixes it.
Gate attempt 2 (fixed app `7400c294…`) completed a rating (PR 0.997), then
the Tab5 reset while a color screenshot was being read over serial and came
back in Startup Settings. Nothing else opened the port (confirmed by the
battery-power session, which was building concurrently and had swapped the
shared RISC-V toolchain mid-build once). Not reproduced in four logged
ratings, gate attempt 3 or 150 color screenshots with Wi-Fi live; noted in
the release limits. Gate attempt 3 passed: PR 0.991 with networking loaded,
PANEL VERIFY 0/921,600, Shut Down with Wi-Fi to the power-off screen, clean
restart. Final records: `artifacts/release-v5.2/release-status.json`.

## Guest RAM up to 24 MB — 2026-10-10

Austin asked for 20–24 MB of guest RAM so Mac OS 8 can run a web browser,
based on 5.1. He also dropped the JIT for good. Work is on branch
`claude/guest-ram-24mb` (`worktrees/guest-ram-24mb`, from v5.1 `6c8a7e9`).
The paused JIT work was committed and kept only as the local tag
`archive/rv32-jit`; its branch was deleted and its 945 MB of evidence moved
to the macOS Trash.

The 16 MB cap was only the list of choices. With 16 MB, 10.75 MB of PSRAM
was still free after startup. The address map, the inline RAM fast path and
translated RAM code do not assume 16 MB. Changes:

- Memory choices are 8, 16, 20 and 24 MB, defined once in
  `boot_control_values.h`. Saved 4/12 MB settings round up to 8/16.
- If guest RAM would leave less than 5 MB of PSRAM, startup steps down 4 MB
  at a time instead of failing. The boot log reports the largest free block.
- `table68k` (1 MB) is freed once the dispatch table is built.
- The compiled-out JIT sources and hooks are removed. Firmware section sizes
  are identical.
- `MEM` over serial reports PSRAM free, largest block and lowest since boot.
- Fixed in passing (also in 5.1): a power-on Wi-Fi auto-connect was dropped
  before the emulator started, so the guest never had a network unless
  settings were changed over serial. It now waits up to 10 s.

Hardware: second Tab5 (ST7121), USB-Debug profile, app SHA256
`6447514f…2b349b52`, readback matched. At 24 MB: 3.44 MB PSRAM free after
startup (largest block 3.2 MB), lowest 2.86 MB across Speedometer, Wi-Fi and
browsing; Mac OS 8.1 reports 24 MB built-in, 18 MB largest unused block.
Speedometer PR 1.01 (CPU 0.806, Graphics 1.036, Disk 1.03, Math 10.56;
5.1 on this unit: 1.008). `PANEL VERIFY` 0/921,600 mismatches. Internet
Explorer 3.01a loaded `http://system7today.com` over the lab Wi-Fi.

Guest disk changes on that unit: Extensions Manager set `Speedy8 Net`
(`Speedy8` plus TCP/IP and six Open Transport libraries) is active and
TCP/IP is on; `Speedy8` itself is unchanged. With networking loaded the
rating is PR 0.998. Wi-Fi is configured with auto-connect, RAM is 24 MB.
The original app (5.1 USB-Debug, SHA256 `eda80f43…`) is backed up in the
worktree's ignored `artifacts/guest-ram-24mb/hardware/backup/`.

Not hardware-tested: Waveshare and Rev3 images (built only), finger touch on
the new Memory row (host-rendered only). Next: review the PR and decide
whether it ships as a 5.1.x release.

## v5.1 release — 2026-10-10

Austin asked to combine the Shut Down fix with the preboot work and the other
work since 5.0.1, push it, merge it into master and release it as 5.1. He
chose the translated code with ROM and system code only (not Speedometer's
own code). He asked for testing, with any issues fixed, before release. He
also chose to publish all four board images, with Waveshare and Rev3 noted as
build-validated only. The paused JIT is not included. Austin later dropped
it for good (see the guest RAM section above).

Integration branch: `claude/release-v5.1`, in session worktree
`.claude/worktrees/jolly-hertz-2b45fc`, based on v5.0.1 `94af0b8`:

| Commit | Content |
|---|---|
| `5871add` | Shut Down fix (section below) |
| `bcfa164` | Preboot UI, copied from `worktrees/preboot-ui` as it stood on 2026-10-09 23:26 |
| `5933165` | Translated code, copied from `worktrees/perf-pass-2` (final state, identical code to its `sys-22`/`full-23` builds) |
| `af1c02f` | Speedometer runner retries the tests-complete alert (found in pre-release testing) |
| `7af6bed` | Version 5.1, release notes, README, this section |
| `80c2a47` | Client retries `PANEL VERIFY` when its reply is cut short (found in the hardware gate) |

Those two worktrees were read, not changed; they still hold their uncommitted
copies. Code merged without conflicts; only AUTOMATION.md and this file
needed hand merging. The translation is the perf pass's `sys-22` generated
file (SHA256 `7fb5dfee…95e3dc`, 537 ROM regions, 398 system modules, 680 KB).
It is git-ignored and kept in `artifacts/release-v5.1/aot/`; a 5.1 image
cannot be rebuilt without it.

Pre-release checks on the combined code:

- 58 host tests; all five profiles build, and `check_blob.py` passes on each.
- Host differential test: 51,102 runs, 186 M instructions, 0 failures.
  Device self-test on the Tab5: PASS, 26,866 runs, 91.8 M instructions,
  0 failures (52 skipped by design).
- Tab5 release candidate: 537/537 ROM regions installed. Three ratings gave
  CPU 0.804–0.805, Graphics 1.032–1.034, Disk 1.029–1.031, Math 10.63–10.64
  and PR 1.008–1.009. A preboot tour showed 0/921,600 panel mismatches at
  every step. Shut Down reached the Platinum power-off alert, and the next
  start was clean. Restart returned to the Finder. Free internal SRAM after
  start-up is 168 KB.
- Issues found were all in host tooling, not firmware. The Speedometer runner
  could leave the tests-complete alert open (`af1c02f`), and the client failed
  on a `PANEL VERIFY` reply cut short by other serial output (`80c2a47`).
  The gate script also needed to answer Speedometer's "Save before
  quitting?" prompt. Release images are byte-identical between `7af6bed`
  and `80c2a47`.

Hardware gate on the final debug app (`eda80f43…`, byte-identical at
`7af6bed` and `80c2a47`): readback matched, 5.1 banner, all 537 ROM regions
installed, Finder with settings preserved. Rating: CPU 0.804, Graphics 1.032,
Disk 1.031, Math 10.64, PR 1.009. `PANEL VERIFY` 0/921,600. Shut Down
reached the power-off alert (phase `stopped`), and the next start was clean
with no crash markers. Another gate attempt on the same image rated PR 1.000
(Disk 0.986, Math 10.40), the low end of run-to-run variation.

Device state: the Tab5 runs the 5.1 debug app at the Finder, with settings
unchanged. Its earlier application (the preboot-ui build) is backed up at
`artifacts/shutdown-quit/hardware/app-partition-before.bin` in this worktree.

Evidence (session worktree, ignored): `artifacts/release-v5.1/`, which holds
the integration patches, builds, test logs, device runs, the gate's
`hardware/health.json`, and `release-status.json` with the publication
outcome. The three sections below record how each part was developed; their
branch, device-state and next-step statements are historical.

## Shut Down panic fix — 2026-10-09

Austin reported that Special > Shut Down on v5.0.1 (and `claude/preboot-ui`)
printed `QuitEmulator called`, then panicked with a NULL load in
`m68k_do_execute()`. The device rebooted into recovery settings, so the
"safe to switch off" screen never appeared. The work is on branch
`claude/jolly-hertz-2b45fc` (session worktree
`.claude/worktrees/jolly-hertz-2b45fc`), based on v5.0.1 `94af0b8`.

Cause: upstream `QuitEmulator()` exits the process. This port's version only
cleared `emulator_running`, which the CPU never reads, so execution continued
after the EMUL_OP. PowerOff (Q650 ROM+0x9E16) is patched by overwriting only
the first word of `BSR.W` with `EMUL_OP_SHUTDOWN`. The CPU therefore decoded
the displacement word and then `JMP (-8,PC,A0.L)` into unmapped memory, where
the dummy bank returns a NULL `pc_p`. The panic registers held fragments of
`default_xlate()`'s message. The bug has been present since at least v3.4.2
and is not a 5.0.1 regression. Polling once per 8 instructions only means that
setting BRK alone would also stop the CPU too late.

Fix: `QuitEmulator()` calls `m68k_request_quit()`, which sets a sticky request
and BRK. `m68k_emulop()` then parks the PC on two host-side `EXEC_RETURN`
opcodes, so the rest of the dispatch batch does nothing. `m68k_do_specialties()`
returns before any trace, interrupt or STOP handling, and `m68k_execute()`
returns at every nesting level. The inner dispatch loop and the opcode handlers
are unchanged.

Tab5 verification (debug app, flashed app-only at 0x10000 with a matching
readback):

- Shut Down: the CPU stopped in the same millisecond as the quit, and teardown
  took 0.2 s. Boot status reported `stopped`, and no panic occurred within
  45 s. A protocol-5 panel readback (fix plus the preboot-ui automation patch)
  showed "It is now safe to switch off your Macintosh."
- The next boot reached Finder with no improper-shutdown alert.
- Restart showed Welcome, then Starting Up, then Finder, without calling
  `QuitEmulator`.
- Speedometer Performance Rating, 3 runs each:

| | CPU | Graphics | Disk | Math | PR |
|---|---:|---:|---:|---:|---:|
| v5.0.1 release debug app | 0.804 | 0.495–0.496 | 0.997–1.031 | 9.435–9.478 | 0.762–0.765 |
| Fix | 0.803 | 0.495–0.497 | 1.030–1.034 | 9.412–9.419 | 0.764–0.766 |

The Performance Rating did not regress. CPU (one display unit) and Math
(about 0.3%) are slightly lower, although no hot path changed. The only new
work is one flag test per EMUL_OP and per specialty call.

Evidence is in this worktree's ignored `artifacts/shutdown-quit/` (test
driver, images, serial logs, screenshots, ratings). The Tab5 runs the final
debug app `705c8a5e…`, whose code sections are identical to the tested
`bd7a5c2d…`; only `__TIME__` strings differ. The previous application (the
preboot-ui protocol-5 build) is backed up as
`artifacts/shutdown-quit/hardware/app-partition-before.bin` (SHA256
`5a862ad0…`). Next: Austin's review. Pushing, opening a PR and releasing all
need his approval. `claude/preboot-ui` needs this fix before its own Shut Down
test.

## Preboot UI redesign and diagnostic screenshots — 2026-10-09

Austin reported the preboot UI had "gotten all wonky" and asked for a Mac OS 8
look (Charcoal font, pixel-doubled like the Mac screen) plus diagnostic
screenshots of the preboot environment. Work is on branch `claude/preboot-ui`
(`worktrees/preboot-ui`, based on master `94af0b8`), uncommitted. The control
checkout's paused JIT work is untouched.

Root cause of the regression: `scripts/build_assets.py` needed Pillow for the
Chicago TTF and PNG artwork, PlatformIO's Python has no Pillow, and the
fallback was silent. Builds from fresh worktrees therefore shipped the 5x7
debug font and a placeholder Happy Mac: the published v5.0.1 images and the
AOT image that was on the Tab5 both contain it. The old UI also drew 1-pixel
lines at native panel resolution (not doubled) and left a stipple stripe
through the settings window.

Changes:

- `preboot_ui.cpp` draws on a 640x360 (Waveshare 640x400) indexed canvas and
  presents changed areas through the emulator's own 2x indexed tile path.
  Platinum chrome, buttons, checkboxes, radios and scroll bars were measured
  pixel by pixel from Mac OS 8.1 running in the emulator. Icons and the desktop
  speckle are original.
- `assets/fonts/charcoal12.bdf` is a Charcoal 12 recreation traced from guest
  rendering; it reproduces the guest's text with 0 differing pixels. It
  replaces Chicago everywhere, including the emulator's touch keyboard overlay.
  `chicago_font.*` and `pixChicago.ttf` are removed.
- `build_assets.py` is standard-library only (own PNG decoder, verified
  identical to Pillow). It regenerates every header deterministically each
  build, and a present-but-undecodable asset fails the build. Also fixes a
  latent `_parse_bdf` bug that kept only the last glyph.
- `preboot_screens.cpp` holds pure layouts (settings, Wi-Fi, password
  keyboard, alerts, splash) shared by painting, hit testing and host tests.
  Behavior changes: lists scroll (arrows auto-repeat, track pages); one row per
  Wi-Fi name (strongest access point); the keyboard adds `- _ , . / ; : ' " = +`;
  Cancel discards typed text; the Tab5 USB Disk "Done" restart now returns to
  settings, as the alert says.
- Protocol 5 (`AUTOMATION.md#preboot-screenshots-and-touch`): screenshots and
  `PANEL VERIFY` before the emulator starts, `SCREENSHOT PANEL` (1280x720
  readback, any phase, ~1.5 s), `TOUCH`, `BOOT HIT`. The tour tool
  `tools/preboot_screenshots.py` captures every screen with verification.

Verification: 55 host tests pass, including native renders of every screen on
both boards and rotations with an exact 2x panel check. All five profiles
build. On the Tab5 (final debug app SHA256
`2f725a84d8793a812a23cabf85b1bf0164bdfd84d0706d8da5eb2d9ecf5bd96f`, readback
match), the full tour (settings, pressed button, list and radio edits, Wi-Fi
scan, keyboard typing, reload, USB Disk and its restart) had 0/921,600 panel
mismatches at every step. Start Up reached Finder, emulator `PANEL VERIFY`
0 mismatches, and the saved settings equal the session baseline. Evidence:
control checkout `artifacts/preboot-ui/` (`before/`, `final/tour/`,
`final/boot-check.json`, build and test logs).

Not verified: physical finger touches (all input was injected through the touch
task), Waveshare hardware, the error alert on hardware. Found but not fixed
(pre-existing on master): Mac OS Special > Shut Down panics in
`m68k_do_execute` after `QuitEmulator`, so "safe to switch off" never shows.

Device state: the Tab5 runs the final debug build above, on the Startup
Settings screen, settings unchanged. Its previous application (the AOT
`full-23` performance image, 5.0.1) is backed up as
`artifacts/preboot-ui/hardware/app-partition-before.bin` (SHA256
`05fe438e…bd18a2`); restore with an app-only write at 0x10000.

Next: Austin reviews the screens on the device; commit/PR on his approval;
rebuild release images so they carry the real font.

## Translated code pass — 2026-09-27/28

Austin asked for every Speedometer 4.02 Performance Rating score near or
above 1.0, without a JIT, and authorized flashing the attached Tab5. Work is
on branch `claude/perf-pass-2` (`worktrees/perf-pass-2`), based on v5.0.1
`94af0b8`. It is uncommitted, not pushed, and nothing was released. The
control checkout's paused JIT work on `codex/rv32-jit` was not touched; its
uncommitted edits to this file and to PERF_WORKLOG.md are not in this branch
and have to be merged by whoever integrates.

| | CPU | Graphics | Disk | Math | PR |
|---|---:|---:|---:|---:|---:|
| v5.0.1 | 0.804 | 0.490–0.494 | 1.031 | 9.42 | 0.764 |
| ROM and system code translated | 0.804 | 1.023–1.025 | 1.027–1.028 | 10.48 | 1.006 |
| Also Speedometer's own code | 1.603–1.604 | 1.012–1.014 | 1.024–1.030 | 11.11–11.12 | 1.334–1.335 |

Hot 68k code is translated to C++ at build time from the interpreter's own
handler bodies and entered through patched opcodes; nothing is generated at
run time. ROM regions are checked against the ROM at start-up. System and
application code in RAM is translated as position-independent modules,
installed where it is seen running and checked again after every guest cache
flush. The translated code runs from PSRAM. Design and rules:
[EMULATOR_PERFORMANCE.md](EMULATOR_PERFORMANCE.md#translated-code).
Measurements and rejected experiments:
[PERFORMANCE_REPORT.md](PERFORMANCE_REPORT.md). Procedure:
[tools/aot/README.md](tools/aot/README.md).

The CPU test is a sort loop in Speedometer itself. It moves only when that
loop is in the profile, so the middle row is what an unprofiled program
gets.

Verification: 55 host tests. Every translation passed the differential
self-test on the host and, built by the firmware toolchain, on the Tab5
(for the flashed image: 27,526 runs, 92.1 million instructions compared, no
mismatch, 57 trials skipped because they overwrote the code under test).
`PANEL VERIFY` 0/921,600 after every set of runs. Cold boot to Finder 49.6 s
(5.0.1: 49.5 s). Color QuickDraw monochrome 1.001, 8-bit 0.868 (5.0.1: 0.549
and 0.509).

Device state: the Tab5 runs the last row's image, `full-23`
(`artifacts/perf-pass-2/builds/full-23.bin`, debug profile, SHA-256
`2feab40d3f4168d72d9be071e8a0866f3abeec5d4a9cd9e16edafd01b07c0978`).
Only the application at 0x10000 was
written; bootloader, partition table, NVS, settings and the SD image were
not. To go back, flash
`artifacts/release-v5.0.1/debug-components/firmware.bin` at 0x10000.

Open points for Austin:

- `src/basilisk/aot/aot_rom_generated.cpp` is a translation of Apple's ROM
  and system software. It is git-ignored. Firmware built with it contains
  that translation; decide before publishing such a build.
- Whether the shipped profile should include Speedometer's own code.
- Waveshare and Rev3 hardware have not run this; their profiles build and
  pass `check_blob.py`. The Tab5 release profile (`esp32p4_pioarduino`) was
  built but not flashed: every measurement is of the debug profile.
- The repository's ESP-IDF project files (`CMakeLists.txt`, `main/`) list
  sources that no longer exist. They stopped an attempt to rebuild the SDK
  and are otherwise unused.

Evidence, every flashed image with its ELF and patch, profiles and scripts:
control checkout, `artifacts/perf-pass-2/` (`NOTES.md` is the index).

## v5.0.1 Performance release — 2026-09-27

Austin asked to raise Speedometer 4.02 Performance Rating CPU and Graphics
toward 1.0 from v5.0 without stability regressions. Austin then approved release
as 5.0.1, explicitly accepting that the Waveshare boards would not be
hardware-tested. Work is on branch `claude/speedometer-perf`
(`worktrees/speedometer-perf`), based on v5.0 `1468a91`. The control
checkout's own uncommitted work is untouched.

| | CPU | Graphics | Disk | Math | PR |
|---|---:|---:|---:|---:|---:|
| v5.0 | 0.545 | 0.244 | 1.02 | 6.68 | 0.458 |
| v5.0.1 | 0.803–0.804 | 0.495–0.497 | 1.029–1.033 | 9.43–9.48 | 0.765–0.766 |

The changes are an inline guest-RAM fast path with out-of-line IRAM thunks,
native Toolbox/OS trap dispatch guarded by a ROM signature check, cheaper
generated flags, and 295 hot handlers in IRAM. On the display side there are
32-pixel dirty spans with 32×40 tiles, a packed indexed tile writer and
uncached scanout writes. The release also fixes the frozen guest clock, which
costs 0.6% on Math. Documentation:
[EMULATOR_PERFORMANCE.md](EMULATOR_PERFORMANCE.md) (design and maintenance),
[PERFORMANCE_REPORT.md](PERFORMANCE_REPORT.md) (measurements and rejected
experiments) and [AUTOMATION.md](AUTOMATION.md) (`--suite rating`, `PERF`,
`PEEK`, `PANEL VERIFY`).

Verification: 52 host tests. All five profiles build with
`.build/release-tools/bin/pio` (PlatformIO Core 6.2.0); Core 6.1.18 cannot
install the Waveshare/rev3 platform. Eight soak ratings on the final
performance code each ended with 0/921,600 panel mismatches, with no resets
or crash markers. Color QuickDraw completed at all depths. Release gate for
the 5.0.1 debug app (SHA256
`9dcf67f6bb9fb2648b9bd86bbd7e797051eb0a682461d3364096af8d2beb5d2d`): app-only
flash, readback match, `Firmware: 5.0.1` banner, settings preserved, Finder
desktop, one Performance Rating (PR 0.765) and a clean `PANEL VERIFY`. The
Tab5 runs that image.

Release PR: https://github.com/amcchord/M5Tab-Macintosh/pull/22.
Evidence, packaging script and hardware gate are in the control checkout's
`artifacts/release-v5.0.1/`; per-build experiments are in
`artifacts/speedometer-perf/`. Publication outcome (commit, tag, asset
digests) is recorded in `artifacts/release-v5.0.1/release-status.json`.
To roll back, flash the matching v5.0 image, or the v5.0 debug app
`artifacts/release-v5.0/debug-components/firmware.bin` at 0x10000.

Risks: Waveshare and Rev3 images are build-validated only. IRAM grew ~46 KB,
and Tab5 internal SRAM free after init fell from 231 KB to 177 KB. Native
A-line QuickDraw stays disabled; it is unsafe on Mac OS 8 without
ROM-internal hooks.

Next: user hardware testing, especially on Waveshare.

## v5.0 Stable release — 2026-09-26

Austin approved the corrected Tab5 display and explicitly requested publication
as **5.0 - Stable**. Release work stays on `codex/display-reliability`; the
control checkout's uncommitted performance changes remain separate.

The release includes shared display publication, atomic damage and coherent
visual state, safe renderer/capture lifetimes, HID/key ownership fixes, and
native QuickDraw disabled by default after the Finder-border A/B test. A serial
startup banner identifies version 5.0. See RELEASE_NOTES.md for board images,
validation, limits and rollback. Publication evidence and packaged downloads
are retained under the control checkout's `artifacts/release-v5.0/`.

Final validation passed 46 host tests (7.126 seconds), the debug build,
and the standard board/silicon build matrix. Tab5 SD SPI now runs at 10 MHz
instead of 25 MHz following intermittent CRC/status failures during startup.
The final debug app is byte-identical to the tested diagnostic, SHA256
`cfbc1d1592df1d49463434525a132d99592d71c19e56f410e9ab9ad5d93ee213`.
Application readback matched, and original settings were preserved.

The earlier 25 MHz candidate sometimes displayed a guest illegal-instruction
or damaged-System-file dialog. A cold start did not consistently clear it;
Austin reported that another reboot got past it. The 10 MHz comparison then
completed three starts without errors in captured SD logs. Finder was visually
verified on the first and third starts; the second reached the normal
improper-shutdown notice caused by the test reset. See SD_STABILITY.md for
scope, evidence and limits. The original disk image was retained.

Release PR: https://github.com/amcchord/M5Tab-Macintosh/pull/21.
Next release operation: publish tag v5.0 as **5.0 - Stable** after validating
all packaged component hashes. Exact source/release commits and publication
outcome are retained in `artifacts/release-v5.0/release-status.json` and the
release's BUILD-MANIFEST.json. The original application backup remains under
`artifacts/display-reliability-20260926/hardware-flash/`.

PR #16 was reviewed separately. Its multi-block transfers affect preboot USB
Disk copying, not the emulator's SD file access. It remains unmerged; its
invalid derived-reference filesystem accessor should be replaced before
integration. Review evidence and a compiling accessor alternative are in
`artifacts/release-v5.0/pr16-review/`.


## Display reliability and QuickDraw regression — 2026-09-26

Active work is on local branch `codex/display-reliability`, based on v4.7.1,
in the attached checkout
`<Codex managed worktree>/M5Tab-Macintosh`.
The original control checkout's uncommitted performance work is preserved;
the release branch is separate and is published through PR #21.

[DISPLAY_RELIABILITY_REVIEW.md](DISPLAY_RELIABILITY_REVIEW.md) records the
v3.4.2–v4.7.1 audit, fixed defects, display ownership contract, verification
and hardware checklist. Both boards now share synchronous publication into
their panel-owned framebuffer. Damage, palette/mode, overlay snapshots,
shutdown and screenshot lifetime have explicit ordering. Conservative
QuickDraw guards and HID/key-ownership fixes cover adjacent corruption risks.

The final host suite passes 46 tests, including six native targets with
AddressSanitizer/UndefinedBehaviorSanitizer. All five firmware profiles build.
Build results and test logs are
in this checkout's ignored `artifacts/display-reliability/`. Candidate images
are retained under the control checkout's
`artifacts/display-reliability-20260926/`, with source SHA and checksums.

Austin subsequently authorized flashing the connected Tab5. The 82e5266
pre-v3 debug application was flashed and read back with an exact SHA256 match;
SD mounted, the emulator started, and existing settings were preserved.
Austin then reported dotted Finder borders. An A/B firmware comparison with
only `CPU_NATIVE_QD_ACCEL=0` eliminated them in the guest screenshot.

Native QuickDraw is now disabled by default in every firmware profile. At the
end of that comparison the Tab5 ran the matching no-acceleration build; [QUICKDRAW_REGRESSION.md](QUICKDRAW_REGRESSION.md)
records the comparison, image hashes and remaining limits. The previous app
partition is backed up under the artifact package's `hardware-flash/` directory.
The release-preparation section above supersedes this device state. Single-buffer optical tearing
and the performance cost of disabling QuickDraw still need measurement. The
device-state statements below are historical v4.7.1 evidence.

## v4.7.1 release — 2026-09-26

The control checkout is the project root. Branch
`codex/serial-debug-performance` starts at GitHub master `87ca223`. This pass
improves serial automation for screenshot/action/screenshot testing and all
persisted preboot settings. The repository owner authorized committing,
publishing, and releasing this work as patch version v4.7.1. The prior
local changes remain preserved in stash `cc5007c0020d8930ec65b2b96bee803af161d657`.

Protocol 4 adds 768-byte screenshot reads, CRC-checked selective retries,
buffered host reads, boot-session request IDs, and cached replies so lost
acknowledgements can be retried without repeating input. Version 3 clients
remain supported. Native USB/JTAG attaches now set DTR/RTS atomically on POSIX
hosts; sequential updates had reset the guest when reopening the port.
Abandoned frame leases expire, held input is released after inactivity, and
the panel resumes before the saved screenshot finishes transferring.

Entry points:

- `AUTOMATION.md`: setup, protocol, recovery semantics, MCP and benchmark use.
- `tools/mac_control.py`: persistent Python client, CLI and MCP server.
- `tools/debug_benchmark.py`: serial-only timings, failure counts and PNG evidence.
- `src/basilisk/automation.cpp`: firmware protocol and snapshot transfer.
- `src/basilisk/boot_gui.cpp`: queued settings requests, validation, SD persistence,
  and one-shot return to preboot.
- `test/test_boot_control.py`: host API contracts and native firmware value tests.

## Verification and device state

The attached pre-v3 M5Stack Tab5 (ESP32-P4 revision 1.3) is running the tested
`esp32p4_pioarduino_debug` build, using Arduino 3.3.8 / IDF 5.5.4. The original
GitHub Tab5 SDK failed to mount this unit's SD card. The opt-in debug profile
matches its prior SDK and leaves release profiles unchanged.

Startup also exposed a pre-existing double-delete of the boot touch task.
The decoded crash was in `uxListRemove` from `vTaskDelete` after the worker had
already deleted itself. A completion semaphore now makes queue cleanup safe;
this is the narrow lifecycle fix previously present in the saved local work.

The latest firmware built successfully and all 40 host/native tests passed:

```sh
pio run -e esp32p4_pioarduino_debug
python3 -m unittest discover -s test -p 'test_*.py'
```

Measured on the same Mac desktop scene at 640×360, with a persistent USB
connection (baseline: two captures per mode; final: twenty per mode):

| Operation | Baseline median | Final median | Final p95 |
| --- | ---: | ---: | ---: |
| Monochrome screenshot | 2.209 s | 0.196 s | 0.350 s |
| Color screenshot | 22.236 s | 0.546 s | 0.994 s |
| Ping | about 5 ms | 3.39 ms | 4.62 ms |

- All 40 final screenshots and 20 benchmark pings passed.
- 400 additional ping/mouse/key-release commands passed; five lost replies
  recovered through cached acknowledgements.
- A deliberately dropped TYPE acknowledgement retried once; visual evidence
  showed `Retry safe` exactly once. A stale boot-session command was rejected.
- Mouse menu opening, keyboard shortcut, text entry, and return to desktop
  were checked in screenshots. The old v3 client also captured successfully.
- Twenty close/open cycles retained the same firmware boot session.

Evidence is in ignored `artifacts/serial-debug/`: `final/results.json`,
`control-stress.json`, `reconnect.json`, `recovery-checks.json`,
`retry-safe-typing.png`, `final-desktop.png`, and build/test/upload logs.
The current flashed application (including preboot control) SHA256 is
`3cf143798ab01450fd32b4030ff93ff537220207772bd879bba64fda1ed700a6`.

## Preboot control verification — 2026-09-26

Serial now starts after SD/configuration initialization and before the settings
UI. CLI, Python, and MCP can query all selections, list/rescan SD media, change
all eleven persisted settings, save/reload, connect/disconnect WiFi, and boot.
Passwords are write-only. `boot-enter` restarts once into settings and holds
there; normal later power cycles retain the existing splash behavior.

Requests execute on the GUI/main CPU task through copied queues. A cheap pending
flag also services them between CPU batches and during STOP; waiting for the
old instruction-count maintenance tick timed out while the guest was idle.
Expired queued requests never execute later. Media/type/value checks run before
mutations; save verifies a temporary file before replacing the recoverable
previous file. Emulator input is gated until initialization completes. Serial
recovery remains available after emulator initialization failure or exit.

On the attached Tab5, the final build passed:

- Every RAM/rotation/boolean choice, all available disk/CD/folder selections,
  UTF-8/space-containing SSIDs, password set/clear, and reload restoration.
- Twelve malformed or invalid requests rejected, including credential reads;
  invalid CD boot prevented and guest input rejected before initialization.
- Media rescan and an MCP read through the actual serial connection.
- Modified RAM/audio/SSID persisted through a reboot; all original settings
  restored and verified through another reboot. No test credentials remain.
- 25 complete settings reads: median 100 ms. Remote preboot entry took 12.16 s
  including restart; emulator start took 1.67 s, before Finder readiness.
- Three final color screenshots: 0.50, 1.11, and 0.63 s, all successful.

Evidence and reproducible device-check scripts are under ignored
`artifacts/preboot-control/`, including `final-results.json`, `settings-checks.json`,
`final-desktop.png`, and build/upload/test logs. Device is restored to its
original settings and Mac desktop. These changes are included in v4.7.1.

The original device application was backed up to
`artifacts/serial-debug/device-original-app.bin` (SHA256
`a2e3768371c74ad5b5bcceddd7e81b76229d9ec200be7d9ada8324a343d986d3`).
To roll back that application only, close the serial client and use:

```sh
python3 -m esptool --chip esp32p4 --port PORT --baud 1500000 \
  write-flash 0x10000 artifacts/serial-debug/device-original-app.bin
```

The development pass changed only the attached device's application flash.
SD images, flash NVS, and the WiFi co-processor firmware were not reflashed.
The release includes all source, tests, documentation, and host tools from this
pass; older unrelated stashes remain preserved locally.

Release assets comprise four standard board/silicon merged images, a separately
labeled pre-v3 Tab5 USB debug merged image and application-only image, a small
host-tools ZIP, SHA256SUMS, and BUILD-MANIFEST.json. The debug application was
read back from the device and its SHA-256 matched the tested build above.
Standard builds retain their v4.7 SDK pins. The project-local Python 3.13 /
PlatformIO 6.2.0 / esptool 5.4.0 environment documented in README is used because
installing the older Tab5 platform downgraded the shared PlatformIO Core to
6.1.18, which cannot build the newer Waveshare platform.

`RELEASE_NOTES.md` records the user-facing patch scope and limitations. The
GitHub release/tag and attached BUILD-MANIFEST.json record the exact source
commit and download hashes. Build, packaging, and publication evidence is
retained in ignored `artifacts/release-preparation/`.

## Remaining scope

Screenshots still capture the guest framebuffer, not the physical preboot UI.
Fatal board/SD/ROM setup failures before the serial task starts remain outside
the API. USB Disk mode must be exited on the touchscreen before configuration
commands can access the SD card. WiFi credential persistence was tested; live
AP association and failure/power-loss injection into SD replacement were not.
UART/Waveshare, production P4 silicon, and long-duration soak tests were not
exercised on hardware. Request replay protection assumes one serialized client and caches
only the last tagged request. Hardware validation on those other targets and
longer soak tests remain follow-up work.
