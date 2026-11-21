# Wi-Fi application updates

The standard installation has one application slot. The optional A/B layout has two 6 MiB slots. It can receive a release over Wi-Fi while the current firmware remains available. A USB data cable authorizes each update.

Download the Host Tools ZIP and the matching `-app.bin` for your board, ESP32-P4 revision and firmware profile. Extract Host Tools and put the application file beside `WIFI_UPDATE.md`. Check the downloads against the release’s `SHA256SUMS`. [v5.4 beta 3 downloads](https://github.com/amcchord/M5Tab-Macintosh/releases/tag/v5.4-beta.3) · [Choosing a download](RELEASE_NOTES.md#choosing-a-download)

Run the examples from the extracted folder on macOS or Linux. Set `PORT` to the programming USB port and `APP` to the filename you downloaded. The example selects a standard pre-v3 Tab5.

```sh
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r tools/requirements.txt
PORT=/dev/cu.usbmodemXXXX
APP=./M5Tab-Macintosh-v5.4-beta.3-app.bin
```

Shut down in Finder. Start the board and tap the Happy Mac splash to enter Startup Settings. On firmware with the updater, check the installed layout. A first installation or an earlier release without the status command can proceed to USB provisioning.

```sh
python3 tools/wifi_update.py --port "$PORT" --status
```

If `layout` is `ab`, [install the application](#install-an-application). If it is `single`, provision the second slot once over USB.

## Provision over USB

This procedure is for standalone 16 MiB installations. It replaces the partition table, moves and shrinks SPIFFS, and installs the selected application in slot 0. Export any SPIFFS files you use and keep your original full-flash backup. Macintosh disks reside on the removable microSD card.

```sh
python3 -m pip install 'esptool>=5,<6'
BACKUP="original-flash-$(date +%Y%m%d-%H%M%S).bin"
python3 -m esptool --chip esp32p4 --port "$PORT" --after no-reset read-flash 0 0x1000000 "$BACKUP"
python3 -m esptool --chip esp32p4 --port "$PORT" verify-flash 0 "$BACKUP"
```

Continue after the backup verifies. Keep the bootloader from the matching merged release installed. If this is the first install, or the installed bootloader came from another profile, flash the matching merged release at offset zero **after** saving the original backup. [USB flashing instructions](README.md#flashing-the-firmware)

Host Tools includes the A/B partition table and its initial slot selection. This command keeps the installed bootloader and NVS at their existing addresses.

```sh
python3 -m esptool --chip esp32p4 --port "$PORT" write-flash \
  0x8000 wifi-update/partitions-ota.bin 0xe000 wifi-update/boot_app0.bin \
  0x10000 "$APP"
```

After restart, open Startup Settings and run the status command again. It should report `layout` as `ab` and `running` as `ota_0`. A standard merged USB flash or PlatformIO upload returns the board to the single-slot layout.

## Install an application

In Startup Settings, open **Wi-Fi**, join the same LAN as the computer, then return to the main Settings screen. Keep the programming USB cable connected. Replace the example IP address with the computer’s LAN IPv4 address.

```sh
python3 tools/wifi_update.py --port "$PORT" --host 192.168.1.20 --reboot "$APP"
```

The host serves just the selected file from that interface on a temporary TCP port. The host firewall must allow the board to connect to that port. USB authorizes the exact URL, byte count and SHA-256 of the file. ESP-IDF validates the application format, then the P4 reads back the inactive slot and checks the pinned digest before selecting it. The board descriptor and compiled SDK silicon minimum identify compatible images. Earlier firmware and merged images lack the required descriptor. Omitting `--reboot` stages the verified update for the next restart.

The serial commands are `BOOT UPDATE STATUS`, `BOOT UPDATE START <bytes> <sha256> http://<host-ip>:<port>/firmware.bin`, `BOOT UPDATE CANCEL`, and `BOOT UPDATE REBOOT`. Status reports the transfer stage, verified bytes, current slot and selected slot. Use `python3 tools/wifi_update.py --port "$PORT" --cancel` in Startup Settings to abort an active transfer or return the boot selection to the running slot before restart. Closing the host interrupts delivery. If the device already received the full file, it can finish verification and stage it. Check status and cancel explicitly to revoke it. While a verified update is staged, Tab5 Startup Settings disables USB Disk because exiting USB Disk restarts the board. Starting the Macintosh keeps the current firmware running until the next restart. The C6 carries the data through Wi-Fi. The P4 owns the app slots, verification and boot selection.

## Recovery

A failed transfer, bad digest, incompatible image or power loss before boot selection leaves the active app selected. With a rollback-enabled bootloader and a valid previous slot, the new app stays pending until board, display, a mounted SD card and Startup Settings initialization succeed. A blank card can then enter startup media setup. An initialization failure requests rollback. A reset or power interruption before confirmation makes the bootloader return to the previous app. A hang requires a reset. This startup check covers the preboot hardware and media; it does not exercise the entire Mac OS session. Bootloaders built without rollback keep the selected slot after reset.

A successful update leaves the previous application in the other slot until the next transfer starts. To explicitly return to a verified previous slot after a successful update, use the ESP-IDF OTA tool over USB, for example `otatool.py --port "$PORT" switch_ota_partition --slot 0` when slot 0 is the known-good one. To return the whole installation to its backed-up state, write and verify the original flash backup at offset zero over USB. The Wi-Fi updater can write only the inactive application and the OTA selection record.

[Espressif application descriptors](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/system/app_image_format.html) · [Espressif OTA and rollback](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/system/ota.html)
