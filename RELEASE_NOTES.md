# 5.4 beta 2 — Disk images, indexed color and USB Disk

This prerelease includes all four commits from the [Tab5 hardware handoff](https://github.com/amcchord/M5Tab-Macintosh/tree/cb3a03a8c7f838a852070d78e93fcc08e7627e60).

- Opening a Macintosh disk image preserves HFS volume flags and Finder information. The guest sees the volume state recorded on disk.
- Indexed color queries and panel scanout now use the same initial palette. The display renderer updates just the affected rows for sparse changes.
- USB Disk batches adjacent SD sectors and preserves neighboring bytes during partial-sector writes. The host tools include a write and cold-read checksum benchmark. The batching approach was also contributed by elwinloomis in [PR #16](https://github.com/amcchord/M5Tab-Macintosh/pull/16).
- The AOT tools report frequently executed instructions omitted from the translation. Profiling and self-test firmware can now be built for both supported Tab5 silicon families. Release firmware uses the same ROM and system translation as v5.3 and v5.4 beta 1.

## Upgrading an existing Macintosh disk

Choose **Special → Shut Down** in Finder and wait for the safe-to-power-off screen before flashing. Back up the SD card and disk image first.

A Mac OS 8.1 image that had been left mounted showed a flashing question-mark floppy on beta 2. Its primary HFS header recorded an unclean volume. On the attached Tab5, a duplicate of that image booted with v5.3. After a clean Finder shutdown on v5.3 the same duplicate booted on beta 2 with its open folders restored. If you encounter this symptom, preserve the original and perform recovery on a duplicate.

## Verification

All 73 host tests passed. The host differential test compared the release translation with the interpreter over 131,824 trials and 506,880,417 instructions, with zero failures and 108 skips. All five firmware profiles passed the translated-code checks. Image checksums and the bootloader, partition table and application offsets were verified during packaging.

The pre-v3 Tab5 standard and USB Debug applications both booted Mac OS 8.1 with 24 MB of guest RAM on an ESP32-P4 v1.3. Both installed 537 of 537 translated ROM regions. A new Finder folder and its SimpleText document survived a clean shutdown and reopened on the other build.

Nine Startup Settings views passed full-panel comparison, including USB Disk entry and exit. All four indexed depths matched all 921,600 pixels on settled screens in both rotations. One sample taken during a 16-color transition had 4,816 mismatches and passed on two subsequent reads. The host screenshot tool now checks that Startup Settings actually returns when the Tab5 restarts before acknowledging the USB Disk Done button.

One Speedometer 4.02 Performance Rating on the standard Tab5 application was 0.908 (CPU 0.802, Graphics 0.810, Disk 0.900, Math 10.134). This is a single run on the test copy.

Waveshare and Rev3 profiles received build and image verification only. USB-A transfer speed, the After Dark Star Trek TNG module, optical tearing and physical touch gestures were not tested. The Tab5 logged display underruns at startup on both v5.3 and beta 2. [Hardware test details](https://github.com/amcchord/M5Tab-Macintosh/blob/v5.4-beta.2/TAB5_HARDWARE_HANDOFF.md).

## Downloads

| Hardware or tool | Download | Flash offset |
| --- | --- | --- |
| Tab5, pre-v3 | `M5Tab-Macintosh-v5.4-beta.2.bin` | `0x0` |
| Tab5, v3.1 or newer | `M5Tab-Macintosh-Rev3-v5.4-beta.2.bin` | `0x0` |
| Waveshare 10.1, pre-v3 | `M5Tab-Macintosh-Waveshare-P4-10.1-v5.4-beta.2.bin` | `0x0` |
| Waveshare 10.1, v3.1 or newer | `M5Tab-Macintosh-Waveshare-P4-10.1-Rev3-v5.4-beta.2.bin` | `0x0` |
| Tab5, pre-v3, USB Debug | `M5Tab-Macintosh-USB-Debug-v5.4-beta.2.bin` | `0x0` |
| USB Debug application for an existing compatible standalone Tab5 installation | `M5Tab-Macintosh-USB-Debug-v5.4-beta.2-app.bin` | `0x10000` |
| Serial control, benchmarks and instructions | `M5Tab-Macintosh-Host-Tools-v5.4-beta.2.zip` | — |
| Brightness Control Strip module | `M5Tab-Macintosh-Mac-Extras-v5.4-beta.2.zip` | — |

Use the ESP32-P4 chip revision to select firmware. The Rev3 images require v3.1 or newer. Merged images replace the bootloader and partition table, including any launcher. The application-only image requires this project's compatible partition map and bootloader. See the [flashing instructions](https://github.com/amcchord/M5Tab-Macintosh/blob/v5.4-beta.2/README.md#flashing-the-firmware).

`SHA256SUMS` covers the downloads and build manifest. `BUILD-MANIFEST.json` identifies the source commit, component hashes and device checks. The private generated translation has SHA-256 `7fb5dfeea3f9d38c3a8901f216d17f4520736bdc5035bda0fbecc6588595e3dc`. A fresh source checkout uses the interpreter.

For rollback, flash the matching board and chip image from [stable v5.3](https://github.com/amcchord/M5Tab-Macintosh/releases/tag/v5.3). Restore an SD backup to revert guest files or settings.
