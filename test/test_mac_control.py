import importlib.util
import io
import json
from pathlib import Path
import struct
import unittest
import zlib
import time
import os
from unittest.mock import patch


MODULE_PATH = Path(__file__).parents[1] / "tools" / "mac_control.py"
SPEC = importlib.util.spec_from_file_location("mac_control", MODULE_PATH)
mac_control = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(mac_control)


class MacControlEncodingTests(unittest.TestCase):
    def test_rgb565_palette_conversion(self):
        palette = bytearray(512)
        values = (0xF800, 0x07E0, 0x001F, 0xFFFF)
        for index, value in enumerate(values):
            palette[index * 2 : index * 2 + 2] = value.to_bytes(2, "little")
        rgb = mac_control.indexed_rgb565_to_rgb(bytes(palette), bytes(range(4)))
        self.assertEqual(rgb, b"\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\xff")

    def test_png_is_well_formed(self):
        png = mac_control.encode_png(2, 1, b"\xff\x00\x00\x00\xff\x00")
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        ihdr_length = struct.unpack(">I", png[8:12])[0]
        self.assertEqual(ihdr_length, 13)
        self.assertEqual(struct.unpack(">II", png[16:24]), (2, 1))
        idat = png.index(b"IDAT")
        compressed_length = struct.unpack(">I", png[idat - 4 : idat])[0]
        scanline = zlib.decompress(png[idat + 4 : idat + 4 + compressed_length])
        self.assertEqual(scanline, b"\x00\xff\x00\x00\x00\xff\x00")

    def test_b2lz_literals_and_overlapping_match(self):
        # "abc" followed by a distance-3, length-6 overlapping match.
        encoded = b"\x02abc\x82\x03\x00"
        self.assertEqual(mac_control.decode_b2lz(encoded, 9), b"abcabcabc")
        with self.assertRaises(mac_control.ControlError):
            mac_control.decode_b2lz(b"\x80\x00\x00", 4)

    def test_monochrome_payload_to_png(self):
        # One literal packed byte: first pixel white, second pixel black.
        payload = b"\x00\x80"
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        control = mac_control.MacControl.__new__(mac_control.MacControl)
        png, width, height = control._monochrome_payload_to_png(
            payload, 2, 1, 1, crc, "L"
        )
        self.assertEqual((width, height), (2, 1))
        idat = png.index(b"IDAT")
        compressed_length = struct.unpack(">I", png[idat - 4 : idat])[0]
        scanline = zlib.decompress(png[idat + 4 : idat + 4 + compressed_length])
        self.assertEqual(scanline, b"\x00\xff\xff\xff\x00\x00\x00")

    def test_named_and_numeric_keycodes(self):
        self.assertEqual(mac_control.parse_key_code("command"), 0x37)
        self.assertEqual(mac_control.parse_key_code("0x24"), 0x24)
        with self.assertRaises(mac_control.ControlError):
            mac_control.parse_key_code("not-a-key")

    def test_screenshot_protocol_ignores_logs_and_checks_frame(self):
        palette = bytearray(512)
        palette[2:4] = (0xF800).to_bytes(2, "little")
        palette[4:6] = (0x07E0).to_bytes(2, "little")
        payload = bytes(palette) + zlib.compress(bytes((1, 2)))
        crc = zlib.crc32(payload) & 0xFFFFFFFF

        class FakeSerial:
            is_open = True
            timeout = 0.25

            def __init__(self):
                self.commands = []
                self.responses = []

            def queue_chunk(self, sequence):
                chunk = payload[sequence * 48 : (sequence + 1) * 48]
                crc = zlib.crc32(chunk) & 0xFFFFFFFF
                self.responses.append(
                    f"@B2 D 7 {sequence} {chunk.hex()} {crc:08X}\n".encode()
                )

            def write(self, data):
                self.commands.append(data)
                command = data.decode().strip()
                if command.startswith("@B2 SCREENSHOT ") and not any(
                    word in command for word in ("BATCH", "CHUNK", "CLOSE")
                ):
                    chunks = (len(payload) + 47) // 48
                    nonce = command.rsplit(" ", 1)[1]
                    self.responses.extend((
                        b"[VIDEO] unrelated log\n",
                        f"[FPU@B2 F2 {nonce} 7 2 1 {len(payload)} 2 {crc:08X} {chunks} zlib\n".encode(),
                    ))
                elif command.startswith("@B2 SCREENSHOT BATCH 7 "):
                    _, _, _, _, first, count = command.split()
                    for sequence in range(int(first), int(first) + int(count)):
                        self.queue_chunk(sequence)
                    if first == "0" and len(self.responses) >= 2:
                        # Hardware CDC can concatenate duplicate records when
                        # the intervening newline is lost. Keep the second
                        # marker so the parser can recover both fragments.
                        left = self.responses.pop(-2).rstrip(b"\n")
                        right = self.responses.pop(-1)
                        self.responses.append(left + right)
                    self.responses.append(
                        f"@B2 OK SCREENSHOT BATCH 7 {first} {count}\n".encode()
                    )
                elif command.startswith("@B2 SCREENSHOT CHUNK 7 "):
                    sequence = int(command.rsplit(" ", 1)[1])
                    self.queue_chunk(sequence)
                elif command == "@B2 SCREENSHOT CLOSE 7":
                    self.responses.append(b"@B2 OK SCREENSHOT CLOSE 7\n")

            def flush(self):
                pass

            def reset_input_buffer(self):
                self.responses.clear()

            def readline(self):
                return self.responses.pop(0) if self.responses else b""

        control = mac_control.MacControl.__new__(mac_control.MacControl)
        control.serial = FakeSerial()
        control._http_url = False
        png, width, height = control.screenshot_color_png(timeout=0.1)
        self.assertEqual((width, height), (2, 1))
        self.assertTrue(png.startswith(b"\x89PNG"))
        self.assertTrue(control.serial.commands[0].startswith(b"@B2 SCREENSHOT "))
        self.assertEqual(control.serial.commands[-1], b"@B2 SCREENSHOT CLOSE 7\n")


class McpServerTests(unittest.TestCase):
    def test_initialize_and_tool_listing(self):
        requests = io.BytesIO(
            b'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}\n'
            b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n'
            b'{"jsonrpc":"2.0","id":2,"method":"tools/list"}\n'
        )
        responses = io.BytesIO()
        mac_control.serve_mcp(object(), requests, responses)
        messages = [json.loads(line) for line in responses.getvalue().splitlines()]
        self.assertEqual(messages[0]["result"]["protocolVersion"], "2024-11-05")
        tool_names = {tool["name"] for tool in messages[1]["result"]["tools"]}
        self.assertIn("mac_screenshot", tool_names)
        self.assertIn("mac_screenshot_color", tool_names)
        self.assertIn("mac_type", tool_names)


class SerialTransportTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "posix", "POSIX modem control")
    def test_usb_control_lines_are_updated_atomically(self):
        import termios
        connection = mac_control._AtomicModemLines()
        connection.fd = 42
        for dtr, rts in ((False, False), (False, True), (True, True), (True, False)):
            connection._dtr_state, connection._rts_state = dtr, rts
            with patch("fcntl.ioctl") as ioctl:
                connection._update_dtr_state()
                expected = (termios.TIOCM_DTR if dtr else 0) | (termios.TIOCM_RTS if rts else 0)
                ioctl.assert_called_once_with(42, termios.TIOCMSET, struct.pack("I", expected))

    class Serial:
        timeout = 0.25

        def __init__(self, chunks=()):
            self.chunks = list(chunks)
            self.commands = []

        @property
        def in_waiting(self):
            return len(self.chunks[0]) if self.chunks else 0

        def read(self, size):
            return self.chunks.pop(0) if self.chunks else b""

        def write(self, data):
            self.commands.append(data)

        def flush(self):
            pass

    def control(self, chunks=()):
        control = mac_control.MacControl.__new__(mac_control.MacControl)
        control.serial = self.Serial(chunks)
        return control

    def test_partial_records_survive_read_timeouts_and_bulk_reads(self):
        c = self.control([b"noise @", b"", b"B2 OK PO", b"", b"NG 4\n@B2 OK INFO 4 Tab5 640 360\n"])
        lines = c._protocol_lines(time.monotonic() + 1)
        self.assertEqual(next(lines), "@B2 OK PONG 4")
        # Ending a generator early must not throw away its other buffered ACK.
        lines.close()
        self.assertEqual(next(c._protocol_lines(time.monotonic() + 1)), "@B2 OK INFO 4 Tab5 640 360")
        self.assertEqual(c.serial.timeout, 0.25)

    def test_tagged_request_ignores_delayed_ack_and_error(self):
        c = self.control([b"@B2 RES 00000001 OK MOUSE MOVE 1 1\n@B2 ERR stale\n",
                          b"@B2 RES 00000001 ERR stale\n@B2 RES 00000002 OK MOUSE MOVE 2 2\n"])
        c.protocol_version = 4
        c._request_id = 1
        self.assertEqual(c.request("MOUSE MOVE 2 2", "OK MOUSE MOVE"), "@B2 OK MOUSE MOVE 2 2")
        self.assertEqual(c.serial.commands, [b"@B2 REQ 00000002 00000000 MOUSE MOVE 2 2\n"])

    def test_action_timeout_does_not_send_a_new_request_id(self):
        c = self.control()
        c.protocol_version = 4
        with self.assertRaises(mac_control.ControlError):
            c.request("KEY TAP 36", "OK KEY TAP", timeout=0.001)
        self.assertEqual(len(c.serial.commands), 1)

    def test_lost_ack_retries_identical_envelope(self):
        c = self.control()
        c.protocol_version = 4
        c.session_id = "ABCDEF01"
        c._request_id = 0
        def write(data):
            c.serial.commands.append(data)
            if len(c.serial.commands) == 2:
                c.serial.chunks.append(b"@B2 RES 00000001 OK MOUSE CLICK 10 20 0\n")
        c.serial.write = write
        c.click(10, 20)
        self.assertEqual(c.serial.commands, [b"@B2 REQ 00000001 ABCDEF01 MOUSE CLICK 10 20 0\n"] * 2)
        self.assertEqual(c.last_request_retries, 1)

    def test_reboot_rejects_retry_from_previous_session(self):
        c = self.control([b"@B2 RES 00000001 ERR session_changed\n"])
        c.protocol_version = 4
        c.session_id = "ABCDEF01"
        c._request_id = 0
        with self.assertRaisesRegex(mac_control.ControlError, "session_changed"):
            c.click(10, 20)
        self.assertEqual(len(c.serial.commands), 1)

    def test_handshake_requires_a_valid_v4_boot_session(self):
        c = self.control()
        c._accept_handshake("@B2 OK PONG 3")
        self.assertEqual(c.protocol_version, 3)
        for invalid in ("@B2 OK PONG 4", "@B2 OK PONG x", "@B2 OK PONG 4 NOTHEX00"):
            with self.assertRaises(mac_control.ControlError):
                c._accept_handshake(invalid)
        c._accept_handshake("@B2 OK PONG 4 1234ABCD")
        self.assertEqual((c.protocol_version, c.session_id), (4, "1234ABCD"))

    def test_corrupt_or_stale_blocks_are_retried_without_restarting_frame(self):
        c = self.control()
        data = bytes(range(256)) * 3
        crc = zlib.crc32(data) & 0xFFFFFFFF
        good = f"@B2 R 7 768 {data.hex()} {crc:08X}\n".encode()
        def write(command):
            c.serial.commands.append(command)
            if len(c.serial.commands) == 1:
                c.serial.chunks.extend([good.replace(b"R 7 ", b"R 6 "), good[:-10] + b"00000000\n"])
            else:
                c.serial.chunks.extend([good[:80], b"", good[80:]])
        c.serial.write = write
        self.assertEqual(c._screenshot_read("7", 768, 768, time.monotonic() + 2), data)
        self.assertEqual(c.serial.commands, [b"@B2 SCREENSHOT READ 7 768 768\n"] * 2)

    def test_protocol_noise_is_bounded_and_recovers(self):
        c = self.control([b"x" * 20000, b"\n@B2 OK PONG 4\n"])
        self.assertEqual(next(c._protocol_lines(time.monotonic() + 1)), "@B2 OK PONG 4")
        self.assertLessEqual(len(c._rx_buffer), mac_control.MAX_PROTOCOL_LINE)

    def test_failed_v4_capture_aborts_even_when_header_is_lost(self):
        c = self.control()
        c.protocol_version = 4
        with patch.object(c, "_screenshot_png_transfer", side_effect=mac_control.ControlError("lost header")), \
             patch.object(c, "request", return_value="@B2 OK SCREENSHOT ABORT") as request:
            with self.assertRaisesRegex(mac_control.ControlError, "lost header"):
                c._screenshot_png_once(1)
            request.assert_called_once_with("SCREENSHOT ABORT", "OK SCREENSHOT ABORT", timeout=0.5)

    def test_v4_full_frame_uses_large_blocks_and_closes_lease(self):
        c = self.control()
        c.protocol_version = 4
        pixels = bytes(range(256)) * 4
        # Literal b2lz stream, deliberately crossing a READ boundary.
        payload = bytes(512) + b"".join(bytes([127]) + pixels[i:i+128] for i in range(0, 1024, 128))
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        def write(data):
            c.serial.commands.append(data)
            parts = data.decode().split()
            if parts[1] == "REQ":
                c.serial.chunks.append(f"@B2 RES {parts[2]} OK SCREENSHOT CLOSE 7\n".encode())
            elif parts[2] == "READ":
                offset, count = map(int, parts[4:6])
                block = payload[offset:offset+count]
                reply = f"@B2 R 7 {offset} {block.hex()} {zlib.crc32(block) & 0xFFFFFFFF:08X}\n".encode()
                c.serial.chunks.extend([reply[:31], b"", reply[31:700], reply[700:]])
            else:
                c.serial.chunks.append(f"@B2 F2 {parts[2]} 7 32 32 {len(payload)} 1024 {crc:08X} {(len(payload)+47)//48} L\n".encode())
        c.serial.write = write
        png, width, height = c.screenshot_color_png(timeout=2)
        self.assertEqual((width, height), (32, 32))
        self.assertTrue(png.startswith(b"\x89PNG"))
        self.assertEqual(sum(b"SCREENSHOT READ" in cmd for cmd in c.serial.commands), 3)
        self.assertIn(b"SCREENSHOT CLOSE 7", c.serial.commands[-1])
        self.assertEqual(c.last_screenshot_stats["block_retries"], 0)


if __name__ == "__main__":
    unittest.main()
