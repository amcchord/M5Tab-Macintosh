import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


TOOLS = Path(__file__).parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location("speedometer_benchmark", TOOLS / "speedometer_benchmark.py")
speedometer_benchmark = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
# Its dataclasses resolve annotations through sys.modules.
sys.modules[SPEC.name] = speedometer_benchmark
SPEC.loader.exec_module(speedometer_benchmark)
from mac_control import encode_png  # noqa: E402


class SplashFallbackTests(unittest.TestCase):
    def test_help_only_menu_bar_is_a_splash(self):
        # 1-bit splash: the disabled titles dither and only Help reads.
        self.assertTrue(speedometer_benchmark.unreadable_menus_splash("Help\n2:18 AM"))

    def test_enabled_app_menus_without_file_are_not_a_splash(self):
        # OCR of an open result window that dropped "File" (2026-09-27 capture).
        text = "Edit\nTests\nAnalysis\nUtilities\nwindows\nHelp\nPerformance Test\nMath\n9.532"
        self.assertFalse(speedometer_benchmark.unreadable_menus_splash(text))

    def test_finder_menus_are_not_a_splash(self):
        self.assertFalse(speedometer_benchmark.unreadable_menus_splash("File\nEdit View Special\nHelp"))


class IconLocatorTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.image = Path(directory.name) / "desktop.png"
        self.image.write_bytes(encode_png(640, 360, bytes(640 * 360 * 3)))

    def test_icon_sits_above_its_label(self):
        # Vision record for the alias label on the benchmark desktop.
        records = [
            {"text": "Browse the Internef", "x": 0.846875, "y": 0.6222, "width": 0.15, "height": 0.0333},
            {"text": "Speedometer 4.02 sli.", "x": 0.828125, "y": 0.288889, "width": 0.16875, "height": 0.033333},
        ]
        self.assertEqual(speedometer_benchmark.speedometer_icon_position(self.image, records), (584, 224))

    def test_missing_label_returns_none(self):
        records = [{"text": "MicroMac", "x": 0.88, "y": 0.8, "width": 0.08, "height": 0.03}]
        self.assertIsNone(speedometer_benchmark.speedometer_icon_position(self.image, records))


if __name__ == "__main__":
    unittest.main()
