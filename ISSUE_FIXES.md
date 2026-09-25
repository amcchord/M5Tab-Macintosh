# September 2026 issue review and v4.7 stable release

Work is isolated on `codex/issue-fixes-release`, based on the v4.6/master
commit `44b9bc081a286387126006abf2775d69cfed892e`. The original checkout's
uncommitted development work was not included. Release and issue feedback were
requested by the repository owner.

## Stable promotion — September 25, 2026

The repository maintainer reported that all tested changes work, excluding
Elecrow support (#10), and requested a full release. v4.7 promotes the four
v4.7-beta.1 firmware binaries without rebuilding them. SHA-256 hashes must match
the published beta assets exactly; only filenames and release metadata change.
The stable tag also includes documentation updates; firmware source remains
`4f067e6f72f2c38fb6d047e159e691568bcf2746` (the beta release commit).

Issues #17, #13 and #15 are resolved following maintainer confirmation. #10
remains open and unsupported. The confirmation does not specify individual
board/chip combinations, so no more detailed hardware test matrix is claimed.

## Issue outcomes

| Issue | Implemented behavior | Status / remaining work |
|---|---|---|
| [#17: production P4 boot failure](https://github.com/amcchord/M5Tab-Macintosh/issues/17) | Add two Rev3 environments with the production SDK (minimum chip revision 3.1), preserve older-silicon environments, remove the Waveshare PHY override, and backport M5GFX's zero-initialized DSI config. Remove the hard-coded SDK include path that could mix chip variants. | Maintainer confirmed working; included in v4.7 stable. |
| [#13: cannot change time](https://github.com/amcchord/M5Tab-Macintosh/issues/13) | Implement complete four-byte guest RTC writes, monotonic clock advancement, NVS persistence, and the same clock in the boot menu. Historical dates remain valid. | Maintainer confirmed working; included in v4.7 stable. The software clock still does not advance while powered off. |
| [#15: Maelstrom boot loop](https://github.com/amcchord/M5Tab-Macintosh/issues/15) | Validate guest audio descriptors and complete sample ranges on both boards; avoid undefined signed shifts in 8-bit audio conversion. Enter settings after crash resets or with an SD recovery file, and allow a splash tap to override `skip_gui`. Document PRAM-only recovery and direct-flash comparison. | Maintainer confirmed the beta changes work and approved stable promotion. No disk image is automatically reset or repaired. |
| [#10: Elecrow CrowPanel 9-inch](https://github.com/amcchord/M5Tab-Macintosh/issues/10) | Reviewed the vendor repository and current board abstraction; identified required port work. | Confirm PCB revision and a working vendor example, then implement and test a dedicated backend. No Elecrow firmware is shipped. |

## Elecrow port findings

The [official vendor repository](https://github.com/Elecrow-RD/CrowPanel-Advanced-9inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen)
contains schematics for PCB revisions 1.0, 1.1 and 1.2. Its
[v1.2 driver manual](https://github.com/Elecrow-RD/CrowPanel-Advanced-9inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen/blob/master/Eagle_SCH%26PCB/1.2/readme.md)
describes an EK79007-compatible 1024×600 panel and GT911 touch. The issue names
an NS4168 audio device, whereas our Waveshare path uses ES8311 and Tab5 uses
ES8388. This needs board-specific verification rather than reuse of either
existing binary.

The current renderer assumes integer 2× scaling and a 1280-pixel output width.
Retaining a 640×400 Mac screen would require 1.5× scaling to 960×600 with 32-pixel
side margins, corresponding touch transforms, and a boot-settings layout that
fits a 600-pixel display. The SD, hosted C6, backlight, panel, touch, and audio
pin assignments must come from the confirmed PCB revision.

The reporter has been asked for the PCB version, ESP32-P4 revision, and a link
to the vendor firmware/example they have successfully run. Their offered
hardware testing is necessary to validate the port.

## Release validation and operation

- `python3 -m unittest discover -s test -v`: 19 host tests, including C++ address
  and undefined-behavior sanitizers, clock persistence/failure handling, DSI
  patch drift detection, and merged-image validation.
- `bash -n scripts/build_release.sh` and `git diff --check`.
- Build all four images with `scripts/build_release.sh v4.7-beta.1 all`, using
  Python 3.13, PlatformIO 6.2.0, and esptool 5.4.0. The older installed
  PlatformIO 6.1.18 is incompatible with the Waveshare platform requirement;
  Python 3.14 is rejected by these pinned platforms. The README documents a
  project-local build environment.
- Packaging fails on merge errors, validates exact component bytes at
  `0x2000`, `0x8000`, and `0x10000`, and checks the linked SDK's minimum silicon
  revision. Per-image JSON records include variant and component SHA-256s;
  release checksums identify the final downloadable images.
- Local build/test evidence is retained in `artifacts/issue-fixes/` (ignored).
  The public release records its exact source commit and validation outcome.
- No physical tablet was connected during the original agent build. The
  maintainer subsequently tested the beta and authorized stable promotion.
  Preserve those tested binaries; a firmware rebuild is not part of promotion.

Backup/rollback: keep the v4.6 release and existing SD images. Users should back
up their SD card before game/disk recovery testing. Older-silicon users can
reflash v4.6 at `0x0` if v4.7 regresses. v4.6 is not a working fallback on
Rev3 silicon; those users should keep their previously working firmware. A
direct merged-image flash replaces M5Launcher and may clear device NVS. Never
erase the SD or reset guest PRAM automatically.

Stable publication procedure: merge the documentation-only promotion branch,
tag it as `v4.7`, upload the four unchanged binaries with stable filenames and
updated checksums/metadata, verify uploaded SHA-256 values, and publish as the
latest stable release. Retain v4.7-beta.1 and v4.6 as historical releases.
Update #17, #13 and #15 with the stable link and close them following maintainer
confirmation; leave #10 open. Promotion evidence is retained in
`artifacts/v4.7-stable/`.
