# Tab5 ESP32-C6 diagnostics proof

This optional firmware runs an HTTP server on the Tab5’s ESP32-C6 while retaining Espressif’s Hosted SDIO radio service. A browser on the same network can open `http://<shared-IP>:62080/` to read C6 uptime and free memory, see the P4 SDIO link and power-saving state, or time a 64 KiB download. The page checks every downloaded byte. The SDIO indicators describe the transport. Guest applications and the Macintosh disk remain on the P4.

The browser talks directly to the C6. Page refreshes, the JSON handler and transfer bytes consume C6 resources. The routes are read-only, available to clients on the LAN, and carry no guest data or credentials. The transfer measurement covers the browser, Wi-Fi and C6 HTTP server. It does not measure the P4 emulator or the SD card.

## Build

Use ESP-IDF v5.5.4 and ESP-Hosted v2.12.13 (`dd0176e5fc959d79b306f6677a2f878123c58e9a`). From the repository root with the IDF environment active:

```sh
mkdir -p artifacts/c6
git clone --depth 1 --branch v2.12.13 https://github.com/espressif/esp-hosted-mcu.git artifacts/c6/hosted
export ESP_HOSTED_DIR="$PWD/artifacts/c6/hosted"
idf.py -C radio/tab5-c6 -B "$PWD/artifacts/c6/build" -DSDKCONFIG="$PWD/artifacts/c6/sdkconfig" build
```

The project compiles the pinned upstream radio sources and adds the HTTP handlers. It uses the upstream C6 bootloader and partition map for 4 MiB flash, including two 1,920 KiB application slots. The November 21 build produced a 1,402,624-byte C6 application with a valid image digest. Native handler tests with simulated hardware checked the SDIO status fields and all 65,536 response bytes. The page accepted that payload and rejected a corrupted copy. Neither radio flashing nor over-the-air traffic was exercised on a Tab5.

## Host pairing

Espressif’s network-split mode shares one station IP between two TCP/IP stacks. This project uses the C6 range 61440–65535. Host ephemeral ports occupy 49152–61439. The explicit host-forwarding list includes TCP 80, 443 and 8080. The host supplies Wi-Fi connection settings. The C6 image contains no preconfigured SSID. [Espressif network split guide](https://github.com/espressif/esp-hosted-mcu/blob/v2.12.13/docs/feature_network_split.md)

The bundled Arduino P4 libraries were built with `CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED` disabled. To use this proof on hardware, the P4 needs a compatible SDK built with network split enabled and the same port assignments and Hosted version. A compiler define on the emulator sketch alone cannot change the linked lwIP or radio libraries. Espressif supplies a matching [host application example](https://github.com/espressif/esp-hosted-mcu/tree/v2.12.13/examples/host_network_split__power_save). On a paired setup, verify P4 DHCP/DNS, guest web access, reconnect after sleep, both HTTP servers and their behavior under concurrent traffic. The current proof includes C6 diagnostics and a radio transfer test. Remote input, emulation counters and P4 application updates require a separate P4 service. Do not select or flash this C6 image through the P4 updater.

## Hardware and recovery

The P4 links to the C6 on SDIO slot 1 using clock GPIO12, command GPIO13, data GPIO11/10/9/8 and reset GPIO15. The Tab5 USB-C programming connection targets the P4. The C6 has a separate six-pin J1 `C6_ISP` header. In schematic pin order the connections are WLAN 3.3 V, C6 TXD, C6 RXD, C6 reset, C6 GPIO9 and ground. Use the vendor’s connector orientation and 3.3 V logic. [Tab5 schematics](https://docs.m5stack.com/en/core/Tab5?id=schematics) · [M5Stack C6 recovery procedure](https://docs.m5stack.com/en/guide/tab5/c6_wifi_restore)

Before a radio trial, read the chip ID, flash capacity and a full backup through the C6 header with the host emulator shut down. Keep the original radio and P4 images together. M5Stack’s factory C6 image is a merged image at offset zero and declares 4 MiB flash. A byte-for-byte backup restores the specific radio version that worked with the installation. [Vendor factory image](https://github.com/m5stack/M5Tab5-UserDemo/tree/fe8aa051beb9385468f7e46b909e764169df3f7a/platforms/tab5/wifi_c6_fw)
