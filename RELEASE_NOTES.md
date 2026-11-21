# 5.4 beta 3

## Getting started on a Tab5

Insert a microSD card and boot. When the ROM or startup disk is missing, **Set Up SD Card** opens. Join Wi-Fi, choose Mac OS 8.1 or System 7.5.3, and tap **Download**. The installer needs 1.2 GiB free. It checks the download and reads back the expanded files before offering **Start Up**. Interrupted downloads can resume. Existing files with different contents are preserved. [Setup instructions](README.md#sd-card-setup)

**SD Card** in Startup Settings shows the mounted filesystem and card size. Formatting requires confirmation and erases the entire card, including other partitions. The Tab5 creates a FAT32 volume suitable for Macintosh disk images. [Storage and benchmarks](SD_STABILITY.md)

## Startup disks and remote access

- On supported Mac OS 7.6 through 8.1 boot disks, the guest File Manager checks an unclean HFS volume. For the published System 7.5.3 starter, firmware verifies its filesystem metadata before correcting a duplicate file ID, the allocation of that ID, two file counts, and the shutdown flag. [System 7 repair](HFS_STARTER7.md)
- Pair a browser over USB for color screenshots, memory and firmware status, and a timed guest-speed measurement. Optional control supports mouse clicks, double-clicks and typing. The stock ESP32-C6 carries Wi-Fi traffic. The ESP32-P4 serves the console. [Browser controls](AUTOMATION.md#wi-fi-console)
- The optional two-slot flash layout supports USB-authorized firmware updates over Wi-Fi. The P4 verifies the application against the selected board, silicon revision and SHA-256 and reads back the inactive slot before selecting it. [Setup, updates and rollback](WIFI_UPDATE.md)
- The indexed renderer skips unchanged row runs. A deterministic Finder selection trace used 55% fewer submitted panel bytes at both 1 and 8 bits per pixel, with identical final pixels in the host renderer. [Workload and measurements](EMULATOR_PERFORMANCE.md)

## Choosing a download

Choose the firmware for your board and ESP32-P4 revision. The release has separate images for pre-v3 and v3.1-or-newer Tab5 and Waveshare boards, plus a pre-v3 Tab5 USB Debug build.

| Installation | File | Use |
| --- | --- | --- |
| First USB flash or standard reinstall | Matching merged firmware `.bin` | Flash at offset zero. This installs the standard single-slot layout. |
| Existing two-slot installation | Matching `-app.bin` | Give this file to the Wi-Fi updater. The updater chooses the inactive slot. |
| Host utilities | Host Tools ZIP | USB pairing, screenshots, input, benchmarking and updates. |
| Guest utilities | Mac Extras ZIP | Macintosh Control Strip module and guest utilities. |

Check `SHA256SUMS` and `BUILD-MANIFEST.json` for download hashes, SDK versions, chip limits, and the exact source revision. [USB flashing instructions](README.md#flashing-the-firmware)

Stable v5.3 remains available. Restoring a saved whole-flash backup over USB also restores the previous partition layout. Macintosh disks and saved settings reside on the microSD card. [Stable v5.3](https://github.com/amcchord/M5Tab-Macintosh/releases/tag/v5.3)
