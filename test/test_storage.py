"""Exercise the production guest disk backend and FatFs USB transfer path."""
from pathlib import Path
import json
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class StorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        flags = ["c++", "-std=c++17", "-g", "-fsanitize=address,undefined",
                 "-Itest/storage_stubs", "-Itest/aot_host/stubs", "-Isrc/basilisk/include",
                 "-Isrc/basilisk"]
        subprocess.run([*flags, "-DDISK_IMAGE_HOST_TEST", "src/basilisk/sys_esp32.cpp",
                        "test/test_disk_image.cpp", "-o", str(cls.work / "image")],
                       cwd=ROOT, check=True)
        subprocess.run([*flags, "test/test_usb_disk.cpp", "-o", str(cls.work / "usb")],
                       cwd=ROOT, check=True)

    def test_disk_image_metadata_survives_open(self):
        path = self.work / "fixture.dsk"
        for primary_attributes in (0, 0x100):
            image = bytearray(512 * 32)
            for offset, attributes in ((1024, primary_attributes), (len(image) - 1024, 0x100)):
                struct.pack_into(">H", image, offset, 0x4244)
                struct.pack_into(">H", image, offset + 10, attributes)
                struct.pack_into(">II", image, offset + 100, 16, 17)
            for mode in ("open", "readonly", "write"):
                with self.subTest(primary_attributes=primary_attributes, mode=mode):
                    path.write_bytes(image)
                    subprocess.run([self.work / "image", path, mode], check=True, capture_output=True)
                    expected = bytearray(image)
                    if mode == "write":
                        expected[2048:2050] = bytes.fromhex("dead")
                    self.assertEqual(path.read_bytes(), expected)

    def test_multisector_usb_transfers(self):
        subprocess.run([self.work / "usb"], check=True)

    def test_benchmark_detects_a_corrupt_readback_and_never_overwrites(self):
        reference, card_file = self.work / "reference", self.work / "card-file"
        reference.write_bytes(b"Macintosh" * 65536)
        benchmark = [sys.executable, str(ROOT / "tools/usb_disk_benchmark.py")]
        subprocess.run([*benchmark, "write", reference, card_file], check=True, capture_output=True)
        first_read = subprocess.run([*benchmark, "read", reference, card_file], check=True, capture_output=True)
        result = json.loads(first_read.stdout)
        self.assertEqual(result["reference_sha256"], result["read_sha256"])
        with card_file.open("r+b") as stream:
            stream.write(b"X")
        for action in ("write", "read"):
            self.assertNotEqual(subprocess.run([*benchmark, action, reference, card_file],
                                               capture_output=True).returncode, 0)
