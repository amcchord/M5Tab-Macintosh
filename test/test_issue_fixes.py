"""Host regressions for clock writes, guest audio ranges and release packaging."""
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load_script(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


release = load_script("release_images")
patch = load_script("patch_m5gfx_dsi")


class FirmwareLogicTests(unittest.TestCase):
    def test_clock_and_guest_memory_boundaries(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler, "A host C++ compiler is required")
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "firmware-tests"
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined,address", "-fno-omit-frame-pointer",
                            "-I", str(ROOT / "src/basilisk/include"),
                            str(ROOT / "test/test_issue_fixes.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


class DSIPatchTests(unittest.TestCase):
    original = ("    esp_lcd_dsi_bus_config_t bus_config;\n"
                "    bus_config.phy_clk_src = static_cast<typeof(bus_config.phy_clk_src)>"
                "(MIPI_DSI_PHY_CLK_SRC_DEFAULT);\n")

    def test_default_clock_is_zero_initialized(self):
        result = patch.patch_source(self.original)
        self.assertIn("bus_config = {};", result)
        self.assertNotIn("phy_clk_src", result)

    def test_patch_is_idempotent(self):
        result = patch.patch_source(self.original)
        self.assertEqual(result, patch.patch_source(result))

    def test_unexpected_library_fails_closed(self):
        with self.assertRaises(RuntimeError):
            patch.patch_source("something else")


class ClockBackendTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "clock-backend"
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined,address", "-fno-omit-frame-pointer",
                        "-I", str(ROOT / "test/clock_stubs"),
                        "-I", str(ROOT / "src/basilisk/include"),
                        str(ROOT / "src/basilisk/mac_clock_esp32.cpp"),
                        str(ROOT / "test/test_clock_backend.cpp"),
                        "-o", str(cls.binary)], check=True)

    def test_empty_nvs_does_not_write(self):
        subprocess.run([str(self.binary), "empty"], check=True)

    def test_saved_historical_date_loads(self):
        subprocess.run([str(self.binary), "saved"], check=True)

    def test_save_checkpoint_and_failed_commit_retry(self):
        subprocess.run([str(self.binary), "writes"], check=True)


class ReleaseImagesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.merged = self.root / "merged.bin"
        data = bytearray(b"\xff" * 0x10100)
        for name, offset in (("bootloader.bin", 0x2000), ("partitions.bin", 0x8000),
                             ("firmware.bin", 0x10000)):
            component = bytearray(64)
            if name == "partitions.bin":
                component[:2] = b"\xaa\x50"
            else:
                component[0] = 0xE9
                struct.pack_into("<H", component, 12, 18)
            (self.root / name).write_bytes(component)
            data[offset:offset + len(component)] = component
        self.merged.write_bytes(data)

    def test_all_four_download_names_are_distinct(self):
        self.assertEqual(len(release.TARGETS), 4)
        self.assertEqual(len(set(release.TARGETS.values())), 4)
        for environment, name in release.TARGETS.items():
            self.assertEqual(environment.endswith("_rev3"), name.endswith("-Rev3"))

    def test_valid_components_at_flash_offsets(self):
        release.validate_merged(self.merged, self.root)

    def test_truncated_image_is_rejected(self):
        self.merged.write_bytes(self.merged.read_bytes()[:0x10020])
        with self.assertRaises(ValueError):
            release.validate_merged(self.merged, self.root)

    def test_stale_application_is_rejected(self):
        data = bytearray(self.merged.read_bytes())
        data[0x10030] ^= 1
        self.merged.write_bytes(data)
        with self.assertRaises(ValueError):
            release.validate_merged(self.merged, self.root)

    def test_wrong_chip_is_rejected(self):
        component = bytearray((self.root / "firmware.bin").read_bytes())
        component[12] = 9
        (self.root / "firmware.bin").write_bytes(component)
        data = bytearray(self.merged.read_bytes())
        data[0x10000:0x10000 + len(component)] = component
        self.merged.write_bytes(data)
        with self.assertRaises(ValueError):
            release.validate_merged(self.merged, self.root)


if __name__ == "__main__":
    unittest.main()
