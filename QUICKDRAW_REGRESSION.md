# Finder border regression — 2026-09-26

The native QuickDraw acceleration introduced in commit `44b9bc0` (the 4.6
work) causes the observed dotted Finder window borders. A device A/B test
isolated it: identical display code and saved settings, with only
`CPU_NATIVE_QD_ACCEL` changed from 1 to 0. The original Mac QuickDraw path
restored solid borders.

Native QuickDraw is now **disabled by default in all five profiles**. The
implementation and its host guard tests remain available for investigation.
The display reliability fixes are retained. This is a correctness-first
fallback, not a claim that the native implementation has been repaired.

## Evidence

Device: M5Stack Tab5, ESP32-P4 revision 1.3, USB serial
`30:ED:A0:E2:FC:2B`, using the pre-v3 debug SDK profile (Arduino 3.3.8 / IDF
5.5.4). Both runs used 16 MB RAM, rotation 180, audio enabled, the same disk
image, and the same 640×360 Finder window.

Artifacts are under the control checkout's
`artifacts/display-reliability-20260926/`:

- `hardware-flash/scrollbar-regression.png`: acceleration enabled.
- `quickdraw-regression/quickdraw-disabled-desktop.png`: acceleration disabled.
- `quickdraw-regression/border-pixels.json`: pixel counts in matched borders.
- `quickdraw-regression/reference-health.json`: firmware options, settings,
  serial capture status and zero native QuickDraw calls in the comparison.
- Build/upload logs and exact comparison configuration are alongside them.

In the 320-pixel lower border sample, acceleration produced 160 black and
160 white pixels. Without acceleration all 320 were black. In the 160-pixel
right border sample, the corresponding counts were 80/80 versus 160 black.
The nearby solid gray row remained identical between captures. These
differences exist in the guest framebuffer, before panel conversion/scanout.

The enabled run had 80 accelerated shape calls and 10 accelerated copies,
with zero accelerated lines, MoveTo or ScrollRect calls. Shape drawing is a
strong next suspect, but this test does not identify the exact primitive or
semantic error. Re-enabling selected paths requires differential evidence
against guest QuickDraw, including these Finder borders and patched/system
drawing behavior. Apple documents the relationship between pen patterns,
resolved colors and shape drawing in its
[CGrafPort reference](https://developer.apple.com/library/archive/documentation/mac/QuickDraw/QuickDraw-203.html)
and [PixPat reference](https://developer.apple.com/library/archive/documentation/mac/QuickDraw/QuickDraw-208.html).

## Flash and rollback record

Austin explicitly requested flashing the connected device. Only application
flash at `0x10000` was written. The original complete 6 MiB application
partition was saved to `hardware-flash/original-app-partition.bin`, and the
existing partition table was read and checked before the first write.
Bootloader, partition table and NVS were not reflashed.

The first flashed reliability candidate was source `82e5266`, application
SHA256 `951e58e01d66d65dea449db1993101779d7ca997a8abea3c985a35f4b470a287`.
A full application readback matched that SHA. It booted, mounted SD and
preserved the saved settings, but retained the dotted borders.

The currently installed comparison application is the same source with
`CPU_NATIVE_QD_ACCEL=0`, SHA256
`d79cac3ca0745b242d60d363701fc7e3ab1cb54af56191f6a762a51d550aee6d`.
Esptool verified the write. Startup, settings and a color serial screenshot
were checked, and native QuickDraw counters remained zero. Its effective
firmware flags match the new repository default. The firmware still reports
4.7.1; identify this build by its application hash.

To roll back the application from the `hardware-flash/` directory:

```sh
python -m esptool --chip esp32p4 --port /dev/cu.usbmodem211201 \
  --baud 1500000 write-flash 0x10000 original-app-partition.bin
```

The port may change after reconnect. The backup hash and full first-flash
record are in `hardware-flash/flash-record.json`. The comparison upload and
health record supersede that file's installed-image identity.

## Validation and limits

The comparison Tab5 firmware built successfully. All 46 host tests passed
after changing the default. All five firmware profiles built successfully
(201.332 seconds total). The native guard tests exercise dormant code; they
do not establish equivalence with guest QuickDraw.

Disabling native QuickDraw may reduce graphics throughput. No new benchmark
claim is made. The screenshots establish the guest-pixel fix, while physical
panel behavior, optical tearing and other hardware cases still need Austin's
testing. No source was pushed and no release was published. The separate
uncommitted performance work in the control checkout remains preserved.
