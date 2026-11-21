# Wi-Fi application updates

The optional A/B layout keeps two 6 MiB copies of the ESP32-P4 application. The normal release layout has one application slot. Provision A/B once over USB on a standalone 16 MiB installation. This procedure changes the layout and shrinks and relocates the SPIFFS partition. Back up the whole flash and export any SPIFFS data before proceeding. Keep the matching bootloader installed. The microSD card holds the Macintosh disk images.

## Provision over USB

Choose the usual PlatformIO environment for the board and silicon. Build from source containing this updater. This example uses a pre-v3 Tab5 USB Debug installation. Use a new backup filename and the board's actual serial port.

```sh
PORT=/dev/cu.usbmodemXXXX
ENV_NAME=esp32p4_pioarduino_debug
SDK="$HOME/.platformio/packages/framework-arduinoespressif32"
pio run -e "$ENV_NAME"
esptool --chip esp32p4 --port "$PORT" read-flash 0 0x1000000 original-flash.bin
shasum -a 256 original-flash.bin
python3 "$SDK/tools/gen_esp32part.py" partitions_ota.csv partitions-ota.bin
esptool --chip esp32p4 --port "$PORT" write-flash \
  0x8000 partitions-ota.bin 0xe000 "$SDK/tools/partitions/boot_app0.bin" \
  0x10000 ".pio/build/$ENV_NAME/firmware.bin"
```

The bundled 8 KiB OTA data image selects slot 0. Its initial selection also gives rollback-enabled bootloaders a previous slot during the first update. NVS stays at `0x9000`. Use this partition table for subsequent USB re-provisioning. The normal merged release and PlatformIO upload target reinstall the single-slot table.

## Install an application

Shut down in Finder. Open Startup Settings, join the LAN under Wi-Fi, and return to the main Settings screen. Connect the programming USB cable. On the computer, provide its LAN IPv4 address and the application-only `firmware.bin` for the right board and silicon.

```sh
python3 tools/wifi_update.py --port "$PORT" --status
python3 tools/wifi_update.py --port "$PORT" --host 192.168.1.20 --reboot \
  ".pio/build/$ENV_NAME/firmware.bin"
```

The host serves just the selected file from that interface on a temporary TCP port. The host firewall must allow the board to connect to that port. USB authorizes the exact URL, byte count and SHA-256 of the file. ESP-IDF validates the application format, then the P4 reads back the inactive slot and checks the pinned digest before selecting it. The board descriptor and compiled SDK silicon minimum identify compatible images. Earlier firmware and merged images lack the required descriptor. Omitting `--reboot` stages the verified update for the next restart.

The serial commands are `BOOT UPDATE STATUS`, `BOOT UPDATE START <bytes> <sha256> http://<host-ip>:<port>/firmware.bin`, `BOOT UPDATE CANCEL`, and `BOOT UPDATE REBOOT`. Status reports the transfer stage, verified bytes, current slot and selected slot. Use `python3 tools/wifi_update.py --port "$PORT" --cancel` in Startup Settings to abort an active transfer or return the boot selection to the running slot before restart. Closing the host interrupts delivery. If the device already received the full file, it can finish verification and stage it. Check status and cancel explicitly to revoke it. While a verified update is staged, Tab5 Startup Settings disables USB Disk because exiting USB Disk restarts the board. Starting the Macintosh keeps the current firmware running until the next restart. The C6 carries the data through Wi-Fi. The P4 owns the app slots, verification and boot selection.

## Recovery

A failed transfer, bad digest, incompatible image or power loss before boot selection leaves the active app selected. With a rollback-enabled bootloader and a valid previous slot, the new app stays pending until board, display, a mounted SD card and Startup Settings initialization succeed. A blank card can then enter startup media setup. An initialization failure requests rollback. A reset or power interruption before confirmation makes the bootloader return to the previous app. A hang requires a reset. This startup check covers the preboot hardware and media; it does not exercise the entire Mac OS session. Bootloaders built without rollback keep the selected slot after reset.

A successful update leaves the previous application in the other slot until the next transfer starts. To explicitly return to a verified previous slot after a successful update, use the ESP-IDF OTA tool over USB, for example `otatool.py --port "$PORT" switch_ota_partition --slot 0` when slot 0 is the known-good one. To return the whole installation to its backed-up state, write and verify the original flash backup at offset zero over USB. The Wi-Fi updater can write only the inactive application and the OTA selection record.

[Espressif application descriptors](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/system/app_image_format.html) · [Espressif OTA and rollback](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/system/ota.html)
