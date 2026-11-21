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
                        "src/basilisk/hfs_boot.cpp",
                        "test/test_disk_image.cpp", "-o", str(cls.work / "image")],
                       cwd=ROOT, check=True)
        subprocess.run([*flags, "test/test_usb_disk.cpp", "-o", str(cls.work / "usb")],
                       cwd=ROOT, check=True)

    def test_usb_mode_exit_and_failed_start(self):
        includes = self.work / "usb-includes"
        includes.mkdir()
        for include in ("Arduino.h", "board_sd.h", "board_config.h", "sdkconfig.h",
                        "M5Unified.h", "USB.h", "USBMSC.h"):
            (includes / include).touch()
        for board in ("BOARD_M5STACK_TAB5", "BOARD_WAVESHARE_P4_101"):
            with self.subTest(board=board):
                binary = self.work / board
                subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined",
                                "-DSOC_USB_OTG_SUPPORTED=1", "-DCONFIG_TINYUSB_MSC_ENABLED=1",
                                "-D" + board, "-I" + str(includes), "-Itest/storage_stubs",
                                "-Isrc/basilisk/include", "test/test_usb_mode.cpp", "-o", str(binary)],
                               cwd=ROOT, check=True)
                subprocess.run([binary], check=True)

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

    def test_unclean_startup_handoff(self):
        # Flat classic HFS with a blessed System file, a 512-byte catalog
        # leaf and a two-extent System resource fork containing vers ID 1.
        disk = bytearray(64 * 512)
        disk[:2] = b'LK'
        disk[10:17] = b'\x06System'
        mdb = memoryview(disk)[1024:1536]
        mdb[:2] = b'BD'
        mdb[36:40] = b'\x03Mac'
        struct.pack_into('>H', mdb, 14, 3)
        struct.pack_into('>HI', mdb, 18, 52, 512)
        struct.pack_into('>H', mdb, 28, 4)
        struct.pack_into('>I', mdb, 92, 19)
        struct.pack_into('>IHH', mdb, 146, 1024, 0, 2)
        alternate = len(disk) - 1024
        disk[alternate:alternate + 512] = mdb
        struct.pack_into('>H', disk, alternate + 10, 0x100)
        catalog = 4 * 512
        disk[catalog + 8] = 1
        struct.pack_into('>I', disk, catalog + 24, 1)
        struct.pack_into('>H', disk, catalog + 32, 512)
        struct.pack_into('>I', disk, catalog + 36, 2)
        struct.pack_into('>H', disk, catalog + 510, 14)
        leaf = catalog + 512
        struct.pack_into('>BBH', disk, leaf + 8, 255, 1, 1)
        struct.pack_into('>HH', disk, leaf + 508, 130, 14)
        struct.pack_into('>BBIB6s', disk, leaf + 14, 12, 0, 19, 6, b'System')
        record = leaf + 28
        disk[record] = 2
        disk[record + 4:record + 12] = b'zsysMACS'
        struct.pack_into('>I', disk, record + 36, 700)
        struct.pack_into('>4H', disk, record + 86, 2, 1, 8, 1)
        resources = 6 * 512
        struct.pack_into('>4I', disk, resources, 256, 512, 14, 60)
        struct.pack_into('>IBBBB', disk, resources + 256, 10, 8, 0x10, 0x80, 0)
        resmap = 12 * 512
        struct.pack_into('>H', disk, resmap + 24, 28)
        struct.pack_into('>H4sHHH', disk, resmap + 28, 0, b'vers', 0, 10, 1)

        path = self.work / 'startup.dsk'
        cases = [
            ("8.1", [], 0x0900),
            ("7.6.1", [(resources + 260, b'\x07\x61')], 0x0900),
            ("7.5", [(resources + 260, b'\x07\x50')], 0),
            ("already clean", [(1034, b'\x01')], 0x0100),
            ("already inconsistent", [(1034, b'\x08')], 0x0800),
            ("locked", [(1034, b'\x80')], 0x8000),
            ("wrapper", [(1024 + 124, b'H+')], 0),
            ("mismatched alternate", [(alternate + 2, b'\xff')], 0),
            ("invalid extent", [(1024 + 150, b'\xff\xff')], 0),
            ("malformed node", [(leaf + 508, b'\xff\xff')], 0),
            ("unblessed", [(1024 + 92, bytes(4))], 0),
            ("wrong creator", [(record + 8, b'????')], 0),
            ("invalid map", [(resources + 4, b'\xff' * 4)], 0),
            ("invalid version ref", [(resmap + 36, b'\xff\xff')], 0),
        ]
        for label, edits, expected in cases:
            with self.subTest(label):
                image = bytearray(disk)
                for offset, data in edits:
                    image[offset:offset + len(data)] = data
                path.write_bytes(image)
                subprocess.run([self.work / "image", path, "open", "0", f"{expected:x}", "read"],
                               check=True, capture_output=True)
                self.assertEqual(path.read_bytes(), image)

        prefix = bytes(8 * 512)
        path.write_bytes(prefix + disk)
        for mode, expected, action in [("readonly", "0000", "read"), ("open", "0900", "guest_write")]:
            subprocess.run([self.work / "image", path, mode, str(len(prefix)), expected, action],
                           check=True, capture_output=True)
        self.assertEqual(path.read_bytes(), prefix + disk)
