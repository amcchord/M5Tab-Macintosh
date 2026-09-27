# Closed-loop host control

The programming/console connection exposes an `@B2` automation protocol from
preboot settings through emulator operation. The protocol captures the logical Mac screen and
injects input at the ADB layer, so coordinates are always `640x360` on Tab5 or
`640x400` on Waveshare regardless of physical panel scaling or 180-degree
rotation. Normal touchscreen, attached keyboard, and USB mouse input continue
to work.

## Host setup

```sh
python3 -m pip install -r tools/requirements.txt
python3 tools/mac_control.py --port /dev/cu.usbmodem101 info
python3 tools/mac_control.py --port /dev/cu.usbmodem101 screenshot mac.png
python3 tools/mac_control.py --port /dev/cu.usbmodem101 click 310 180
python3 tools/mac_control.py --port /dev/cu.usbmodem101 type "Hello from the host"
python3 tools/mac_control.py --port /dev/cu.usbmodem101 key command+s
```

Set `BASILISK_PORT` to omit `--port`. If exactly one USB serial device is
present, the tool also finds the usual macOS and Linux device names
automatically. Keep the serial port in one process at a time; close PlatformIO's
serial monitor before starting the control client.

On the Tab5's native ESP32-P4 USB/JTAG port, the bridge first attaches with DTR
and RTS inactive. On POSIX hosts it sets both lines atomically: setting them
separately can briefly assert RTS alone and reset the P4, even when both were
configured inactive before opening. HUPCL is also cleared to preserve the
control-line state on close. After a 25-second attach-only window it can issue one USB reset recovery
pulse if the protocol still does not answer. Pass `--no-reset` when preserving
the current guest is more important than recovering an unresponsive USB link.
For a testing loop, prefer MCP mode (or one long-lived `MacControl` instance)
so the serial connection stays open across every screenshot and action.

Screenshots are captured at logical resolution and saved as PNG without Pillow.
The attached-computer path defaults to a packetized USB pull with per-packet and
whole-frame CRCs. A small internal-RAM LZ encoder compresses the logical 8-bit
indexed framebuffer and its RGB565 palette without touching the physical-panel
RGB image. A per-boot, unguessable HTTP endpoint remains available on port 8052
as an automatic fallback when USB capture fails.

Protocol 4 transfers up to 768 compressed bytes per request. The client reads
USB data in bulk, retains partial records across read timeouts, and retries only
missing or corrupt blocks. Ordinary command replies carry request IDs, so a
late acknowledgement cannot complete a subsequent action. The device caches
the last tagged command and reply; retries resend the same envelope and return
that cached result without executing input again. A per-boot session ID rejects
commands left over from a previous boot. After retries are exhausted, inspect
the screen before issuing the action again with a new ID.
Old firmware and clients retain the version 3, 48-byte packet protocol.

On POSIX hosts the client requests exclusive serial ownership. Keep one
`MacControl` instance open for the whole test. `connect()` negotiates the
protocol version, and `last_screenshot_stats` reports the serial transfer mode,
compressed payload bytes, capture/total seconds, and block retries.

Mouse and keyboard commands always remain on serial because they are tiny,
ordered, and recoverable with `release-all`. On Tab5 the serial device is native
USB/JTAG, so changing the nominal baud rate does not make bulk transfers faster.
The emulator CPU remains on core 1; serial input, HTTP serving, compression, and
frame capture run in dedicated tasks on core 0. A capture can contain a small
tear if the guest repaints during the snapshot; capture again after the UI
settles when pixel stability matters.

The panel pauses only during snapshot capture/compression, then resumes while
the saved frame is transferred. An abandoned frame expires after 15 seconds
without a packet request. Injected held keys and buttons are released after 30
seconds without a protocol command; send periodic `PING` commands to keep an
intentional longer hold alive. Normal physical input is unaffected.

To measure serial performance and reliability without injecting input or
resetting the device:

```sh
python3 tools/debug_benchmark.py --port /dev/cu.usbmodem101 \
  --samples 20 --output artifacts/serial-debug/check
```

This keeps one connection open, checks ping and both screenshot modes, writes
JSON timings/failures and PNG evidence, and exits nonzero if any sample fails.
It forces serial transport so WiFi cannot hide a USB regression. Serial service
starts after SD/configuration initialization, before the settings screen. A
fatal board, SD, or ROM setup error before that point remains outside the API.

For the pre-v3 Tab5 used for serial development, an opt-in build profile uses
Arduino 3.3.8 / ESP-IDF 5.5.4, matching its previous working firmware:

```sh
pio run -e esp32p4_pioarduino_debug
pio run -e esp32p4_pioarduino_debug -t upload --upload-port /dev/cu.usbmodem101
```

This profile requires PlatformIO Core 6.1.19 or newer. Its application binary is
under `.pio/build/esp32p4_pioarduino_debug/`; it does not overwrite release
images. The release profiles retain their existing SDK pins. The earlier Tab5
SDK failed to mount the SD card on the development unit, so use the tested
profile for that hardware. The boot touch worker now signals completion before
its queue is freed, avoiding a double-delete crash during unattended startup.

Useful commands:

```text
info
screenshot OUTPUT.png
move X Y
move-relative DX DY
click X Y [--button 0|1|2]
drag FROM_X FROM_Y TO_X TO_Y [--duration SECONDS] [--steps N]
type TEXT
key KEY_OR_CHORD [--action tap|down|up]
release-all
```

`type` supports the US-ASCII characters available on the emulated US keyboard.
Named keys include `return`, `tab`, `space`, `delete`, `escape`, `control`,
`command`, `shift`, `option`, and the arrow keys. Numeric ADB keycodes are also
accepted. `release-all` is the recovery command if a client disconnects while a
key or mouse button is held.

## Preboot configuration

All eleven persisted settings are available over USB serial. Run `boot-enter`
to reboot once into settings; the device stays there until touch or `boot-start`
boots it. The client reconnects using the new boot session automatically. This
restarts the guest: firmware flushes SD handles, clock, and PRAM at a CPU safe
point, but it does not ask Mac OS to shut down. Shut down the guest first when
a clean guest filesystem is required. An ordinary later power cycle retains
the existing splash/tap behavior.

```sh
python3 tools/mac_control.py boot-enter
python3 tools/mac_control.py boot-status
python3 tools/mac_control.py boot-get
python3 tools/mac_control.py boot-list disk
python3 tools/mac_control.py boot-list cdrom
python3 tools/mac_control.py boot-list extfs
python3 tools/mac_control.py boot-set disk "/Macintosh.dsk"
python3 tools/mac_control.py boot-set ramsize 16
python3 tools/mac_control.py boot-set audio false
python3 tools/mac_control.py boot-set rotation 0
python3 tools/mac_control.py boot-save
python3 tools/mac_control.py boot-start
```

| Key | Values |
| --- | --- |
| `disk` | Existing SD disk image path (required) |
| `cdrom` | Existing SD CD image path; `""` disables it |
| `extfs` | Existing SD directory path; `""` disables sharing |
| `ramsize` | `4`, `8`, `12`, or `16` MiB |
| `audio`, `boot_from_cd` | `true` or `false` |
| `rotation` | `0` or `180` degrees; hardware support as in the touch UI |
| `wifi_ssid` | UTF-8 string, at most 32 bytes |
| `wifi_pass` | Write-only string, at most 63 bytes; `""` clears it |
| `wifi_auto` | `true` or `false` |
| `skip_gui` | Legacy saved flag; explicit entry always opens settings |

`boot-get [KEY]` reads the current selections, including during emulation;
`wifi_password_set` reports password presence without exposing its value.
`boot-set` requires the main settings screen, updates the touch UI, and changes
the selections in memory. `boot-save` persists them; `boot-start` validates the
media, saves, and starts the emulator. It returns when emulator initialization
finishes; use screenshots to wait for Finder. Changed Wi-Fi credentials with
auto-connect enabled are applied before this boot, with a connection wait of
up to ten seconds. They also persist for later power cycles.
`boot-wifi-connect` starts an asynchronous preboot connection using current credentials; `boot-wifi-disconnect`
drops it without changing saved settings. Use `NET` for connection status.

`boot-reload` discards unsaved selections and reloads the saved file, including
the password. `boot-rescan` refreshes all three SD lists. To leave the Wi-Fi
subscreen, use `boot-enter` again. Configuration operations are rejected while
USB Disk mode owns the SD card; exit that mode on the device first.

Settings writes use a verified temporary file and a recoverable backup. A
failed save reports an error and does not boot. Newline/control characters,
unknown keys, unsupported choices, overlong values, and nonexistent paths are
rejected rather than truncated. Each change is one acknowledged operation;
multiple changes become persistent together with `save` or `start`. Touch input
and serial changes run on the same task. A queued request expires after three
seconds rather than executing much later; after a timeout, read back before
issuing a new action. Version 4 retry IDs also cover these commands.

For a persistent Python test connection:

```python
from mac_control import MacControl

with MacControl(reset_on_failure=False) as device:
    device.connect()
    device.boot_enter()
    before = device.boot_get()
    device.boot_set("ramsize", 16)
    device.boot_set("audio", False)
    device.boot_start()
    # Run guest tests, then enter preboot and restore the selected settings.
```

## LLM/MCP mode

The same script is a stdio MCP server with a persistent serial connection:

```sh
python3 /absolute/path/to/tools/mac_control.py \
  --port /dev/cu.usbmodem101 mcp
```

Configure an MCP-capable agent to launch that command. It exposes these tools:

- `mac_screenshot`
- `mac_move`
- `mac_click`
- `mac_drag`
- `mac_type`
- `mac_key`
- `mac_release_all`
- `mac_info`
- `mac_boot_status`, `mac_boot_get`, `mac_boot_set`, `mac_boot_list`
- `mac_boot_action` (`enter`, `start`, `save`, `reload`, `rescan`,
  `wifi-connect`, `wifi-disconnect`)

`mac_screenshot` returns an MCP image content block directly, enabling the loop
"capture -> inspect -> act -> capture" without temporary files. The server uses
newline-delimited MCP JSON-RPC over stdio and writes its own diagnostics only to
stderr.

## Wire protocol

The firmware ignores input lines without the `@B2 ` prefix, allowing diagnostic
logs and automation traffic to share the serial link. Protocol version 4
supports:

```text
@B2 PING
@B2 INFO
@B2 NET
@B2 HTTP
@B2 BOOT STATUS
@B2 BOOT ENTER
@B2 BOOT GET key
@B2 BOOT SET key hex_utf8_value
@B2 BOOT LIST disk|cdrom|extfs zero_based_index
@B2 BOOT RESCAN
@B2 BOOT RELOAD
@B2 BOOT SAVE
@B2 BOOT START
@B2 BOOT WIFI CONNECT|DISCONNECT
@B2 SCREENSHOT
@B2 SCREENSHOT BATCH frame_id first_sequence count
@B2 SCREENSHOT CHUNK frame_id sequence
@B2 SCREENSHOT CLOSE frame_id
@B2 SCREENSHOT READ frame_id byte_offset byte_count
@B2 SCREENSHOT ABORT
@B2 REQ 0123ABCD boot_session PING
@B2 MOUSE MOVE x y
@B2 MOUSE REL dx dy
@B2 MOUSE DOWN|UP button
@B2 MOUSE CLICK x y [button]
@B2 KEY DOWN|UP|TAP adb_keycode
@B2 TYPE base64_us_ascii
@B2 RELEASE_ALL
@B2 TRAPS RESET
@B2 TRAPS [rank_offset]
@B2 LAYER [rank_offset]
@B2 QDACCEL
@B2 QDREGION
```

Boot responses are `OK BOOT STATUS phase screen restart_pending`,
`OK BOOT VALUE key hex_utf8_value`, `OK BOOT ITEM total_count hex_utf8_path`,
or `OK BOOT <action>`. A dash encodes an empty string. Integers and booleans
are hex-encoded decimal / `true` / `false` strings. List index zero also reports
a zero-length list (`0 -`). Errors are `ERR BOOT reason`. Guest input and
screenshots before emulator readiness return `ERR emulator_not_ready`.

`READ` accepts 1–768 bytes within the current immutable frame and responds with
`@B2 R frame_id byte_offset hex_data CRC32`. The header and whole-frame CRC are
unchanged from v3. Each block must match its frame, offset, length and CRC.
`ABORT` idempotently releases a serial frame even if its header was lost.
For commands returning `OK`/`ERR` (including `SCREENSHOT CLOSE` and `ABORT`),
`PING` returns `@B2 OK PONG 4 boot_session`. `REQ` takes an eight-digit
hexadecimal request ID followed by that eight-digit boot session and returns
`@B2 RES 0123ABCD OK PONG 4 boot_session` or a matching tagged error. Requests
must be serialized: only the most recent tagged request is cached. Reusing its
ID with a different command is rejected. Screenshot capture and
data reads use their existing nonce/frame identifiers and are sent unwrapped.

`NET` reports connection state and `HTTP` returns the tokenized framebuffer URL
when it is ready. Use the host tool instead of parsing frame traffic directly:
it negotiates HTTP or serial automatically and validates frame boundaries,
payload length, CRC32, decompression length, palette conversion, and PNG
generation.

## Speedometer regression runner

`tools/speedometer_benchmark.py` keeps one serial session open across boot,
clicks through the Speedometer splash and registration prompts, configures the
requested suite, captures periodic evidence, and scores the final Performance
Rating against the checked-in baseline values:

```sh
python3 tools/speedometer_benchmark.py --port /dev/cu.usbmodem14201 \
  --no-reset --suite all --label release-check
python3 tools/speedometer_benchmark.py --port /dev/cu.usbmodem14201 \
  --no-reset --suite mono --label graphics-proxy --poll-interval 5
python3 tools/speedometer_benchmark.py --port /dev/cu.usbmodem14201 \
  --no-reset --suite color --label eight-bit-color --poll-interval 5
```

Focused `mono` and `color` runs read Speedometer's persisted checkbox state and
set an absolute one-test configuration. Run artifacts, screenshots, and JSON
profiles are written below `artifacts/performance-runs/`. The full-suite runner
does not accept the live subtotal window as a final score; it waits for the
explicit tests-complete dialog first.

The `rating` suite runs **Tests > Performance Rating** (Command-R), the usual
headline measurement. It selects the named tests and iteration counts in the
PR dialog, accepts the Disk drive prompt, and records every visible score:

```sh
python3 tools/speedometer_benchmark.py --no-reset --suite rating --label pr
python3 tools/speedometer_benchmark.py --no-reset --suite rating \
  --tests graphics --label graphics-only
python3 tools/speedometer_benchmark.py --no-reset --suite rating \
  --tests cpu --iterations 5 --profile 3000 --label cpu-profile
python3 tools/perf_report.py artifacts/performance-runs/<run> \
  .pio/build/esp32p4_pioarduino_debug/firmware.elf
```

Speedometer's launch splash is artwork that OCR cannot read, while the
disabled menu bar behind it still reads as the application menus. The runner
recognises the splash from pixels (grey menu titles plus a dark picture) and
clicks through it. It opens the desktop alias at the position of its OCR label
("Speedometer…"), because Finder rearranges desktop icons, and falls back to
(590, 222) when the label is unreadable. Single-test runs repeat within about
1%; compare builds with three runs each. Math-only runs repeat within 0.2%,
but full ratings on one build ranged from 9.30 to 9.68 on Math, so compare Math
only within one procedure.

`--profile HZ` samples one core with the firmware's diagnostic PERF sampler
while the tests run (`--profile-core 0` for the display/automation core) and
saves `perf.json` (cycles, retired instructions, 68k instruction count,
L1/L2 cache counters for both cores) and `perf-samples.bin` (host PC and
return address, 68k PC and opcode per sample). `tools/perf_report.py` needs
the exact firmware ELF that was running. The cache "miss" registers advance
per stall cycle rather than per line fill; read them as stall proxies.

Diagnostic commands (all read-only for the guest):

| Command | Reply |
|---|---|
| `PERF START hz capacity [core]` / `PERF STOP` / `PERF FREE` | Start/stop the sampler; free its PSRAM buffer |
| `PERF STATE` | Counters for the last window and sample count |
| `PERF READ offset` | Up to 24 raw samples as hex |
| `PEEK hexaddr length` | Up to 256 bytes of guest RAM or ROM as hex |
| `PANEL VERIFY` | Compares every physical panel pixel with the guest frame buffer's palette color |

`PANEL VERIFY` reads the panel through the display's scanout view, so it
checks what is actually shown, including scaling and rotation. Run it on a
settled screen with the touch overlay hidden; pixels the guest changes during
the scan count as mismatches.
