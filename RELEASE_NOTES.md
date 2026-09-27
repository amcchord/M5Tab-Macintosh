# 5.0 - Stable

A stability release focused on correct drawing and reliable display updates
on M5Stack Tab5 and Waveshare ESP32-P4 10.1 boards.

## What changed

- **Fixed dotted Finder borders.** Native QuickDraw acceleration is disabled
  by default. A hardware A/B comparison showed alternating black/white pixels
  around scrollbars with acceleration enabled and solid borders through the
  original Mac QuickDraw path. This may reduce graphics throughput; earlier
  native-acceleration benchmark scores do not apply to this release.
- **One display publication path.** Both boards write into the panel driver's
  framebuffer with synchronous cache publication. Duplicate asynchronous DMA
  scratch-buffer paths and unused buffers have been removed.
- **Reliable dirty tracking.** Bulk writes mark every affected tile in all
  indexed color depths. Writes arriving during rendering remain pending, and
  failed publication is retried. Early frame notifications cannot strand updates.
- **Coherent visual state.** Mode and palette changes are captured together.
  Tab5 tiles follow the saved rotation, and touch overlays use a stable state
  snapshot throughout each frame batch. Hiding overlays redraws their old area.
- **Safer startup and shutdown.** Splash handoff handles packed stipples and
  idle timeouts. Video initialization reports task failures; exit waits for
  rendering to finish and protects concurrent screenshot access.
- **Input fixes.** Mouse fields stay within their report IDs; wrong-ID and
  truncated reports are ignored. Shared physical/automation key ownership and
  event ordering are serialized.
- **Version identification.** The serial startup banner identifies firmware
  version 5.0. The existing serial automation protocol and preboot controls remain.

See the [display review](DISPLAY_RELIABILITY_REVIEW.md) and
[QuickDraw regression record](QUICKDRAW_REGRESSION.md) for implementation and
comparison details. Board-specific SDK pins are unchanged.

## Downloads

| Hardware / use | Filename | Flash offset |
| --- | --- | --- |
| Tab5, pre-v3, standard SDK | `M5Tab-Macintosh-v5.0.bin` | `0x0` |
| Tab5, v3.1+ | `M5Tab-Macintosh-Rev3-v5.0.bin` | `0x0` |
| Waveshare 10.1, pre-v3 | `M5Tab-Macintosh-Waveshare-P4-10.1-v5.0.bin` | `0x0` |
| Waveshare 10.1, v3.1+ | `M5Tab-Macintosh-Waveshare-P4-10.1-Rev3-v5.0.bin` | `0x0` |
| Tab5, pre-v3, tested USB debug SDK | `M5Tab-Macintosh-USB-Debug-v5.0.bin` | `0x0` |
| Same debug application, existing compatible Tab5 installation | `M5Tab-Macintosh-USB-Debug-v5.0-app.bin` | `0x10000` |
| Python CLI, MCP server, and benchmark | `M5Tab-Macintosh-Host-Tools-v5.0.zip` | — |

Chip revision is separate from panel or PCB revision. Rev3 images require
ESP32-P4 revision **3.1 or newer**; revision 3.0 is not covered. Merged images
include bootloader, partitions and application, and replace an existing launcher.
The debug application-only image preserves bootloader, partition table and NVS
on a compatible installation. Neither package includes a Macintosh ROM, disk
images or credentials.

The optional pre-v3 Tab5 debug profile uses Arduino 3.3.8 / IDF 5.5.4 and matches
the development device used for hardware testing. The standard Tab5 profile
retains its separate SDK pin for boards affected by other display behavior.
Use the matching board/silicon image; the debug image is not for Waveshare or
production Rev3 hardware.

## Verification

- 46 host tests, including production display/overlay code exercised with
  AddressSanitizer and UndefinedBehaviorSanitizer, randomized dirty ranges and
  rotation, packed color modes, publication failure/retry, lifecycle failures,
  HID report IDs, concurrent key ownership and release-image checks.
- All five firmware profiles build. Every merged image is checked against its
  bootloader, partition table and application at the expected flash offsets;
  linked SDK silicon minimums are checked separately.
- Hardware checks on a pre-v3 ESP32-P4 revision 1.3 Tab5: application flashing,
  readback verification, SD mounting, saved-setting preservation, emulator boot,
  serial screenshots and the Finder border comparison. The user confirmed the
  corrected display before requesting the stable release.
- `BUILD-MANIFEST.json` records exact source commits, component hashes and the
  hardware-tested application. `SHA256SUMS` covers downloadable assets.

## Limits and rollback

This is a single scanout buffer; transient optical tearing is still possible.
Serial screenshots show guest pixels, not physical scanout or touch overlays.
The native QuickDraw implementation remains available for investigation but
must pass differential checks against guest rendering before being re-enabled.

Waveshare and production-silicon targets are build-validated, not newly tested
on physical hardware in this pass. Broader application coverage, long-duration
soak testing and the performance cost of the conservative drawing path remain
follow-up work.

The prior [v4.7.1 release](https://github.com/amcchord/M5Tab-Macintosh/releases/tag/v4.7.1)
remains available for rollback with a matching board/silicon image. Preserve
known-good firmware and SD backups. Firmware rollback does not automatically
revert settings or guest disk contents. [Serial control reference](AUTOMATION.md).
