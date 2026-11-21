# Tab5 hardware handoff

## Beta 3 on a pre-v3 Tab5

The test board has an ESP32-P4 v1.3 and the stock ESP32-C6 radio firmware. The original card was put aside after its settings and PRAM were restored. Installation and formatting used a separate 29,664 MiB card.

- **Formatting and storage.** The Tab5 erased the spare, created a full-size FAT32 volume with 32 KiB clusters, and opened **Set Up SD Card**. With the radio enabled, the same 4 MiB file test measured 10,198 KiB/s verified reads and 1,369 KiB/s synchronized writes on four-bit SDMMC. Legacy SPI measured 1,759 and 920 KiB/s respectively. [Card measurements](SD_STABILITY.md)
- **Disk performance inside the Mac.** On the same card, two four-bit System 7.5.3 runs read at 6,468–6,479 KiB/s and wrote and flushed at 1,105–1,474 KiB/s. Legacy SPI measured 1,437 and 697 KiB/s. Each run verified all 4 MiB. The CPU checksum took about 0.42 seconds in both modes. [Guest benchmark](SD_STABILITY.md#disk-performance-inside-system-753)
- **System 7 first boot.** The installer downloaded the ROM and the 115,542,761-byte compressed System 7.5.3 disk, expanded it to 1,048,576,000 bytes, reread it, and booted Finder. Installing the system disk took 14 min 43 s from the first status sample to completion. Within each phase the sampled rates were 231 KiB/s downloading, 3.73 MiB/s expanding, and 9.48 MiB/s verifying. All 921,600 panel pixels matched the expected display.
- **Mac OS 8.1 installation.** Setup verified the 203,003,478-byte compressed download, expanded it to 1,048,576,000 bytes and verified the installed disk by reading it back. On four-bit SDMMC, expansion averaged 3.28 MiB/s and readback averaged 9.44 MiB/s between status samples. Finder opened the MicroMac volume and all 921,600 panel pixels matched.
- **Speed display and resume.** During the Mac OS 8.1 download, the setup screen showed 273.2 KiB/s at 1%. Cancelling preserved 3,473,408 compressed bytes. After a firmware update, setup resumed at that exact byte count. It initially reported zero and then 179.1 KiB/s as new bytes arrived. The rate disappeared while the device verified the ROM. A second update with 181,413,082 bytes saved also resumed at that exact offset. [Device screenshot](screenshots/Tab5-Starter-Download.png)
- **Mac OS 8.1 recovery.** Both the published disk and a copy of a previously failing user disk reached Finder through the guest's volume check. The HFS header on each disk recorded a clean unmount after Finder shut down.
- **System 7.5.3 recovery.** An interrupted startup of the previously repaired starter disk reproduced the flashing question mark. Its captured filesystem metadata passed an independent HFS check. Firmware recognized that metadata, changed the shutdown flag and booted the same disk to Finder. [Recovery criteria](HFS_STARTER7.md#interrupted-startup-after-the-first-repair)
- **Wi-Fi firmware update.** The standard build received the 3,250,928-byte USB Debug application over the stock C6 radio. From the first connection status to verified boot selection took 23.6 seconds. The board restarted into the updated slot. Cancelling a separate transfer kept the running slot selected. [Update procedure](WIFI_UPDATE.md)


Start with a spare SD card and a copy of a known-good Macintosh disk. The first device checks are whether Finder opens the same folders after a clean restart and whether After Dark renders space black. Both can look fine in a host test while failing on a real installation. Then measure how much work the display and USB changes save on this particular Tab5.

This branch starts at `3641580d72982cec464d4800f2ead507ba7c687d` (`v5.4-beta.1`). The evidence collected during development is from host tests and firmware builds. The table lists the original test plan. The beta 2 results below cover the tests completed on the attached Tab5.

| Change | Evidence already available | What the hardware owner can establish |
| --- | --- | --- |
| Opening an HFS disk image preserves its contents | Byte-for-byte tests of writable and read-only opens, including a dirty volume with a different backup header | Normal Finder persistence and guest handling of an unclean volume |
| Indexed palettes use the same initial colors for guest queries and scanout | The real Mac video driver switches depths, reads colors, resets gamma and changes a single palette entry in the host test | Whether the After Dark Star Trek TNG module now displays the intended colors |
| The renderer narrows sparse damage to the affected rows | Exact pixel and payload checks across four depths, both orientations and both supported scales, including a retry | Render time and responsiveness during typing, window movement and dense animation |
| Preboot USB Disk batches adjacent sectors | A 4 KiB aligned request reaches FatFs in one call. Partial-sector writes preserve neighboring bytes in a simulated card. | Read and write rates, eject behavior and a cold-read checksum on the Tab5 SPI card |
| The CPU translator reports hot instructions omitted from its output | Synthetic 68k programs are compared with the interpreter. Tests cover entry limits and collecting the last chunk of guest RAM. | Finder and drawing profiles, an independently timed third application, and a differential run with the owner's ROM |

All 73 host tests passed on macOS. The four board/silicon release environments, the pre-v3 Tab5 debug environment and the four Tab5 AOT profiling/self-test environments all built from a clean source configuration with PlatformIO 6.2.0. The linker maps for both production AOT builds reference the production SDK. Both pre-v3 maps reference the engineering-sample SDK. On the same synthetic ROM and RAM profiles, the baseline and candidate translators emitted byte-identical C++.

The host differential test also ran that translation for 12 trials per entry. It compared 4,133,446 instructions over 20,578 runs with zero failures and 208 skips. A separate production-silicon self-test firmware linked the same synthetic translation. Its relocation check passed for 626,436 bytes across 1,046 functions. Running the self-test on a Tab5 with the owner's ROM and applications is still pending.

To isolate a behavior change with Git, the storage commit is `f68a961`, the display commit is `7ae81c4`, and the profiling tools and build environments landed in `acb0298`.

## Beta 2 device results

The pre-v3 standard and USB Debug applications both ran on an ESP32-P4 v1.3 Tab5 with 24 MB of guest RAM, a Quadra 650 ROM and Mac OS 8.1. Both application digests matched after flashing. The test disk was a verified copy of the installed image.

- The existing disk had its HFS clean-unmount bit cleared. The guest showed a flashing question-mark floppy on beta 2. The same copy booted on v5.3. A clean Finder shutdown on v5.3 allowed beta 2 to boot that copy. Preserve the original image when testing recovery.
- The USB Debug build opened a newly created Finder folder and saved a SimpleText document. After a clean shutdown, the standard build restored the folder and reopened the saved text.
- Nine preboot screens, including USB Disk entry and exit, passed panel verification. All four indexed depths passed at 180° on USB Debug and at 0° on the standard application. At 0° one sample during the 16-color transition had 4,816 mismatched pixels. Two subsequent reads and a second selection of 16 colors matched all 921,600 pixels.
- The board logged display underruns during startup, also seen on v5.3. These readbacks verify settled framebuffer contents. Optical behavior and physical touch gestures need separate observation.
- Only USB-C was attached. USB-A file transfers and the cold-read USB Disk benchmark remain unmeasured. The test image had After Dark 2.0x disabled and did not contain the reported Star Trek TNG module. Its colors remain unverified.
- The standard application completed Speedometer 4.02 with a Performance Rating of 0.908. CPU was 0.802, Graphics 0.810, Disk 0.900 and Math 10.134. There was no matched baseline run.
- The release uses the existing translation. The host differential comparison with a patched Quadra 650 ROM passed 131,824 runs with zero failures. New workload profiling, a device differential self-test and training-versus-holdout timings remain open.

## Starter download and Finder display check — 2026-11-21

The public `sdCard.zip` download matched the local archive byte for byte by SHA-256 (`1417a88acb433927d6a6ea7dd487ddbf6fe9a6f2afbb879eac49f16dbfb373e8`). Both `Macintosh7.dsk` and `Macintosh8.dsk` have primary HFS volume flags of `0x0000`. The saved image from the beta 2 hardware test also has those flags. Only that last image has the recorded beta 2 boot failure. The [startup disk checker](tools/check_startup_disk.py) reads the primary header from an offline image or ZIP. Before replacing the starter download, boot copies of both images, shut down in Finder, recheck their headers and cold-boot each on the Tab5.

With Finder running on the attached Tab5, 2 of 30 panel checks showed differences with the Control Strip open, 0 of 30 with it collapsed, and 2 of 30 after reopening it. All four first differences were inside the strip at guest rows 337–338. Each check compared 921,600 panel pixels. The guest continues drawing during this comparison. Collapse the strip for a quiet Finder baseline and record its state when comparing builds. This test used the running firmware and ended in the original boot session with the strip open. Private captures are under `artifacts/startup-disk-check/` in the control checkout.

## Get the exact checkout and record the baseline

```sh
git clone --branch codex/tab5-hardware-handoff https://github.com/amcchord/M5Tab-Macintosh.git
cd M5Tab-Macintosh
RUN="$PWD/artifacts/tab5-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN"
git rev-parse HEAD > "$RUN/candidate-commit.txt"
git worktree add --detach ../M5Tab-v54-baseline 3641580d72982cec464d4800f2ead507ba7c687d

python3.13 -m venv .build/release-tools
. .build/release-tools/bin/activate
python -m pip install platformio==6.2.0 esptool==5.4.0 -r tools/requirements.txt
pio device list
```

Set `PORT` to the USB-C programming/console port listed on this machine. Use one serial client at a time. While the existing Mac is running, record its settings and a reference screenshot. Shut down from the Mac's Special menu before checking the silicon revision with esptool, which can reset the board.

```sh
PORT=/dev/cu.usbmodemXXXX
python tools/mac_control.py --port "$PORT" --no-reset info > "$RUN/original-info.txt"
python tools/mac_control.py --port "$PORT" --no-reset boot-get > "$RUN/original-settings.json"
python tools/mac_control.py --port "$PORT" --no-reset screenshot --color "$RUN/original.png"
esptool --chip esp32p4 --port "$PORT" chip-id
```

| Tab5 silicon | Normal build for comparisons (`M5_ENV`) | Collection build (`PROFILE_ENV`) | Differential test build (`SELFTEST_ENV`) |
| --- | --- | --- | --- |
| Pre-v3 engineering sample | `esp32p4_pioarduino_debug` | `esp32p4_aot_profile` | `esp32p4_aot_selftest` |
| Production v3.1 or newer | `esp32p4_pioarduino_rev3` | `esp32p4_aot_profile_rev3` | `esp32p4_aot_selftest_rev3` |

These are chip revisions. A panel or PCB version cannot identify the correct SDK. The project has no image for silicon v3.0. Record the chip ID, panel version if known, Mac OS version, ROM identity, guest RAM, SD card, disk image hash and Control Strip state. Preserve the original card, settings and matching known-good firmware. Store ROMs, disk images and captured guest code locally under the ignored `artifacts/` directory. [Existing installation and rollback details](RELEASE_NOTES.md)

## Build and switch between versions

Set the three environment variables from the matching row above. These example values are for **pre-v3**.

```sh
M5_ENV=esp32p4_pioarduino_debug
PROFILE_ENV=esp32p4_aot_profile
SELFTEST_ENV=esp32p4_aot_selftest

python -m unittest discover -s test -p 'test_*.py' -v
(cd ../M5Tab-v54-baseline && pio run -e "$M5_ENV")
pio run -e "$M5_ENV"

mkdir -p "$RUN/baseline" "$RUN/candidate"
cp ../M5Tab-v54-baseline/.pio/build/"$M5_ENV"/firmware.{bin,elf} "$RUN/baseline/"
cp .pio/build/"$M5_ENV"/firmware.{bin,elf} "$RUN/candidate/"
shasum -a 256 "$RUN"/{baseline,candidate}/firmware.bin
```

The command examples use Bash or Zsh. Both clean checkouts build with the interpreter. The private, generated AOT source used for published firmware is excluded from Git. To compare against a release-speed baseline, use the identical generated source in both worktrees for the storage and display tests, and record its SHA-256. Build and save the two images again after adding it. Changing the translation during that comparison would confound the results. [Release translation hash](RELEASE_NOTES.md)

On an existing standalone installation with this project's matching bootloader and partition map, flash **one application** at `0x10000`. The example installs the candidate. Use `$RUN/baseline/firmware.bin` for the control run. The project's merged downloads use a different offset and replace the bootloader. A launcher-managed installation needs its own installation procedure. [Partition map](partitions.csv) · [Flashing guide](README.md#flashing-the-firmware)

```sh
APP="$RUN/candidate/firmware.bin"
esptool --chip esp32p4 --port "$PORT" --after no-reset write-flash 0x10000 "$APP"
esptool --chip esp32p4 --port "$PORT" verify-flash 0x10000 "$APP"
```

Tap the Happy Mac splash to enter Startup Settings, select the copied disk, and start the emulator. To capture a log while exercising the device by hand, run `pio device monitor --port "$PORT" --baud 115200 | tee "$RUN/candidate-serial.log"`. Close the monitor before using the automation client. If the port changes after reset, run `pio device list` again. [Serial control guide](AUTOMATION.md)

## 1. Check HFS and Finder persistence

The old writable open could copy stale attributes from the alternate HFS master directory block to the primary block at byte 1024 and clear two Finder information fields. That could discard saved folder IDs and obscure a genuine improper shutdown. The backend now leaves volume management to Mac OS. [Disk backend](src/basilisk/sys_esp32.cpp) · [Byte-preservation fixtures](test/test_storage.py)

1. Make independent baseline and candidate copies of the same source image. Keep the master outside the SD card. Boot the candidate, open two identifiable folders, position their windows, make a small file with known contents, and choose **Special → Shut Down**.
2. Boot that copy again. Record which windows Finder restores and verify the saved file. Use the same Finder preferences and actions on the baseline copy. Capture any unexpected alert verbatim.
3. If a previously unclean test image is available, boot a duplicate and record whether Mac OS asks to check it. Run Disk First Aid's verification on the duplicate if available. An improper-shutdown warning is a legitimate guest result. Save the duplicate before attempting a repair.
4. To isolate open from guest activity, run `python -m unittest discover -s test -p 'test_storage.py' -v`. It compares every byte before and after the production backend opens and closes a fixture, then checks the exact two bytes written during an explicit guest write. A full Mac boot legitimately changes disk contents.

If SD CRC/status failures appear, keep the serial lines from card initialization through the first guest error. The emulator's normal disk-file I/O and preboot USB Disk use different paths. [Prior Tab5 SD investigation](SD_STABILITY.md)

## 2. Check colors, rotation and redraws

Open the Monitors control panel. Cycle through 256, 2, 4 and 16 colors, then back to 256. At each depth, inspect Finder text, the white window background and black outlines, switch to an application and back, and record the depth. Repeat in the other Tab5 orientation. Toggle the touch keyboard or game overlay and dismiss it. Look for old keys, edges or isolated strips left behind.

Hide the overlay, stop animation and use a settled screen for the following captures. Give each set a distinct name. The panel capture reads the memory used for scanout. A phone photograph can additionally show a panel issue that a memory readback misses.

```sh
CASE=depth8-normal
python tools/mac_control.py --port "$PORT" --no-reset screenshot --color "$RUN/$CASE-guest.png"
python tools/mac_control.py --port "$PORT" --no-reset screenshot --panel "$RUN/$CASE-panel.png"
python tools/mac_control.py --port "$PORT" --no-reset panel-verify > "$RUN/$CASE-verify.json"
```

On a settled Tab5 expect `checked=921600` and `mismatched=0`. Both images use the live palette, so agreement can coexist with an incorrect guest color. For the reported After Dark problem, run **Star Trek TNG** at the same depth on the baseline and candidate. Record module and After Dark versions, whether empty space is black, and how a second module behaves at that depth. Capture the same scene or pause at a comparable point. If the module is unavailable, leave its result pending. [Original After Dark report](https://github.com/amcchord/M5Tab-Macintosh/issues/28)

For display timings, collect at least three complete 30-second intervals per workload, separate from screenshot capture. Try an idle Finder, typing, dragging a narrow selection, then scrolling a full window. Keep the Control Strip, overlay, rotation and depth fixed. Record `[VIDEO PERF]` frame counts, average detection/render microseconds, and the candidate's `panel_bytes` per interval. Divide bytes by elapsed time when comparing intervals of different length. Baseline logs lack the byte field. Sparse writes should reduce payload most. Large redraws are the check for a regression. [Counter definitions and host result](EMULATOR_PERFORMANCE.md#dirty-spans)

## 3. Measure USB Disk with an eject and verified readback

Run this in **preboot USB Disk** on a spare card with at least 128 MiB free per retained sample. Enter USB Disk first so the Tab5 turns off its own USB-A power output. Then connect the Tab5's **USB-A** port to the host with a data-capable USB-A-to-USB-C cable. The Tab5's USB-C port can keep serving serial. Record the card model, format, host OS, cable and negotiated USB speed if available.

Create the host reference once. Set `VOLUME` to the card's actual mount point and confirm it in the host's disk utility. A missing mount or a path on the host's internal drive invalidates the timing. The writer creates a new file exclusively and includes a host flush in the timed copy.

```sh
dd if=/dev/urandom of="$RUN/reference-128MiB.bin" bs=1048576 count=128
VOLUME=/Volumes/TAB5
SAMPLE=candidate-1
CARD_FILE="$VOLUME/tab5-$SAMPLE.bin"
python tools/usb_disk_benchmark.py write "$RUN/reference-128MiB.bin" "$CARD_FILE" > "$RUN/$SAMPLE-write.json"
```

Eject the volume in the host OS. Disconnect the data cable and tap **Done**. The Tab5 restarts into settings to release its USB controller. Re-enter USB Disk and reconnect. Confirm the new mount path, updating `CARD_FILE` if needed. Read before opening the file in any other program.

```sh
python tools/usb_disk_benchmark.py read "$RUN/reference-128MiB.bin" "$CARD_FILE" > "$RUN/$SAMPLE-read.json"
```

A successful read exits zero and records the reference and on-card SHA-256 values. Preserve both JSON files. Perform three write/eject/read rounds per firmware, using a fresh sample name each time. Remove only your test files after verification if space is tight. Compare median MiB/s and the individual runs. A checksum failure, capacity mismatch, timeout or failed eject takes precedence over timing. Check that the next boot reads the card and a USB keyboard works again. The batching idea and separately measured Waveshare results came from elwinloomis. [PR #16](https://github.com/amcchord/M5Tab-Macintosh/pull/16)

## 4. Profile Finder and an application, then time an independent workload

Use a fixed copied disk and a written list of actions. An example set is below. Choose applications already installed on the owner's Mac. Keep the same documents, window sizes, inputs and end conditions for each build.

| Workload | Repeatable actions | Use |
| --- | --- | --- |
| Finder | Open the same populated folder in list view, scroll to the bottom, select all, deselect, close the window. Repeat ten times. | Training |
| Drawing application | Open the same document, run a fixed series of fills and scrolls, then close without saving. Repeat ten times. | Training |
| Text application | Open a fixed large document, run the same series of Find Next commands, then go to the last page. Record the final match and screen. | Timing only, held out of translation |

Collect with `$PROFILE_ENV`, flashed using its application binary as above. Reach the starting screen before each `start`. Each `start` clears the counters. Stop immediately at the end of the sequence. Save the current guest RAM bytes before closing the application or launching another one. The example translator settings and default ROM dump address/size are for the Quadra 650 setup. Supply the matching address, size and translator `--rom-base` for another ROM.

```sh
pio run -e "$PROFILE_ENV"
# Flash .pio/build/$PROFILE_ENV/firmware.bin, boot the copy and prepare Finder.
python tools/aot/aot_profile.py start --port "$PORT"
# Perform the Finder sequence.
python tools/aot/aot_profile.py stop --port "$PORT" --out "$RUN/finder-rom.txt" --ram "$RUN/finder-ram.txt"
# Open the drawing application and prepare its document.
python tools/aot/aot_profile.py start --port "$PORT"
# Perform the drawing sequence, leaving the application resident.
python tools/aot/aot_profile.py stop --port "$PORT" --out "$RUN/drawing-rom.txt" --ram "$RUN/drawing-ram.txt"
python tools/aot/dump_rom.py --port "$PORT" --out "$RUN/patched.rom"

AOT_OUT="$RUN/candidate.cpp" AOT_BUILD_DIR="$RUN/translator" \
  bash tools/aot/run_translate.sh "$RUN/patched.rom" \
    --hot "$RUN/finder-rom.txt" --hot "$RUN/drawing-rom.txt" \
    --ram-profile "$RUN/finder-ram.txt" --ram-profile "$RUN/drawing-ram.txt" \
    --min-count 1000 --ram-min-count 3000 --min-region-insns 2
```

Inspect `$RUN/translator/report.json`. The `rom_hot_untranslated` and `ram_hot_untranslated` arrays rank up to twenty omitted guest instruction addresses each. The counts show how often an instruction ran. The reason distinguishes unsupported instructions, count thresholds, changing RAM and region selection. The two `*_installed_share` values estimate profile coverage in the generated source. The device still verifies the code bytes before it installs entries. [AOT report and test guide](tools/aot/README.md)

```sh
bash test/aot_host/build.sh "$RUN/aot-host" "$RUN/candidate.cpp"
"$RUN/aot-host/aot_diff" "$RUN/patched.rom" --trials 12
GEN=src/basilisk/aot/aot_rom_generated.cpp
if test -f "$GEN"; then cp "$GEN" "$RUN/original-generated.cpp"; fi
cp "$RUN/candidate.cpp" "$GEN"
pio run -e "$SELFTEST_ENV"
python tools/aot/check_blob.py ".pio/build/$SELFTEST_ENV/firmware.elf" \
  --object ".pio/build/$SELFTEST_ENV/src/basilisk/aot/aot_rom_generated.cpp.o"
# Flash the self-test application with the matching ROM/disk selected on the card.
# Save the serial line containing [AOT TEST] RESULT and the number of runs.
pio run -e "$M5_ENV"
python tools/aot/check_blob.py ".pio/build/$M5_ENV/firmware.elf" \
  --object ".pio/build/$M5_ENV/src/basilisk/aot/aot_rom_generated.cpp.o"
```

Use a passing host comparison, a device `RESULT PASS` with nonzero runs, and a passing blob check before timing translated code. The profiling build counts interpreter dispatches and slows the guest. Time the normal build, once with the saved control translation (or interpreter-only) and once with the new translation from the same candidate revision. Save both binaries and ELFs, the startup AOT log, and the generated-source hashes. Use the saved application in `$RUN/candidate` as the control if it has your intended baseline translation.

Time all three workloads three times per build, alternating the build order where practical. Use a recording or timestamped inputs with a visible end condition. Take screenshots after timing. Report medians, individual times, and whether results and images agree. Investigate a slower holdout even if the training cases improve. Speedometer can provide an additional reference, with the same Control Strip state on both builds. [Benchmark commands](EMULATOR_PERFORMANCE.md#measuring)

## What to hand back

Save a short results file alongside the logs in `$RUN`. Someone reading it should be able to identify the first failing step and flash the matching image again.

| Record | Include |
| --- | --- |
| Device and build | Git revisions, build environments, firmware hashes and matching ELFs, silicon, SDK, ROM and OS IDs, disk-copy hash, AOT source hash or interpreter-only |
| Disk and display | Finder state across clean restarts, guest alerts, each depth/orientation, guest and panel screenshots, settled-panel mismatch counts, After Dark module and versions |
| Measurements | All USB hashes and rates, raw display intervals, workload action list, timings for each build, holdout result, translator report and both differential test results |
| First failure | Exact action, expected and observed result, serial excerpt, screenshot pair, reproducibility on a fresh disk copy, and result with the saved control firmware |

Restore the known-good application and the original card/settings when finished. A firmware rollback restores the program. Restoring a disk snapshot restores guest files and Finder state.

### Where to resume debugging

| Observation | First place to inspect |
| --- | --- |
| Finder loses windows on the candidate | Compare the copied disk's primary and alternate header before boot and after a clean shutdown. Confirm the disk path in the boot log. The backend open and write boundaries are in [sys_esp32.cpp](src/basilisk/sys_esp32.cpp). |
| Guest and panel agree on an unexpected color | Inspect the mode switch, `cscSetEntries`, `cscGetEntries` and gamma handling in [video.cpp](src/basilisk/video.cpp). Record the affected palette index and RGB value before and after the module activates. |
| The guest capture is correct but the settled panel has stale lines | Start at the reported first mismatch. Trace the tile's row bounds, overlay coverage and publication result in [video_esp32.cpp](src/basilisk/video_esp32.cpp), then the rotated write in [panel_surface.h](src/board/panel_surface.h). |
| USB Disk enumerates but data differs after eject | Preserve the failing card copy and readback hashes. Log the first failing callback's drive, LBA, offset, length and FatFs result in [usb_disk_media.h](src/basilisk/usb_disk_media.h). A failure before enumeration belongs in [usb_msc.cpp](src/basilisk/usb_msc.cpp). |
| A new translation changes an application's output | Keep the ROM hash, workload, generated source and first mismatch from the differential test. Repeat with the saved control image and trace the reported guest PC in [the translator](tools/aot/aot_translate.py). |
