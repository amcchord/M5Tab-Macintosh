import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path


CHECK = Path(__file__).resolve().parents[1] / "tools" / "check_startup_disk.py"


def volume(flags):
    image = bytearray(16 * 512)
    header = memoryview(image)[1024:1536]
    header[:2] = b"BD"
    struct.pack_into(">H", header, 10, flags)
    struct.pack_into(">HI", header, 18, 10, 512)
    struct.pack_into(">H", header, 28, 4)
    # A stale alternate header must never clear a warning from the primary.
    alternate = len(image) - 1024
    image[alternate:alternate + 512] = header
    struct.pack_into(">H", image, alternate + 10, 0x0100)
    return image


class StartupDiskTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def run_check(self, *paths):
        return subprocess.run([sys.executable, CHECK, *paths], capture_output=True, text=True)

    def test_primary_flags_and_preservation(self):
        path = self.root / "boot.dsk"
        for flags, code, phrase in [(0x0100, 0, "marked cleanly unmounted"),
                                    (0, 1, "missing clean-shutdown flag"),
                                    (0x0900, 1, "marked inconsistent")]:
            with self.subTest(flags=flags):
                original = volume(flags)
                path.write_bytes(original)
                result = self.run_check(path)
                self.assertEqual(result.returncode, code)
                self.assertIn(phrase, result.stdout)
                self.assertEqual(path.read_bytes(), original)

    def test_partition_location_and_bounds(self):
        path = self.root / "partitioned.hfv"
        disk = bytearray(4 * 512) + volume(0x0100)
        for index, kind in [(1, b"Apple_partition_map"), (2, b"Apple_HFS")]:
            offset = index * 512
            disk[offset:offset + 2] = b"PM"
            struct.pack_into(">III", disk, offset + 4, 2, 4, 16)
            disk[offset + 48:offset + 48 + len(kind)] = kind
        path.write_bytes(disk)
        self.assertEqual(self.run_check(path).returncode, 0)
        struct.pack_into(">I", disk, 2 * 512 + 12, 17)
        path.write_bytes(disk)
        self.assertEqual(self.run_check(path).returncode, 2)

    def test_archive_reports_all_disks_and_ignores_finder_metadata(self):
        path = self.root / "kit.zip"
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("sdCard/clean.DSK", volume(0x0100))
            archive.writestr("sdCard/dirty.img", volume(0))
            for name in ["__MACOSX/boot.dsk", "sdCard/._boot.dsk", "sdCard/Q650.ROM"]:
                archive.writestr(name, b"unused")
        original = path.read_bytes()
        result = self.run_check(path)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(len(result.stdout.splitlines()), 2)
        self.assertEqual(path.read_bytes(), original)

    def test_uncheckable_image_takes_precedence(self):
        path = self.root / "suspect.dsk"
        dirty = self.root / "dirty.dsk"
        dirty.write_bytes(volume(0))
        wrapped = volume(0x0100)
        wrapped[1024 + 124:1024 + 126] = b"H+"
        for original in [wrapped, b"", bytes(8192), volume(0x0100)[:4096]]:
            with self.subTest(size=len(original)):
                path.write_bytes(original)
                self.assertEqual(self.run_check(path, dirty).returncode, 2)
        path = self.root / "empty.zip"
        with zipfile.ZipFile(path, "w"):
            pass
        self.assertEqual(self.run_check(path).returncode, 2)
