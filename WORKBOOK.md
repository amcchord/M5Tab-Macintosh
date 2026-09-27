# M5Tab-Macintosh workbook

## Display reliability and QuickDraw regression — 2026-09-26

Active work is on local branch `codex/display-reliability`, based on v4.7.1,
in the attached checkout
`/Users/austinmcchord/.codex/worktrees/display-reliability/M5Tab-Macintosh`.
The original control checkout's uncommitted performance work is preserved;
these changes have not been integrated with it or pushed.

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

Native QuickDraw is now disabled by default in every firmware profile. The
Tab5 is running the matching no-acceleration build; [QUICKDRAW_REGRESSION.md](QUICKDRAW_REGRESSION.md)
records the comparison, image hashes and remaining limits. The previous app
partition is backed up under the artifact package's `hardware-flash/` directory.
Next: Austin continues physical display testing. Single-buffer optical tearing
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
