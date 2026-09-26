# v4.7.1 — Serial testing and preboot control

This patch makes the attached serial connection usable for automated
screenshot/action/screenshot tests and configuration changes before boot.

## Improvements

- Larger screenshot blocks, CRC-checked selective retries, and buffered host
  reads. On the same Tab5 desktop, median color capture improved from 22.2 s
  to 0.55 s; monochrome improved from 2.2 s to 0.20 s.
- Boot-session request IDs and cached replies prevent duplicate input when an
  acknowledgement is lost. Native USB/JTAG opens set DTR/RTS atomically on POSIX
  hosts, preserving the running guest when reconnecting.
- All eleven saved preboot settings can be read or changed through USB serial,
  Python, CLI, and MCP. List/rescan SD media, enter settings remotely, save or
  reload selections, and boot without tapping the screen. WiFi passwords are
  write-only; `wifi_password_set` reports whether one is configured.
- Configuration requests execute on the task that owns settings and SD access.
  Invalid values and missing media are rejected. Saves verify a temporary file
  and retain a recoverable previous copy while replacing the settings file.
- Fix boot touch-task cleanup, release abandoned screenshot leases, and avoid
  holding the panel paused for the entire screenshot transfer.
- Add a serial benchmark and an opt-in Tab5 SDK profile matching the device
  used for physical testing. Protocol 3 screenshot clients remain supported.

## Downloads

| Hardware / use | Filename | Flash offset |
| --- | --- | --- |
| Tab5, pre-v3, standard SDK | `M5Tab-Macintosh-v4.7.1.bin` | `0x0` |
| Tab5, v3.1+ | `M5Tab-Macintosh-Rev3-v4.7.1.bin` | `0x0` |
| Waveshare 10.1, pre-v3 | `M5Tab-Macintosh-Waveshare-P4-10.1-v4.7.1.bin` | `0x0` |
| Waveshare 10.1, v3.1+ | `M5Tab-Macintosh-Waveshare-P4-10.1-Rev3-v4.7.1.bin` | `0x0` |
| Tab5, pre-v3, tested USB debug SDK | `M5Tab-Macintosh-USB-Debug-v4.7.1.bin` | `0x0` |
| Same debug application, existing compatible Tab5 installation | `M5Tab-Macintosh-USB-Debug-v4.7.1-app.bin` | `0x10000` |
| Python CLI, MCP server, and benchmark | `M5Tab-Macintosh-Host-Tools-v4.7.1.zip` | — |

Chip revision is separate from display/PCB revision. Rev3 images require v3.1
minimum; v3.0 is not covered. Merged images include bootloader, partitions, and
application; flashing them replaces a launcher and may clear NVS. The debug
application-only image preserves those regions on a compatible installation.
Neither package contains a Macintosh ROM, disk images, or device credentials.

The standard board/silicon profiles retain their v4.7 SDK pins. The optional
pre-v3 Tab5 debug profile uses Arduino 3.3.8 / IDF 5.5.4, matching the working
firmware on the attached development device. The older standard Tab5 SDK
failed to mount that unit's SD card. Keeping this separate avoids changing the
SDK choice for boards with different display/WiFi behavior.

Install `pyserial` using `tools/requirements.txt`, then run from the repository
(or unpacked host-tools directory):

```sh
python3 tools/mac_control.py boot-enter
python3 tools/mac_control.py boot-get
python3 tools/mac_control.py boot-set ramsize 16
python3 tools/mac_control.py boot-set audio false
python3 tools/mac_control.py boot-start
python3 tools/mac_control.py screenshot --color mac.png
```

`boot-enter` restarts the device once into settings. Firmware flushes SD handles,
clock, and PRAM, but does not request a Mac OS shutdown; shut down the guest first
when a clean guest filesystem is required. `boot-start` returns after emulator
initialization; wait for Finder using screenshots. [Full automation reference](AUTOMATION.md).

## Validation

- 40 automated tests, including C++ AddressSanitizer/UndefinedBehaviorSanitizer,
  serial retries/session handling, preboot validation, clock persistence, and
  release-image checks.
- Four standard firmware builds, with exact merged-component and linked-SDK
  revision checks. Their binaries are build-validated, not newly tested on
  all four hardware combinations.
- Physical testing on an ESP32-P4 revision 1.3 Tab5 using the separately labeled
  debug application: 400 control commands, 40 performance captures, 20 reconnects,
  all saved settings, invalid-input rejection, persistence across reboots, and
  restoration of the original device configuration.
- The final preboot build passed another 25 complete settings reads (median
  100 ms), remote entry and boot, and three color captures in 0.50–1.11 s.
- `SHA256SUMS` and `BUILD-MANIFEST.json` identify the exact downloadable assets
  and their firmware components. The debug application is the byte-for-byte
  hardware-tested build.

## Limits and rollback

Screenshots capture the guest framebuffer, not the physical preboot screen.
Serial service starts after SD/configuration initialization; earlier fatal
board/SD/ROM failures remain outside the API. USB Disk mode must be exited on
screen before settings commands can access the card. Replay protection assumes
one serialized client and caches its last tagged request.

Live WiFi association, power-loss injection during settings replacement,
Waveshare/UART hardware, and production P4 hardware were not exercised in this
pass. Existing v4.7 downloads remain available for rollback on their matching
hardware. Preserve the SD card and a known-good firmware backup; a firmware
rollback does not automatically revert settings saved on the SD card.
