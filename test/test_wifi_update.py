import hashlib
import http.client
import struct
import subprocess
from types import SimpleNamespace
from unittest.mock import Mock
import sys
import tempfile
import unittest
from pathlib import Path
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import wifi_update


class Board:
    def __init__(self, outcome="ready"):
        self.outcome = outcome
        self.received = None
        self.reboot = False

    def info(self):
        return "@B2 OK INFO 5 M5Stack Tab5 640 360"

    def boot_status(self):
        return {"screen": "settings"}

    def request(self, cmd, expected, timeout=None):
        if cmd == "BOOT UPDATE STATUS":
            state = self.outcome if self.received else "idle"
            slot = "ota_1" if state == "ready" else "ota_0"
            size = len(self.received or b'')
            return (f"@B2 OK BOOT UPDATE STATUS state={state} layout=ab running=ota_0 boot={slot} "
                    f"bytes={size} verified={size} size={size} result=digest")
        if cmd == "BOOT UPDATE REBOOT":
            self.reboot = True
            return "@B2 OK BOOT UPDATE REBOOT"
        head, count, digest, url = cmd.rsplit(" ", 3)
        assert head == "BOOT UPDATE START"
        target = urlsplit(url)
        conn = http.client.HTTPConnection(target.hostname, target.port, timeout=5)
        conn.request("GET", "/")
        assert conn.getresponse().status == 404
        conn.close()
        conn.request("GET", target.path)
        reply = conn.getresponse()
        assert reply.status == 200 and reply.getheader("Content-Length") == count
        self.received = reply.read()
        conn.close()
        assert hashlib.sha256(self.received).hexdigest() == digest
        return "@B2 OK BOOT UPDATE START"


class WifiUpdateTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.path = Path(tmp.name) / "firmware.bin"
        self.image = bytearray(20000)
        self.image[0] = 0xe9
        tag = wifi_update.BOARD_TAGS["M5Stack Tab5"]
        self.image[288:288 + len(tag)] = tag
        struct.pack_into("<HH", self.image, 328, 1, 1)
        self.path.write_bytes(self.image)

    def test_serve_exact_pinned_application_and_reboot(self):
        board = Board()
        self.assertEqual(wifi_update.install(board, self.path, "127.0.0.1", True), "ota_1")
        self.assertEqual(board.received, self.image)
        self.assertTrue(board.reboot)

    def test_slow_client_gets_a_new_write_deadline_per_chunk(self):
        image = bytes(range(251)) * 16551
        sent = bytearray()
        timeout = 15
        # A 100 KiB/s link cannot transmit this image in the socket's 15 s
        # sendall deadline, but can complete each 32 KiB write in time.
        def send(part):
            if len(part) / 102400 > timeout:
                raise TimeoutError
            self.assertLessEqual(len(part), 65536)
            sent.extend(part)
        handler = SimpleNamespace(path="/firmware.bin", image=image, connection=Mock(),
                                  wfile=SimpleNamespace(write=send), send_response=Mock(),
                                  send_header=Mock(), end_headers=Mock())
        wifi_update.ImageHandler.do_GET(handler)
        handler.connection.settimeout.assert_called_once_with(timeout)
        self.assertEqual(image, sent)

    def test_firmware_streams_partial_reads_and_preserves_active_slot_on_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            includes = Path(temp) / "includes"
            for name in ("Arduino.h", "board_config.h", "WiFi.h", "sdkconfig.h",
                         "esp_app_desc.h", "esp_app_format.h", "esp_chip_info.h", "esp_http_client.h",
                         "esp_ota_ops.h", "mbedtls/sha256.h", "lwip/inet.h", "lwip/sockets.h"):
                header = includes / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.touch()
            (includes / "esp_idf_version.h").write_text(
                "#define ESP_IDF_VERSION_VAL(a,b,c) (((a)<<16)|((b)<<8)|(c))\n"
                "#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5,5,TEST_IDF_PATCH)\n")
            binary = Path(temp) / "ota"
            command = ["c++", "-std=c++17", "-g", "-fsanitize=address,undefined",
                       "-Wno-deprecated-declarations", "-DBOARD_M5STACK_TAB5=1", "-I" + str(includes),
                       "-Isrc/basilisk/include", "test/test_wifi_update_receiver.cpp", "-o", str(binary)]
            if sys.platform != "darwin":
                command.append("-lcrypto")
            for patch in (1, 4):
                with self.subTest(idf_patch=patch):
                    subprocess.run([*command, f"-DTEST_IDF_PATCH={patch}"], cwd=ROOT, check=True)
                    subprocess.run([binary], check=True, timeout=10)

    def test_bad_result_never_reboots(self):
        board = Board("failed")
        with self.assertRaisesRegex(wifi_update.ControlError, "digest"):
            wifi_update.install(board, self.path, "127.0.0.1", True)
        self.assertFalse(board.reboot)

    def test_incompatible_or_unmarked_binary_is_rejected_locally(self):
        for offset in [0, 288, 330]:
            data = self.image.copy()
            data[offset] ^= 1
            self.path.write_bytes(data)
            board = Board()
            with self.assertRaises(ValueError):
                wifi_update.install(board, self.path, "127.0.0.1", False)
            self.assertIsNone(board.received)
