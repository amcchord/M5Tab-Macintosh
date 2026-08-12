import importlib.util
import io
import json
from pathlib import Path
import struct
import unittest
import zlib


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


if __name__ == "__main__":
    unittest.main()
