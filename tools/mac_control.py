#!/usr/bin/env python3
"""Drive BasiliskII ESP32 over its attached USB CDC/UART serial port.

This module is both a normal CLI and a dependency-light MCP stdio server. It
intentionally writes PNGs with the Python standard library; only pyserial is
required for the transport.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import glob
import json
import os
from pathlib import Path
import struct
import sys
import time
from typing import Any, BinaryIO, Iterable
import urllib.error
import urllib.request
import zlib


PROTOCOL_PREFIX = "@B2 "
DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT = 120.0
SCREENSHOT_BATCH_SIZE = 4
MONO_RGB_LOOKUP = tuple(
    b"".join(
        b"\xff\xff\xff" if value & (0x80 >> bit) else b"\x00\x00\x00"
        for bit in range(8)
    )
    for value in range(256)
)


class ControlError(RuntimeError):
    pass


def find_serial_port() -> str:
    patterns = (
        "/dev/cu.usbmodem*",
        "/dev/cu.usbserial*",
        "/dev/ttyACM*",
        "/dev/ttyUSB*",
    )
    ports = sorted({path for pattern in patterns for path in glob.glob(pattern)})
    if not ports:
        raise ControlError(
            "no USB serial device found; pass --port or set BASILISK_PORT"
        )
    if len(ports) > 1:
        choices = ", ".join(ports)
        raise ControlError(f"multiple USB serial devices found ({choices}); pass --port")
    return ports[0]


def indexed_rgb565_to_rgb(palette: bytes, indices: bytes) -> bytes:
    if len(palette) != 512:
        raise ControlError(f"invalid RGB565 palette size: {len(palette)}")
    colors: list[bytes] = []
    for index in range(256):
        value = palette[index * 2] | (palette[index * 2 + 1] << 8)
        red = (((value >> 11) & 0x1F) * 255 + 15) // 31
        green = (((value >> 5) & 0x3F) * 255 + 31) // 63
        blue = ((value & 0x1F) * 255 + 15) // 31
        colors.append(bytes((red, green, blue)))
    return b"".join(colors[index] for index in indices)


def encode_png(width: int, height: int, rgb: bytes) -> bytes:
    if width <= 0 or height <= 0 or len(rgb) != width * height * 3:
        raise ControlError("invalid RGB image dimensions")

    def chunk(kind: bytes, payload: bytes) -> bytes:
        checksum = binascii.crc32(kind + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", checksum)

    row_size = width * 3
    scanlines = b"".join(
        b"\x00" + rgb[row * row_size : (row + 1) * row_size]
        for row in range(height)
    )
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(scanlines, 6))
        + chunk(b"IEND", b"")
    )


def decode_b2lz(data: bytes, expected_size: int) -> bytes:
    """Decode the firmware's small, linear-time framebuffer LZ stream."""
    output = bytearray()
    position = 0
    while position < len(data):
        token = data[position]
        position += 1
        if token & 0x80:
            if position + 2 > len(data):
                raise ControlError("truncated b2lz match")
            length = (token & 0x7F) + 4
            distance = data[position] | (data[position + 1] << 8)
            position += 2
            if distance == 0 or distance > len(output):
                raise ControlError("invalid b2lz match distance")
            if len(output) + length > expected_size:
                raise ControlError("b2lz output exceeds framebuffer size")
            for _ in range(length):
                output.append(output[-distance])
        else:
            length = token + 1
            if position + length > len(data):
                raise ControlError("truncated b2lz literal")
            if len(output) + length > expected_size:
                raise ControlError("b2lz output exceeds framebuffer size")
            output.extend(data[position : position + length])
            position += length
    if len(output) != expected_size:
        raise ControlError(
            f"b2lz pixel size mismatch: expected {expected_size}, got {len(output)}"
        )
    return bytes(output)


class MacControl:
    def __init__(
        self,
        port: str | None = None,
        baud: int = DEFAULT_BAUD,
        reset_on_failure: bool = True,
    ):
        try:
            import serial  # type: ignore
            from serial.tools import list_ports  # type: ignore
        except ImportError as exc:
            raise ControlError(
                "pyserial is required; run: python3 -m pip install -r tools/requirements.txt"
            ) from exc

        self.port = port or os.environ.get("BASILISK_PORT") or find_serial_port()
        connection = serial.Serial()
        connection.port = self.port
        connection.baudrate = baud
        connection.timeout = 0.25
        connection.write_timeout = 5
        # Opening a native USB/JTAG tty with pyserial's asserted defaults can
        # reset the P4. An automation client should attach to a running Mac
        # without disturbing it, so preload both lines inactive. connect()
        # still has one bounded recovery pulse if attach-only probing fails.
        connection.dtr = False
        connection.rts = False
        connection.open()
        # macOS enables HUPCL on USB CDC ttys by default. Dropping the last
        # file descriptor then changes the control-line state and resets the
        # ESP32-P4, which makes a sequence of one-shot CLI actions unusable.
        # Keep the inactive DTR/RTS state across close; the MCP server already
        # benefits from its persistent connection, but CLI users need this too.
        if os.name == "posix":
            try:
                import termios

                attributes = termios.tcgetattr(connection.fileno())
                attributes[2] &= ~termios.HUPCL
                termios.tcsetattr(connection.fileno(), termios.TCSANOW, attributes)
            except (ImportError, AttributeError, OSError):
                # HUPCL control is a best-effort POSIX optimization. Platforms
                # without termios retain the persistent MCP connection path.
                pass
        self.serial = connection
        port_info = next(
            (item for item in list_ports.comports() if item.device == self.port),
            None,
        )
        self._uses_usb_jtag = bool(
            port_info is not None
            and port_info.vid == 0x303A
            and port_info.pid == 0x1001
        )
        self._reset_on_failure = reset_on_failure
        self._recovery_reset_done = False
        self._http_url: str | bool | None = None

    def close(self) -> None:
        if self.serial.is_open:
            self.serial.close()

    def __enter__(self) -> "MacControl":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def _write(self, command: str) -> None:
        self.serial.write(f"{PROTOCOL_PREFIX}{command}\n".encode("ascii"))
        self.serial.flush()

    def _reset_usb_jtag(self) -> None:
        """Reset an ESP32 native USB-Serial/JTAG port into the application.

        This is the application-side hard-reset sequence used by esptool for a
        native USB target. Merely opening CDC with idle DTR/RTS is not
        sufficient after a host disconnect: ESP32-P4 reports
        CHIP_USB_UART_RESET and can remain in reset until RTS is pulsed.
        """
        # macOS reports the tty before the USB control endpoint is consistently
        # ready. A line-state pulse sent immediately after open is sometimes
        # acknowledged by pyserial but silently dropped by the device.
        time.sleep(1.0)
        try:
            self.serial.dtr = False
            self.serial.rts = True
            time.sleep(0.2)
            self.serial.rts = False
            return
        except OSError as first_error:
            # Some macOS USB stacks invalidate the fd as soon as reset is
            # asserted. Reopen the same device path and continue listening for
            # the boot that the assertion already initiated.
            try:
                self.serial.close()
            except OSError:
                pass
            for _ in range(20):
                time.sleep(0.25)
                try:
                    self.serial.dtr = False
                    self.serial.rts = False
                    self.serial.open()
                    return
                except OSError:
                    try:
                        self.serial.close()
                    except OSError:
                        pass
            raise ControlError(
                "ESP32-P4 USB serial port did not reappear after reset"
            ) from first_error

    @staticmethod
    def _protocol_fragments(raw: bytes) -> Iterable[str]:
        line = raw.decode("ascii", errors="replace").strip()
        # Diagnostics and protocol replies originate on different P4 tasks. A
        # diagnostic write can occasionally prefix an otherwise intact reply
        # (for example ``[FPU@B2 OK PONG 1``). The native USB/JTAG CDC path can
        # also omit a newline between duplicate records. Split every marker so
        # the intact copy remains independently parseable.
        for fragment in line.split(PROTOCOL_PREFIX)[1:]:
            yield PROTOCOL_PREFIX + fragment

    def _protocol_lines(self, deadline: float) -> Iterable[str]:
        while time.monotonic() < deadline:
            raw = self.serial.readline()
            if not raw:
                continue
            yield from self._protocol_fragments(raw)

    def request(self, command: str, expected: str, timeout: float = 5.0) -> str:
        self._write(command)
        deadline = time.monotonic() + timeout
        for line in self._protocol_lines(deadline):
            if line.startswith(f"{PROTOCOL_PREFIX}ERR"):
                raise ControlError(line[len(PROTOCOL_PREFIX) :])
            if line.startswith(f"{PROTOCOL_PREFIX}{expected}"):
                return line
        raise ControlError(f"timed out waiting for {expected!r} after {command!r}")

    def connect(self, timeout: float = DEFAULT_TIMEOUT) -> str:
        deadline = time.monotonic() + timeout
        last_error: Exception | None = None

        # Preserve the guest first. This is long enough for the normal board,
        # SD, and boot-GUI path when the host attaches during power-up.
        attach_deadline = min(deadline, time.monotonic() + 25.0)
        while time.monotonic() < attach_deadline:
            try:
                response = self.request("PING", "OK PONG", timeout=1.0)
                self._recovery_reset_done = True
                return response
            except (ControlError, OSError) as exc:
                last_error = exc
                time.sleep(0.2)

        # A recovery reset is deliberately a last resort and is issued only
        # once. Repeated resets can strand warm-boot peripherals and destroy
        # the very guest state a machine-control client is trying to inspect.
        if (
            self._uses_usb_jtag
            and self._reset_on_failure
            and not self._recovery_reset_done
            and time.monotonic() < deadline
        ):
            self._reset_usb_jtag()
            self._recovery_reset_done = True
            while time.monotonic() < deadline:
                try:
                    return self.request("PING", "OK PONG", timeout=1.0)
                except (ControlError, OSError) as exc:
                    last_error = exc
                    time.sleep(0.2)
        raise ControlError(
            f"device on {self.port} did not answer the @B2 protocol within {timeout:g}s"
        ) from last_error

    def info(self) -> str:
        return self.request("INFO", "OK INFO")

    def screenshot_png(self, timeout: float = 90.0) -> tuple[bytes, int, int]:
        """Capture the fast 1-bit machine-control view as a PNG."""
        # USB is deterministic on the attached-computer workflow. The Tab5's
        # hosted WiFi can report connected while delivering only a few hundred
        # bytes per second, so use it only if the integrity-checked USB pull
        # cannot complete.
        started = time.monotonic()
        last_error: ControlError | None = None
        try:
            return self._screenshot_png_once(timeout, monochrome=True)
        except ControlError as exc:
            last_error = exc

        http_error: Exception | None = None
        try:
            http_url = self._get_http_url()
            remaining = timeout - (time.monotonic() - started)
            if http_url and remaining > 0:
                return self._screenshot_png_http(http_url, min(remaining, 35.0))
        except (ControlError, OSError, urllib.error.URLError) as exc:
            # WiFi can roam or disconnect while the emulator continues. The
            # integrity-checked serial pull path remains the universal fallback.
            http_error = exc
            self._http_url = None
        if http_error is not None and last_error is None:
            raise ControlError("WiFi and serial screenshot transports failed") from http_error
        raise ControlError("screenshot failed over USB and WiFi") from last_error

    def screenshot_color_png(
        self, timeout: float = 90.0
    ) -> tuple[bytes, int, int]:
        """Capture the full indexed-color framebuffer over integrity-checked USB."""
        return self._screenshot_png_once(timeout, monochrome=False)

    def _get_http_url(self) -> str | None:
        if isinstance(getattr(self, "_http_url", None), str):
            return self._http_url
        if getattr(self, "_http_url", None) is False:
            return None
        try:
            line = self.request("HTTP", "OK HTTP", timeout=2.0)
        except ControlError:
            return None
        url = line.split()[-1]
        if not url.startswith("http://"):
            raise ControlError(f"invalid HTTP framebuffer URL {url!r}")
        self._http_url = url
        return url

    def _screenshot_png_http(
        self, url: str, timeout: float
    ) -> tuple[bytes, int, int]:
        request = urllib.request.Request(
            url, headers={"Accept": "application/x-b2-indexed-frame"}
        )
        with urllib.request.urlopen(request, timeout=timeout) as response:
            if response.headers.get_content_type() != "application/x-b2-indexed-frame":
                raise ControlError("unexpected HTTP screenshot content type")
            width = int(response.headers["X-B2-Width"])
            height = int(response.headers["X-B2-Height"])
            raw_size = int(response.headers["X-B2-Raw-Size"])
            expected_crc = int(response.headers["X-B2-CRC32"], 16)
            encoding = response.headers.get("X-B2-Encoding", "zlib")
            expected_size = int(response.headers["Content-Length"])
            payload = response.read()
        if len(payload) != expected_size:
            raise ControlError(
                f"HTTP screenshot size mismatch: expected {expected_size}, got {len(payload)}"
            )
        return self._payload_to_png(
            payload, width, height, raw_size, expected_crc, encoding
        )

    def _screenshot_png_once(
        self, timeout: float, monochrome: bool = True
    ) -> tuple[bytes, int, int]:
        # A prior timed-out transfer can leave a valid-looking frame header in
        # the OS tty buffer after the firmware has released that frame. Never
        # pair a new chunk request with stale capture metadata.
        try:
            self.serial.reset_input_buffer()
        except (AttributeError, OSError):
            pass
        deadline = time.monotonic() + timeout
        header: list[str] | None = None
        # A header is a single USB packet but the first packet after an idle
        # period can still be lost. Retry with a new nonce; delayed replies for
        # older requests are ignored and can never be paired with a new frame.
        while time.monotonic() < deadline and header is None:
            nonce_value = (getattr(self, "_screenshot_nonce", time.monotonic_ns()) + 1) & 0xFFFF
            self._screenshot_nonce = nonce_value
            nonce = f"{nonce_value:04X}"
            command = f"SCREENSHOT MONO {nonce}" if monochrome else f"SCREENSHOT {nonce}"
            response_kind = "M2" if monochrome else "F2"
            self._write(command)
            attempt_deadline = min(deadline, time.monotonic() + 1.0)
            for line in self._protocol_lines(attempt_deadline):
                if line.startswith(f"{PROTOCOL_PREFIX}ERR"):
                    raise ControlError(line[len(PROTOCOL_PREFIX) :])
                parts = line.split()
                if (
                    len(parts) == 11
                    and parts[:2] == ["@B2", response_kind]
                    and parts[2] == nonce
                ):
                    header = parts
                    break
        if header is None:
            raise ControlError("timed out starting screenshot")

        frame_id = header[3]
        width = int(header[4])
        height = int(header[5])
        payload_size = int(header[6])
        raw_size = int(header[7])
        expected_crc = int(header[8], 16)
        chunk_count = int(header[9])
        encoding = header[10]

        payload = bytearray()
        previous_serial_timeout = self.serial.timeout
        self.serial.timeout = 0.02
        try:
            for start in range(0, chunk_count, SCREENSHOT_BATCH_SIZE):
                count = min(SCREENSHOT_BATCH_SIZE, chunk_count - start)
                payload.extend(
                    self._screenshot_batch(frame_id, start, count, deadline)
                )
        finally:
            self.serial.timeout = previous_serial_timeout
            try:
                self.request(
                    f"SCREENSHOT CLOSE {frame_id}",
                    f"OK SCREENSHOT CLOSE {frame_id}",
                    timeout=2.0,
                )
            except ControlError:
                # A later SCREENSHOT releases stale capture memory as well.
                pass

        if len(payload) != payload_size:
            raise ControlError(
                f"screenshot payload size mismatch: expected {payload_size}, got {len(payload)}"
            )

        if monochrome:
            return self._monochrome_payload_to_png(
                bytes(payload), width, height, raw_size, expected_crc, encoding
            )
        return self._payload_to_png(
            bytes(payload), width, height, raw_size, expected_crc, encoding
        )

    def _screenshot_batch(
        self, frame_id: str, start: int, count: int, deadline: float
    ) -> bytes:
        """Pull several fixed-size USB packets with one host round trip.

        Each packet keeps its own CRC and sequence number. If a diagnostic log
        damages one record, only that record is retried through the legacy
        single-chunk command.
        """
        self._write(f"SCREENSHOT BATCH {frame_id} {start} {count}")
        pending = set(range(start, start + count))
        received: dict[int, bytes] = {}
        batch_deadline = min(deadline, time.monotonic() + 0.2 + count * 0.04)
        batch_complete = f"{PROTOCOL_PREFIX}OK SCREENSHOT BATCH {frame_id} {start} {count}"
        batch_done = False
        while pending and not batch_done and time.monotonic() < batch_deadline:
            raw = self.serial.readline()
            if not raw:
                continue
            for line in self._protocol_fragments(raw):
                if line.startswith(batch_complete):
                    batch_done = True
                    break
                record = self._parse_screenshot_record(line, frame_id)
                if record is None:
                    continue
                sequence, data = record
                if sequence not in pending:
                    continue
                received[sequence] = data
                pending.remove(sequence)
            if not pending:
                break

        for sequence in sorted(pending):
            received[sequence] = self._screenshot_chunk(
                frame_id, sequence, deadline
            )
        return b"".join(received[sequence] for sequence in range(start, start + count))

    def _payload_to_png(
        self,
        payload: bytes,
        width: int,
        height: int,
        raw_size: int,
        expected_crc: int,
        encoding: str = "zlib",
    ) -> tuple[bytes, int, int]:
        actual_crc = zlib.crc32(payload) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise ControlError(
                f"screenshot CRC mismatch: expected {expected_crc:08X}, got {actual_crc:08X}"
            )
        if raw_size != width * height:
            raise ControlError("screenshot raw size does not match its dimensions")

        palette = payload[:512]
        if encoding.lower() in {"l", "b2lz"}:
            indices = decode_b2lz(payload[512:], raw_size)
        else:
            try:
                indices = zlib.decompress(payload[512:])
            except zlib.error as exc:
                raise ControlError("invalid zlib screenshot payload") from exc
        if len(indices) != raw_size:
            raise ControlError(
                f"screenshot pixel size mismatch: expected {raw_size}, got {len(indices)}"
            )
        rgb = indexed_rgb565_to_rgb(palette, indices)
        return encode_png(width, height, rgb), width, height

    def _monochrome_payload_to_png(
        self,
        payload: bytes,
        width: int,
        height: int,
        raw_size: int,
        expected_crc: int,
        encoding: str = "b2lz",
    ) -> tuple[bytes, int, int]:
        actual_crc = zlib.crc32(payload) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise ControlError(
                f"screenshot CRC mismatch: expected {expected_crc:08X}, got {actual_crc:08X}"
            )
        packed_size = (width * height + 7) // 8
        if raw_size != packed_size:
            raise ControlError("monochrome screenshot size does not match its dimensions")
        if encoding.lower() not in {"l", "b2lz"}:
            raise ControlError(f"unsupported monochrome encoding {encoding!r}")
        packed = decode_b2lz(payload, packed_size)
        rgb = b"".join(MONO_RGB_LOOKUP[value] for value in packed)
        return encode_png(width, height, rgb[: width * height * 3]), width, height

    def _screenshot_chunk(
        self, frame_id: str, sequence: int, deadline: float
    ) -> bytes:
        last_error: Exception | None = None
        for _ in range(5):
            if time.monotonic() >= deadline:
                break
            self._write(f"SCREENSHOT CHUNK {frame_id} {sequence}")
            packet_deadline = min(deadline, time.monotonic() + 0.5)
            for line in self._protocol_lines(packet_deadline):
                record = self._parse_screenshot_record(line, frame_id)
                if record is None:
                    continue
                received_sequence, data = record
                if received_sequence == sequence:
                    return data
        raise ControlError(
            f"could not receive valid screenshot chunk {sequence} after 5 attempts"
        ) from last_error

    @staticmethod
    def _parse_screenshot_record(
        line: str, frame_id: str
    ) -> tuple[int, bytes] | None:
        parts = line.split()
        if len(parts) != 6 or parts[:2] != ["@B2", "D"] or parts[2] != frame_id:
            return None
        try:
            sequence = int(parts[3])
            data = bytes.fromhex(parts[4])
            expected_crc = int(parts[5], 16)
        except ValueError:
            return None
        if len(data) > 48 or (zlib.crc32(data) & 0xFFFFFFFF) != expected_crc:
            return None
        return sequence, data

    def move(self, x: int, y: int) -> None:
        self.request(f"MOUSE MOVE {x} {y}", "OK MOUSE MOVE")

    def move_relative(self, dx: int, dy: int) -> None:
        self.request(f"MOUSE REL {dx} {dy}", "OK MOUSE REL")

    def mouse_button(self, button: int, pressed: bool) -> None:
        action = "DOWN" if pressed else "UP"
        self.request(f"MOUSE {action} {button}", f"OK MOUSE {action}")

    def click(self, x: int, y: int, button: int = 0) -> None:
        self.request(f"MOUSE CLICK {x} {y} {button}", "OK MOUSE CLICK")

    def drag(
        self,
        from_x: int,
        from_y: int,
        to_x: int,
        to_y: int,
        duration: float = 0.5,
        steps: int = 10,
        button: int = 0,
    ) -> None:
        steps = max(1, steps)
        self.move(from_x, from_y)
        self.mouse_button(button, True)
        try:
            for step in range(1, steps + 1):
                ratio = step / steps
                x = round(from_x + (to_x - from_x) * ratio)
                y = round(from_y + (to_y - from_y) * ratio)
                self.move(x, y)
                if duration > 0:
                    time.sleep(duration / steps)
        finally:
            self.mouse_button(button, False)

    def key_code(self, code: int, action: str = "tap") -> None:
        action = action.upper()
        if action not in {"DOWN", "UP", "TAP"}:
            raise ControlError(f"invalid key action {action!r}")
        self.request(f"KEY {action} {code}", f"OK KEY {action}")

    def type_text(self, text: str) -> None:
        try:
            data = text.encode("ascii")
        except UnicodeEncodeError as exc:
            raise ControlError("TYPE currently supports the Mac US-ASCII keyboard set") from exc
        # Keep the base64 command below the firmware's 1024-byte line buffer.
        for offset in range(0, len(data), 600):
            encoded = base64.b64encode(data[offset : offset + 600]).decode("ascii")
            self.request(f"TYPE {encoded}", "OK TYPE", timeout=60.0)

    def release_all(self) -> None:
        self.request("RELEASE_ALL", "OK RELEASE_ALL")


ADB_KEYS = {
    "a": 0x00,
    "s": 0x01,
    "d": 0x02,
    "f": 0x03,
    "h": 0x04,
    "g": 0x05,
    "z": 0x06,
    "x": 0x07,
    "c": 0x08,
    "v": 0x09,
    "b": 0x0B,
    "q": 0x0C,
    "w": 0x0D,
    "e": 0x0E,
    "r": 0x0F,
    "y": 0x10,
    "t": 0x11,
    "1": 0x12,
    "2": 0x13,
    "3": 0x14,
    "4": 0x15,
    "6": 0x16,
    "5": 0x17,
    "=": 0x18,
    "9": 0x19,
    "7": 0x1A,
    "-": 0x1B,
    "8": 0x1C,
    "0": 0x1D,
    "o": 0x1F,
    "u": 0x20,
    "i": 0x22,
    "p": 0x23,
    "return": 0x24,
    "enter": 0x24,
    "l": 0x25,
    "j": 0x26,
    "k": 0x28,
    "n": 0x2D,
    "m": 0x2E,
    "tab": 0x30,
    "space": 0x31,
    "delete": 0x33,
    "backspace": 0x33,
    "escape": 0x35,
    "esc": 0x35,
    "control": 0x36,
    "ctrl": 0x36,
    "command": 0x37,
    "cmd": 0x37,
    "shift": 0x38,
    "capslock": 0x39,
    "option": 0x3A,
    "opt": 0x3A,
    "alt": 0x3A,
    "left": 0x3B,
    "right": 0x3C,
    "down": 0x3D,
    "up": 0x3E,
}
MODIFIER_KEYS = {"control", "ctrl", "command", "cmd", "shift", "option", "opt", "alt"}


def parse_key_code(name: str) -> int:
    normalized = name.strip().lower()
    if normalized in ADB_KEYS:
        return ADB_KEYS[normalized]
    try:
        value = int(normalized, 0)
    except ValueError as exc:
        raise ControlError(f"unknown key {name!r}") from exc
    if not 0 <= value <= 0x7F:
        raise ControlError("ADB keycode must be between 0 and 0x7f")
    return value


def send_key(control: MacControl, key: str, action: str = "tap") -> None:
    names = [part.strip().lower() for part in key.split("+") if part.strip()]
    if not names:
        raise ControlError("key cannot be empty")
    codes = [parse_key_code(name) for name in names]
    action = action.lower()
    if action == "down":
        for code in codes:
            control.key_code(code, "down")
    elif action == "up":
        for code in reversed(codes):
            control.key_code(code, "up")
    elif action == "tap":
        for code in codes[:-1]:
            control.key_code(code, "down")
        try:
            control.key_code(codes[-1], "tap")
        finally:
            for code in reversed(codes[:-1]):
                control.key_code(code, "up")
    else:
        raise ControlError(f"invalid key action {action!r}")


MCP_TOOLS = [
    {
        "name": "mac_screenshot",
        "description": "Capture a fast monochrome view of the emulated Mac screen as PNG.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "mac_screenshot_color",
        "description": "Capture the full-color emulated Mac screen as PNG (slower).",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "mac_move",
        "description": "Move the Mac pointer to logical screen coordinates.",
        "inputSchema": {
            "type": "object",
            "properties": {"x": {"type": "integer"}, "y": {"type": "integer"}},
            "required": ["x", "y"],
            "additionalProperties": False,
        },
    },
    {
        "name": "mac_click",
        "description": "Move to logical screen coordinates and click a mouse button.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x": {"type": "integer"},
                "y": {"type": "integer"},
                "button": {"type": "integer", "minimum": 0, "maximum": 2, "default": 0},
            },
            "required": ["x", "y"],
            "additionalProperties": False,
        },
    },
    {
        "name": "mac_drag",
        "description": "Drag between two logical Mac screen coordinates.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "from_x": {"type": "integer"},
                "from_y": {"type": "integer"},
                "to_x": {"type": "integer"},
                "to_y": {"type": "integer"},
                "duration": {"type": "number", "minimum": 0, "default": 0.5},
            },
            "required": ["from_x", "from_y", "to_x", "to_y"],
            "additionalProperties": False,
        },
    },
    {
        "name": "mac_type",
        "description": "Type US-ASCII text into the emulated Mac.",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
            "additionalProperties": False,
        },
    },
    {
        "name": "mac_key",
        "description": "Tap, press, or release a named Mac key or chord such as command+s.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "key": {"type": "string"},
                "action": {"type": "string", "enum": ["tap", "down", "up"], "default": "tap"},
            },
            "required": ["key"],
            "additionalProperties": False,
        },
    },
    {
        "name": "mac_release_all",
        "description": "Release every mouse button and keyboard key held by automation.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "mac_info",
        "description": "Return firmware protocol, board, and logical Mac screen information.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
]


def call_mcp_tool(control: MacControl, name: str, arguments: dict[str, Any]) -> list[dict[str, Any]]:
    if name == "mac_screenshot":
        png, width, height = control.screenshot_png()
        return [
            {"type": "image", "data": base64.b64encode(png).decode("ascii"), "mimeType": "image/png"},
            {"type": "text", "text": f"Captured logical Mac screen ({width}x{height})."},
        ]
    if name == "mac_screenshot_color":
        png, width, height = control.screenshot_color_png()
        return [
            {"type": "image", "data": base64.b64encode(png).decode("ascii"), "mimeType": "image/png"},
            {"type": "text", "text": f"Captured full-color Mac screen ({width}x{height})."},
        ]
    if name == "mac_move":
        control.move(int(arguments["x"]), int(arguments["y"]))
    elif name == "mac_click":
        control.click(int(arguments["x"]), int(arguments["y"]), int(arguments.get("button", 0)))
    elif name == "mac_drag":
        control.drag(
            int(arguments["from_x"]),
            int(arguments["from_y"]),
            int(arguments["to_x"]),
            int(arguments["to_y"]),
            float(arguments.get("duration", 0.5)),
        )
    elif name == "mac_type":
        control.type_text(str(arguments["text"]))
    elif name == "mac_key":
        send_key(control, str(arguments["key"]), str(arguments.get("action", "tap")))
    elif name == "mac_release_all":
        control.release_all()
    elif name == "mac_info":
        return [{"type": "text", "text": control.info()}]
    else:
        raise ControlError(f"unknown MCP tool {name!r}")
    return [{"type": "text", "text": "OK"}]


def serve_mcp(control: MacControl, input_stream: BinaryIO, output_stream: BinaryIO) -> None:
    """Serve newline-delimited MCP JSON-RPC over stdio."""

    def reply(message: dict[str, Any]) -> None:
        output_stream.write(json.dumps(message, separators=(",", ":")).encode() + b"\n")
        output_stream.flush()

    for raw in input_stream:
        request: Any = None
        try:
            request = json.loads(raw)
            request_id = request.get("id")
            method = request.get("method")
            if method == "initialize":
                requested_version = request.get("params", {}).get("protocolVersion", "2024-11-05")
                result = {
                    "protocolVersion": requested_version,
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "basiliskii-esp32-control", "version": "1.0.0"},
                }
            elif method == "ping":
                result = {}
            elif method == "tools/list":
                result = {"tools": MCP_TOOLS}
            elif method == "tools/call":
                params = request.get("params", {})
                content = call_mcp_tool(control, params.get("name", ""), params.get("arguments", {}))
                result = {"content": content, "isError": False}
            elif method and method.startswith("notifications/"):
                continue
            else:
                raise ControlError(f"unsupported MCP method {method!r}")
            if request_id is not None:
                reply({"jsonrpc": "2.0", "id": request_id, "result": result})
        except Exception as exc:  # Keep the long-running server alive per JSON-RPC request.
            request_id = request.get("id") if isinstance(request, dict) else None
            if request_id is not None:
                reply(
                    {
                        "jsonrpc": "2.0",
                        "id": request_id,
                        "error": {"code": -32000, "message": str(exc)},
                    }
                )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial device (or BASILISK_PORT)")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument(
        "--no-reset",
        action="store_true",
        help="never pulse native USB/JTAG reset if attach-only probing fails",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("info")
    screenshot = subparsers.add_parser("screenshot")
    screenshot.add_argument("output", type=Path)
    screenshot.add_argument(
        "--color", action="store_true", help="capture the slower full-color framebuffer"
    )
    move = subparsers.add_parser("move")
    move.add_argument("x", type=int)
    move.add_argument("y", type=int)
    relative = subparsers.add_parser("move-relative")
    relative.add_argument("dx", type=int)
    relative.add_argument("dy", type=int)
    click = subparsers.add_parser("click")
    click.add_argument("x", type=int)
    click.add_argument("y", type=int)
    click.add_argument("--button", type=int, choices=(0, 1, 2), default=0)
    drag = subparsers.add_parser("drag")
    drag.add_argument("from_x", type=int)
    drag.add_argument("from_y", type=int)
    drag.add_argument("to_x", type=int)
    drag.add_argument("to_y", type=int)
    drag.add_argument("--duration", type=float, default=0.5)
    drag.add_argument("--steps", type=int, default=10)
    type_parser = subparsers.add_parser("type")
    type_parser.add_argument("text")
    key = subparsers.add_parser("key")
    key.add_argument("key", help="named key, numeric ADB code, or chord (command+s)")
    key.add_argument("--action", choices=("tap", "down", "up"), default="tap")
    subparsers.add_parser("release-all")
    subparsers.add_parser("mcp", help="run a stdio MCP server on one persistent serial connection")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        with MacControl(args.port, args.baud, not args.no_reset) as control:
            control.connect()
            if args.command == "info":
                print(control.info())
            elif args.command == "screenshot":
                capture = control.screenshot_color_png if args.color else control.screenshot_png
                png, width, height = capture()
                args.output.write_bytes(png)
                print(f"wrote {args.output} ({width}x{height})")
            elif args.command == "move":
                control.move(args.x, args.y)
            elif args.command == "move-relative":
                control.move_relative(args.dx, args.dy)
            elif args.command == "click":
                control.click(args.x, args.y, args.button)
            elif args.command == "drag":
                control.drag(
                    args.from_x,
                    args.from_y,
                    args.to_x,
                    args.to_y,
                    args.duration,
                    args.steps,
                )
            elif args.command == "type":
                control.type_text(args.text)
            elif args.command == "key":
                send_key(control, args.key, args.action)
            elif args.command == "release-all":
                control.release_all()
            elif args.command == "mcp":
                print(f"Connected to BasiliskII on {control.port}", file=sys.stderr)
                serve_mcp(control, sys.stdin.buffer, sys.stdout.buffer)
        return 0
    except (ControlError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
