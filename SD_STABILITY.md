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

1. Send `MOUNT SPI` for the Arduino SPI driver, `MOUNT DMA` for Espressif's SDSPI DMA driver, or `MOUNT NATIVE` for four-bit SDMMC. Both SPI drivers use 10 MHz. Native mode requests up to 40 MHz. The log reports the selected driver, clock and card geometry. A diagnostic mount failure ends that test.
2. Send `INFO` to report the card product, live native bus clock and width, FAT type, cluster size, free clusters, and root file and directory counts. Multiply the cluster byte size by the total or free cluster count to find the mounted volume's capacity or free space. A volume can occupy only part of a physical card.
3. Send `RAW` to read 4 MiB from the start of the FAT data area in 16 KiB requests. `RAW1` reads the same region one sector at a time. Both commands are read-only. Send `WIFI` to start the C6 link and scan for networks, then repeat `INFO` and `RAW` to compare.
4. Send `WRITE 82f731a0`, using a fresh eight-digit hexadecimal ID. The test exclusively creates `/SDB82f731a0.tmp`, writes 4 MiB in 16 KiB chunks, syncs and closes the file, then reopens and verifies every byte and its length. `PREALLOC 82f731a1` performs the same test after reserving 4 MiB of contiguous space. Use a separate fresh ID. Timing includes allocation, pattern generation and sync. The log also reports time spent in I/O calls and whether each file is contiguous.
5. Send `RESET`, select a driver again, then send `CHECK 82f731a0` to verify the saved file. For cold verification, power cycle the card and board before selecting the driver.
6. Send `CLEAN` with each completed test ID to verify the entire file and remove it. A mismatched file is preserved. If writing fails during a run, the diagnostic attempts to remove its own newly created file.

Use the same card and Wi-Fi state to compare modes. Wait for the command to finish before resetting. A card initialized in SPI mode generally stays in that mode until its supply is removed. To test native mode after SPI, restart into the diagnostic, reseat the card while it is idle, then issue `MOUNT NATIVE` first. The card needs a FAT32 filesystem.

Tab5 release builds try four-bit SDMMC at 40 MHz first. If that mount fails, a 20 MHz native probe checks whether the card responds independently of the filesystem. A responding card gets a 20 MHz four-bit mount attempt. If its filesystem still cannot mount, startup opens card recovery in native mode. A card that fails native initialization can use the 10 MHz Arduino SPI driver. SPI DMA remains available for diagnostic comparisons. The next driver runs after the previous attempt releases the bus and mount point. USB Disk uses the geometry and drive number of the mounted card. The card uses SDMMC slot 0 and the Wi-Fi coprocessor uses slot 1. [M5Stack's board driver](https://github.com/m5stack/M5Tab5-UserDemo/blob/main/platforms/tab5/components/m5stack_tab5/m5stack_tab5.c).


### Measured effect of FAT cluster size

On November 21, 2026, an attached pre-v3 Tab5 mounted a 29,664 MiB microSD card in four-bit mode at 40 MHz. A 252 MiB FAT32 volume on the card used 512-byte clusters. The aligned 16 KiB raw reads measured 10,597 KiB/s with Wi-Fi off and 10,595 KiB/s after starting the C6 and scanning. Both live bus measurements showed 40 MHz and four data lines.

With Wi-Fi off, a contiguous 4 MiB file written through the filesystem measured 183 KiB/s including sync. Explicitly preallocating the same size took 462 ms and produced 189 KiB/s end to end. File read-back verified at 795 KiB/s for the ordinary file. The pinned FatFs implementation splits direct reads and writes at cluster boundaries, so this volume's 16 KiB application requests require at least 32 separate card data transfers. Both benchmark files were contiguous. [FatFs read and write paths in ESP-IDF 5.5.4](https://github.com/espressif/esp-idf/blob/v5.5.4/components/fatfs/src/ff.c#L3996-L4003)

After the Tab5 formatted this same card, it contained one full-card FAT32 volume with 32 KiB clusters. With Wi-Fi off, the same file test reached 2,074 KiB/s for synced writes and 11,963 KiB/s for verified reads, gains of 11.3× and 15.0×. Each file was synced, closed and reopened. Every byte also passed a second check after the board restarted.

With the C6 connected for a Wi-Fi scan, 16 KiB file requests on that formatted card gave the following rates. Writes include synchronization. These are individual 4 MiB file runs on a single card.

| Tab5 card interface | Write (KiB/s) | Verified read (KiB/s) |
| --- | ---: | ---: |
| Four-bit SDMMC, 40 MHz | 1,369 | 10,198 |
| Arduino SPI, 10 MHz | 920 | 1,759 |
| SPI DMA, 10 MHz | 589 | 940 |

Once an SD card enters SPI mode, returning to the native SD protocol requires removing card power. On the Tab5, socket J2 pin 4 connects directly to the shared SOC_3.3V rail. The ESP32-P4’s LDO4 supplies the SD I/O pads. Releasing it cannot power-cycle the card. Reseat the card with all storage users stopped or fully power down the board, then start with native SDMMC. [M5Stack Tab5 schematic, sheet 2](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/Tab5_Schematics_PDF.pdf#page=2) · [Espressif SD mode selection](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/api-reference/peripherals/sdspi_share.html)

## Tab5 card recovery

Card inspection reports the hardware identity, capacity, existing volume size, partition type and cluster size. On a legacy Arduino SPI mount it briefly closes and reopens the card to read the hardware identity. Run recovery from preboot with all files closed, USB Disk disconnected and downloads idle.

Formatting erases **all partitions and files** on the confirmed card. Each inspection issues a new confirmation. The formatter reopens the card and checks its identity, capacity and first sector before writing. It creates one full-size MBR FAT32 partition with two FATs and aligns the data area to 1 MiB. Cards with 512-byte sectors from 64 MiB through the SDK's 32-bit sector limit are supported. A 2 GiB card gets 16 KiB clusters. A 29 GiB or larger card gets 32 KiB clusters. The formatter rereads the partition and boot sector and remounts the filesystem.

The stand-alone pre-v3 diagnostic builds with `pio run -e esp32p4_tab5_card_format`. Send `INSPECT` over USB-C serial to read the card information. After approving the erasure, send `ERASE FAT32 <token>` using the token from that inspection. A different card, a changed first sector or a restart requires a fresh inspection.
