# Closed-loop host control

When the emulator is running, its programming/console connection also exposes
an `@B2` automation protocol. The protocol captures the logical Mac screen and
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
and RTS inactive so taking ownership of the port does not disturb the running
guest. After a 25-second attach-only window it can issue one USB reset recovery
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

Mouse and keyboard commands always remain on serial because they are tiny,
ordered, and recoverable with `release-all`. On Tab5 the serial device is native
USB/JTAG, so changing the nominal baud rate does not make bulk transfers faster.
The emulator CPU remains on core 1; serial input, HTTP serving, compression, and
frame capture run in dedicated tasks on core 0. A capture can contain a small
tear if the guest repaints during the snapshot; capture again after the UI
settles when pixel stability matters.

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

`mac_screenshot` returns an MCP image content block directly, enabling the loop
"capture -> inspect -> act -> capture" without temporary files. The server uses
newline-delimited MCP JSON-RPC over stdio and writes its own diagnostics only to
stderr.

## Wire protocol

The firmware ignores input lines without the `@B2 ` prefix, allowing diagnostic
logs and automation traffic to share the serial link. Protocol version 3
supports:

```text
@B2 PING
@B2 INFO
@B2 NET
@B2 HTTP
@B2 SCREENSHOT
@B2 SCREENSHOT BATCH frame_id first_sequence count
@B2 SCREENSHOT CHUNK frame_id sequence
@B2 SCREENSHOT CLOSE frame_id
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
