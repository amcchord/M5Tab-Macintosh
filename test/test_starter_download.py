"""Exercise the real starter installer with a local card and a synthetic ZIP member."""
from pathlib import Path
import hashlib
import random
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
STUBS = ROOT / 'test/starter_stubs'
def sha(data):
    return hashlib.sha256(data).hexdigest()


class StarterDownloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        work = tempfile.TemporaryDirectory()
        cls.addClassCleanup(work.cleanup)
        cls.build = Path(work.name)
        includes = cls.build / 'includes'
        for name in ('Arduino.h', 'board_config.h', 'WiFi.h', 'esp_crt_bundle.h', 'esp_heap_caps.h',
                     'esp_http_client.h', 'esp_vfs_fat.h', 'mbedtls/sha256.h', 'miniz.h'):
            path = includes / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "platform.h"\n')
        cls.binary = cls.build / 'install'
        command = ['c++', '-std=c++17', '-g', '-fsanitize=address,undefined', '-Wno-deprecated-declarations',
                   '-DBOARD_SD_MOUNT_POINT="card"', '-I' + str(includes), '-I' + str(STUBS),
                   '-Isrc/basilisk/include', 'test/test_starter_download.cpp', '-lz']
        if sys.platform != 'darwin':
            command.append('-lcrypto')
        subprocess.run([*command, '-o', cls.binary], cwd=ROOT, check=True)
        # Multiple compressor chunks and output dictionary wraps. The repeated
        # tail creates real distance references back into the output history.
        block = random.Random(43).randbytes(100000)
        cls.data = block + block[-30000:] * 5
        compressor = zlib.compressobj(6, wbits=-15)
        cls.deflate = compressor.compress(cls.data) + compressor.flush()

    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.root = Path(work.name)
        self.card = self.root / 'card'
        self.card.mkdir()
        self.stage = self.card / '.m5-starter-v1'
        self.target = self.card / 'Test.dsk'
        self.packed = self.stage / 'Test.dsk.deflate'
        self.temp = self.stage / 'Test.dsk.new'

    def run_install(self, mode='ok', packed=None, packed_hash=None, final_hash=None, size=None):
        compressed = self.deflate if packed is None else packed
        source = self.root / 'member'
        source.write_bytes(compressed)
        return subprocess.run([self.binary, mode, source, str(len(self.data) if size is None else size),
                               packed_hash or sha(compressed), final_hash or sha(self.data)],
                              cwd=self.root, capture_output=True, text=True)

    def test_fragmented_inflater_writes_full_aligned_blocks_and_one_tail(self):
        block = 32768
        for size in (block * 3, block * 3 + 451, block - 1):
            with self.subTest(size=size):
                data = self.data[:size]
                compressor = zlib.compressobj(6, wbits=-15)
                packed = compressor.compress(data) + compressor.flush()
                result = self.run_install('fragmented', packed=packed, size=size, final_hash=sha(data))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                writes = [int(line.split()[1]) for line in result.stdout.splitlines()
                          if line.startswith('IMAGE_WRITE ')]
                expected = [block] * (size // block)
                if size % block:
                    expected.append(size % block)
                self.assertEqual(writes, expected)
                self.assertEqual(self.target.read_bytes(), data)
                self.target.unlink()

    def test_interrupted_and_cancelled_transfers_resume_at_exact_byte(self):
        for mode, code in [('interrupt', 1), ('cancel', 2)]:
            with self.subTest(mode=mode):
                result = self.run_install(mode)
                self.assertEqual(result.returncode, code, result.stderr)
                self.assertFalse(self.target.exists())
                prefix = self.packed.read_bytes()
                self.assertEqual(prefix, self.deflate[:4096])
                result = self.run_install()
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f'REQUEST {12345 + len(prefix)} ', result.stdout)
                self.assertEqual(self.target.read_bytes(), self.data)
                self.assertFalse(self.temp.exists())
                self.assertFalse(self.packed.exists())
                self.target.unlink()

    def test_requires_status_range_length_and_space_before_creating_files(self):
        for mode in ['http200', 'wrong_range', 'wrong_length', 'no_space']:
            with self.subTest(mode=mode):
                result = self.run_install(mode)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertFalse(self.packed.exists())
                self.assertFalse(self.target.exists())
                self.assertFalse(self.temp.exists())
                if mode == 'no_space':
                    self.assertNotIn('REQUEST', result.stdout)

    def test_invalid_data_never_published(self):
        result = self.run_install(packed_hash='0' * 64)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(self.packed.exists())
        self.assertFalse(self.target.exists())
        for packed, kwargs in [(b'bad-deflate', {}), (self.deflate + b'extra', {}),
                               (self.deflate, {'size': len(self.data) + 1}),
                               (self.deflate, {'final_hash': '0' * 64})]:
            with self.subTest(size=len(packed), kwargs=kwargs):
                self.packed.unlink(missing_ok=True)
                result = self.run_install(packed=packed, **kwargs)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertFalse(self.target.exists())
        self.assertEqual(self.run_install().returncode, 0)
        self.assertEqual(self.target.read_bytes(), self.data)

    def test_existing_images_and_foreign_staging_directory_survive(self):
        self.stage.mkdir()
        note = self.stage / 'existing.txt'
        note.write_text('user file')
        result = self.run_install()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(note.read_text(), 'user file')
        note.unlink()
        self.target.write_bytes(b'Existing disk')
        result = self.run_install()
        self.assertEqual(result.returncode, 1)
        self.assertNotIn('REQUEST', result.stdout)
        self.assertEqual(self.target.read_bytes(), b'Existing disk')
        self.target.write_bytes(self.data)
        self.packed.write_bytes(b'previous download')
        self.temp.write_bytes(b'previous attempt')
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('REQUEST', result.stdout)
        self.assertFalse(self.temp.exists())
        self.assertFalse(self.packed.exists())
