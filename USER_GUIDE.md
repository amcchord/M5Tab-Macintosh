# Macintosh media, settings and recovery

[First installation and card setup](README.md#sd-card-setup) · [Flashing the firmware](README.md#flashing-the-firmware)

## Macintosh media

The outer microSD volume uses FAT32. Each Macintosh disk image on that volume has its own guest filesystem. Copy files onto the card with a reader or the Tab5’s USB Disk mode. Eject the card or drive on the computer after copying.

| File or folder at the card root | Use |
| --- | --- |
| `Q650.ROM` | Macintosh Quadra 650 ROM. Startup setup uses this name. |
| `Macintosh8.dsk` or `Macintosh7.dsk` | Starter hard disk for Mac OS 8.1 or System 7.5.3. Older installations and your own bootable images can have other names. |
| `.dsk`, `.img`, `.hfv` | Macintosh disk images listed under **Hard Disk**. |
| `.iso`, `.cdr`, `.toast` | CD images listed under **CD-ROM**. Select the image and **Boot from CD** to start from a bootable installer or Disk Tools disc. |
| A folder such as `Shared` | Select it under **Shared Folder** to exchange files between the card and Finder. Keep resource-fork sidecar folders with their files when copying Macintosh software. |
| `basilisk_settings.txt` | Startup choices, including disk paths, memory, sound and Wi-Fi. |
| `BasiliskII_XPRAM` | Guest parameter RAM, including settings maintained by classic Mac OS. |

To use your own system disk, place a compatible bootable image and the ROM at the root, tap the Happy Mac splash and select the disk. The on-device installer keeps an existing file whose contents differ from its published starter. Move that file elsewhere first if you want the installer’s copy.

## Startup settings

Tap the Happy Mac during the brief startup splash, then choose **Start Up** when ready. Under **Memory**, select 8, 16, 20 or 24 MB. Mac OS 8.1 with a browser benefits from 24 MB. The **Sound** option enables audio. **Rotate 180** changes the Tab5 screen and touch orientation.

Most settings can also be edited in `basilisk_settings.txt` while the board is off and the card is in a reader. For example, `disk=/Macintosh7.dsk`, `ramsize=16`, `boot_from_cd=no` and `audio=yes` each occupy a separate line. To keep Startup Settings open for recovery, create an empty `basilisk_recovery.txt` at the card root. Remove that file to resume automatic startup.

### Keyboard and mouse

The touch keyboard opens with a three-finger tap. Tap Shift, Control, Option or Command to latch the modifier, then tap it again to release. A four-finger tap switches to the game pad with arrow keys, Escape, Return, Space and Option. A USB mouse supplies relative pointer movement. The scroll wheel sends arrow keys.

The official 70-key Tab5 Keyboard can be attached to Ext.Port1 while the emulator is running. Its default I²C address is `0x6D`. **Aa** is Shift, **Ctrl** is Control, **Alt** is Option and **Sym** selects the printed symbol layer.

### Network settings inside Mac OS

1. Open **Wi-Fi…** in Startup Settings, choose the network, tap the password field, enter the password and tap **Join**. Return and start the Macintosh.
2. In the guest’s **TCP/IP** control panel, set **Connect via** to **Ethernet** and **Configure** to **Using DHCP Server**. If the panel is missing on an Open Transport system, enable TCP/IP and the Open Transport extensions in **Extensions Manager**, then restart the guest.
3. For manual configuration, use `10.0.2.15`, subnet `255.255.255.0`, router `10.0.2.2` and DNS `10.0.2.3`. Classic browsers can use plain HTTP. Modern HTTPS generally requires a newer TLS implementation.

The guest uses a private network behind the board. To view its screen from a modern browser, pair the local console over USB and use its printed LAN URL. [Browser pairing, serial commands and screenshots](AUTOMATION.md#wi-fi-console)

## Recovery

| Symptom | What to check |
| --- | --- |
| Card is absent or unmountable | Reseat it while idle, then choose **Retry Mount**. The Tab5’s **SD Card** screen shows card and volume sizes. If the card contains files you need, inspect and back them up in a reader before considering **Erase Card**. |
| Setup reports a full card | Allow at least 1.2 GiB free on the FAT32 volume. Card capacity and mounted volume size can differ on a partitioned card. |
| Download stopped or an SD write failed | Reconnect Wi-Fi or reseat the card after shutting down, as appropriate. Reopen setup and choose **Retry**. Setup resumes the compressed download from the saved file and verifies the completed image before publishing it. |
| Finder reports an improper shutdown | Use **Special → Shut Down** at the end of a session. For supported Mac OS 7.6, 7.6.1, 8.0 and 8.1 classic HFS startup volumes, the emulator requests the guest’s disk check. The supplied System 7.5.3 image has a separate verified metadata repair. Persistent disk errors warrant a backup and a bootable Disk Tools check. [System 7 repair details](HFS_STARTER7.md) |
| An application crashes at every startup | Hold Startup Settings open, select a known-good boot disk or Disk Tools CD, and try disabling **Sound**. Back up the card before changing disk images. To reset guest parameter RAM, rename `BasiliskII_XPRAM` on the card and restart. |
| Serial flashing fails | Check the port and data cable. Close the serial monitor or other clients. Try `--baud 460800` or `115200`. If needed, hold **BOOT**, tap **RESET**, then release **BOOT**. |
| Disk transfers are unexpectedly slow | Open **SD Card** in Startup Settings and use Host Tools’ `boot-card status` to read the driver and bus width. A Tab5 card already in SPI mode may need removal and reinsertion with the board off to return to four-bit mode. [SD drivers and measurements](SD_STABILITY.md) |

With the image closed in any emulator, Host Tools can inspect its classic HFS clean-shutdown flag. An all-clear reports the flag state only. It cannot establish that the entire disk is healthy.

```sh
python3 tools/check_startup_disk.py sdCard.zip
python3 tools/check_startup_disk.py /Volumes/TAB5/Macintosh8.dsk
```

## Build environments

From a source checkout with PlatformIO installed, run `pio run -e ENVIRONMENT` using the name below. The Waveshare target fetches its matching ESP-Hosted radio firmware on the first build. For a development upload, `pio run -e ENVIRONMENT -t upload` writes the project’s standard partition layout. Boards provisioned for two-slot updates should use the application update procedure. [Wi-Fi update guide](WIFI_UPDATE.md)

| Board and ESP32-P4 silicon | PlatformIO environment |
| --- | --- |
| Tab5, pre-v3 | `esp32p4_pioarduino` |
| Tab5, pre-v3, USB Debug | `esp32p4_pioarduino_debug` |
| Tab5, v3.1 or later | `esp32p4_pioarduino_rev3` |
| Waveshare 10.1″, pre-v3 | `waveshare_p4_101` |
| Waveshare 10.1″, v3.1 or later | `waveshare_p4_101_rev3` |

Both boards use a 68040 emulator and indexed Macintosh graphics. The Tab5 displays a 640 × 360 guest framebuffer on a 1280 × 720 panel. The Waveshare displays 640 × 400 at 1280 × 800 in landscape. The two CPU cores share emulation, display, input and I/O work. Release firmware adds ahead-of-time translations for selected Mac ROM and system code. To reproduce those translations, use your own source media and the AOT tools. [Translation and profiling](https://github.com/amcchord/M5Tab-Macintosh/blob/v5.4/tools/aot/README.md) · [Architecture and performance](EMULATOR_PERFORMANCE.md)

Use `pio device monitor` for startup and serial diagnostics. For repeatable measurements, the command-line tools can capture guest screenshots, send input, read firmware and memory counters, and run timed benchmarks. Include the exact firmware filename, board, ESP32-P4 revision, guest system and steps when reporting a failure. [Host automation](AUTOMATION.md) · [Tab5 hardware testing](TAB5_HARDWARE_HANDOFF.md)
