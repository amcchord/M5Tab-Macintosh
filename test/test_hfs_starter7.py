"""Check the five-byte 7.5.3 repair, including power-loss recovery and refusal paths."""
import hashlib
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SIZE = 1048576000
PREFIX = 19 * 512 + 2 * 1048576
PATCHES = [(1137333, 0x60, 0x61), (1057, 0x61, 0x62), (1037, 3, 4),
           (1111, 0xfc, 0xfd), (1034, 0, 1)]
SOURCE = os.environ.get('M5TAB_STARTER7_DISK')


class HfsStarter7Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(build.cleanup)
        cls.binary = Path(build.name) / 'starter7'
        cmd = ['c++', '-std=c++17', '-g', '-fsanitize=address,undefined', '-DDISK_IMAGE_HOST_TEST',
               '-Wno-deprecated-declarations', '-Itest/storage_stubs', '-Itest/aot_host/stubs',
               '-Isrc/basilisk/include', '-Itest/common', 'src/basilisk/sys_esp32.cpp',
               'src/basilisk/hfs_boot.cpp', 'test/test_hfs_starter7.cpp', '-o', str(cls.binary)]
        if sys.platform != 'darwin':
            cmd.append('-lcrypto')
        subprocess.run(cmd, cwd=ROOT, check=True)
        generator = random.Random(753)
        cls.prefix = bytearray(generator.randbytes(PREFIX))
        cls.prefix[1024:1030] = bytes.fromhex('4244e586d0ff')
        cls.prefix[1034:1036] = bytes(2)
        cls.prefix[1060:1070] = b'\x09MicroMac7'
        for offset, before, after in PATCHES:
            cls.prefix[offset] = before
        cls.tail = generator.randbytes(1024)
        cls.digest = hashlib.sha256(cls.prefix + cls.tail).hexdigest()

    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.path = Path(work.name) / 'renamed.dsk'

    def seed(self, prefix, tail):
        # Sparse data area. The metadata audit has a fixed read budget.
        with self.path.open('wb') as disk:
            disk.write(prefix)
            disk.seek(SIZE - 1024)
            disk.write(tail)

    def metadata(self):
        with self.path.open('rb') as disk:
            prefix = disk.read(PREFIX)
            disk.seek(SIZE - 1024)
            return prefix, disk.read()

    def invoke(self, mode, code=0, *args):
        result = subprocess.run([self.binary, self.path, mode, *args], capture_output=True, text=True)
        self.assertEqual(result.returncode, code, result.stderr)
        return result.stderr

    def test_partial_repairs_resume_and_clean_flag_is_last(self):
        self.seed(self.prefix, self.tail)
        expected = bytearray(self.prefix)
        for index, (offset, before, after) in enumerate(PATCHES):
            with self.subTest(patch=offset):
                # Each invocation gets exactly one successful byte write before
                # the sync reports failure. Already-correct bytes are skipped.
                self.invoke(self.digest, 2, '1')
                expected[offset] = after
                self.assertEqual(self.metadata(), (expected, self.tail))
                self.assertEqual(expected[1034], int(index == len(PATCHES) - 1))
        self.invoke(self.digest, 1)
        self.assertEqual(self.metadata(), (expected, self.tail))
        self.seed(self.prefix, self.tail)
        self.invoke(self.digest)
        self.assertEqual(self.metadata(), (expected, self.tail))

    def test_unrecognized_metadata_and_attributes_are_preserved(self):
        for offset in (0, 1028, 1035, 1057, 1536, 19000, 1137333, PREFIX - 1, SIZE - 1024, SIZE - 1):
            with self.subTest(offset=offset):
                prefix, tail = bytearray(self.prefix), bytearray(self.tail)
                if offset < PREFIX:
                    prefix[offset] ^= 0x80
                else:
                    tail[offset - (SIZE - 1024)] ^= 0x80
                self.seed(prefix, tail)
                self.invoke(self.digest, 1)
                self.assertEqual(self.metadata(), (prefix, tail))
        self.seed(self.prefix, self.tail)
        self.invoke('startup')  # Production digest rejects the synthetic fixture.
        self.assertEqual(self.metadata(), (self.prefix, self.tail))

    @unittest.skipUnless(SOURCE, 'set M5TAB_STARTER7_DISK to the extracted official starter')
    def test_published_starter_at_normal_open_and_boot_entry_points(self):
        with Path(SOURCE).open('rb') as source:
            prefix = source.read(PREFIX)
            source.seek(SIZE - 1024)
            tail = source.read()
        self.assertEqual(hashlib.sha256(prefix + tail).hexdigest(),
                         '46402dbdbd6ddeedabacc30434f57d45c525d55387cbe3c42f6394ece4b53a5b')
        self.seed(prefix, tail)
        for mode in ('open', 'readonly', 'cdrom'):
            self.invoke(mode)
            self.assertEqual(self.metadata(), (prefix, tail))
        self.assertIn('Repaired verified System 7.5.3 starter', self.invoke('startup'))
        expected = bytearray(prefix)
        for offset, before, after in PATCHES:
            self.assertEqual(expected[offset], before)
            expected[offset] = after
        self.assertEqual(self.metadata(), (expected, tail))
        self.assertNotIn('Repaired', self.invoke('startup'))
        self.assertEqual(self.metadata(), (expected, tail))
