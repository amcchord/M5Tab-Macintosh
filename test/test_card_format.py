"""Run the production card identity, partition inspection and confirmation paths offline."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class CardFormatTests(unittest.TestCase):
    def test_format_requires_fresh_matching_media(self):
        self.run_fixture('test/test_card_format.cpp')

    def test_preserve_native_sd(self):
        self.run_fixture('test/test_native_sd.cpp', '-DTAB5_SD_NATIVE=1')

    def run_fixture(self, source, *defines):
        with tempfile.TemporaryDirectory() as d:
            inc = Path(d)
            for name in ('Arduino.h', 'SD.h', 'sdmmc_cmd.h', 'driver/sdmmc_host.h',
                         'driver/sdspi_host.h', 'driver/spi_master.h', 'esp_random.h',
                         'esp_heap_caps.h', 'esp_rom_crc.h', 'ff.h', 'diskio.h', 'diskio_impl.h',
                         'diskio_sdmmc.h', 'SPI.h', 'sd_diskio.h'):
                h = inc / name
                h.parent.mkdir(exist_ok=True, parents=True)
                h.write_text('#include "platform.h"\n')
            binary = inc / 'test'
            subprocess.run(['c++', '-std=c++17', '-fsanitize=address,undefined',
                            '-DBOARD_M5STACK_TAB5', *defines, '-I' + str(inc),
                            '-Itest/card_format_stubs', '-Isrc/board', source,
                            '-o', binary], cwd=ROOT, check=True)
            subprocess.run([binary], check=True)
