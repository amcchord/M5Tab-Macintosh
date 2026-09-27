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
