# Tab5 SD startup follow-up

During 5.0 release preparation, the pre-v3 Tab5 debug build intermittently
reported SD command CRC/status failures, followed by a guest illegal-instruction
or damaged-System-file dialog. A software restart and one user power cycle
still encountered a failure; a later user reboot got past it. Permanent disk
image damage was not established.

The diagnostic changed only the Tab5 SPI SD frequency from 25 MHz to 10 MHz.
It retained the same SD card, Mac OS 8 disk, 16 MB RAM, audio setting, rotation,
SDK, and disabled native QuickDraw. Only application flash was written; full
readback matched the built application. The final release keeps that clock
setting on all Tab5 profiles. Waveshare's SDMMC configuration is unchanged.

## Observations

- First start: Finder loaded, with solid window/scrollbar borders.
- Second controlled start: the normal Mac OS improper-shutdown notice appeared.
  No CRC/status or illegal-instruction errors were present in the captured log.
- Third controlled start: Finder loaded again, with no such logged errors.
- Original saved settings and disk selection were preserved. No disk image was
  replaced or repaired by host tools.
- The final debug application matches the hardware-tested diagnostic exactly:
  SHA256 `cfbc1d1592df1d49463434525a132d99592d71c19e56f410e9ab9ad5d93ee213`.

Evidence is retained locally under `artifacts/release-v5.0/sd-diagnostic/`.
The first attach lost its USB file descriptor after reset, so its early boot
log is incomplete; reattaching without another reset recovered serial control.
The subsequent two start logs include the mount and guest startup sequence.

## Limits

This is a conservative mitigation supported by a small hardware sample. It
does not establish the exact electrical or driver cause, or guarantee all
cards and boards. The lower clock may reduce disk throughput; no new disk
benchmark claim is made. Longer testing and other cards remain follow-up work.

PR #16 batches raw sectors in preboot USB Disk mode. It does not change the
normal emulator File/FatFs access path, SPI clock, or card initialization and
was not included as a fix for this failure.

## Current branch

Opening a writable Macintosh image now preserves the primary HFS volume
attributes and Finder information. The guest can apply its own consistency
checks when it mounts the volume. A host regression compares every byte
of an image across open and close, including one with different primary
and alternate volume attributes. [Storage tests](test/test_storage.py)

The preboot USB Disk path now sends the full aligned sector count to the
mounted FatFs drive. A partial-sector write reads and preserves the surrounding
bytes. The handoff includes separate procedures for checking guest disk
behavior and benchmarking USB copies after eject and remount.
[Tab5 hardware handoff](TAB5_HARDWARE_HANDOFF.md)

## Benchmarking the card

Build `pio run -e esp32p4_tab5_sd_benchmark` for the pre-v3 Tab5. This standalone firmware uses the USB Debug SDK and the emulator's board and card drivers. The card stays idle until the first mount command over USB-C serial at 115200 baud.

1. Send `MOUNT SPI` for the release driver, `MOUNT DMA` for Espressif's SDSPI DMA driver, or `MOUNT NATIVE` for four-bit SDMMC. Both SPI drivers use 10 MHz. Native mode requests up to 40 MHz. The log reports the selected driver, clock and card geometry. A diagnostic mount failure ends that test.
2. Optionally send `WIFI` to start the C6 link and scan for networks. Leave Wi-Fi running through the timed transfers. Send `WIFI` afterwards to check that it can still scan.
3. Send `WRITE 82f731a0`, using a fresh eight-digit hexadecimal ID for each run. The test exclusively creates `/SDB82f731a0.tmp` on the card, writes 4 MiB in 16 KiB chunks, calls `fsync`, closes the file, then reopens and checks every byte and the length. It reports separate write and read times including pattern generation and verification.
4. Send `RESET`, select a driver again, then send `CHECK 82f731a0` to verify the saved file. For cold verification, power cycle the card and board before selecting the driver.
5. Send `CLEAN 82f731a0` to verify the entire file again and remove it. A partial write, wrong ID or mismatched data leaves the file in place for inspection.

Use the same card and Wi-Fi state to compare modes. Wait for the command to finish before resetting. A card initialized in SPI mode generally stays in that mode until its supply is removed. To test native mode after SPI, restart into the diagnostic, reseat the card while it is idle, then issue `MOUNT NATIVE` first. The card needs a FAT32 filesystem.

For an emulator integration build, add `-DTAB5_SD_DMA=1` or `-DTAB5_SD_NATIVE=1` to the Tab5 environment's existing build flags. Either build tries its selected driver at startup and falls back to Arduino SPI if mounting fails. USB Disk uses the geometry and drive number of the mounted card. Native mode shares the host controller with the C6 and needs device testing with Wi-Fi after both a cold start and a software restart. The pin map and slot configuration follow [M5Stack's board driver](https://github.com/m5stack/M5Tab5-UserDemo/blob/main/platforms/tab5/components/m5stack_tab5/m5stack_tab5.c).
