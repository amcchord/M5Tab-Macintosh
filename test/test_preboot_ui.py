"""Render the preboot screens natively and check panel scaling.

Set PREBOOT_UI_PNG_DIR to also keep a PNG of every screen for review.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from mac_control import encode_png  # noqa: E402


def ppm_to_png(source: Path, target: Path) -> None:
    data = source.read_bytes()
    header, _, rest = data.partition(b"\n255\n")
    width, height = (int(v) for v in header.split()[1:3])
    target.write_bytes(encode_png(width, height, rest[: width * height * 3]))


class PrebootUITests(unittest.TestCase):
    def render(self, board):
        subprocess.run([sys.executable, str(ROOT / "scripts/build_assets.py")], check=True,
                       stdout=subprocess.DEVNULL)
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / "preboot-ui-test"
            subprocess.run([
                "clang++", "-std=c++17", "-O1", "-g", "-pthread",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                f"-D{board}=1", "-I", str(ROOT / "test/display_stubs"),
                "-I", str(ROOT / "src/board"), "-I", str(ROOT / "src/basilisk"),
                "-I", str(ROOT / "src/basilisk/include"),
                "-I", str(ROOT / "src/generated"),
                str(ROOT / "test/test_preboot_ui.cpp"),
                str(ROOT / "src/basilisk/preboot_ui.cpp"),
                str(ROOT / "src/basilisk/preboot_screens.cpp"),
                str(ROOT / "src/board/board_display_surface.cpp"),
                str(ROOT / "src/board/mini_gfx/mini_gfx.cpp"),
                "-o", str(binary)], check=True)
            env = dict(os.environ, PREBOOT_UI_OUT=folder)
            subprocess.run([str(binary)], check=True, timeout=60, env=env)
            pngs = os.environ.get("PREBOOT_UI_PNG_DIR")
            for ppm in sorted(Path(folder).glob("*.ppm")):
                if pngs:
                    out = Path(pngs) / board.lower()
                    out.mkdir(parents=True, exist_ok=True)
                    ppm_to_png(ppm, out / (ppm.stem + ".png"))
            return sorted(p.stem for p in Path(folder).glob("*.ppm"))

    def test_scale1_screens(self):
        self.assertIn("settings", self.render("BOARD_HOST_SCALE1"))

    def test_tab5_screens(self):
        names = self.render("BOARD_M5STACK_TAB5")
        self.assertIn("settings", names)
        self.assertIn("settings-firmware-staged", names)
        self.assertIn("keyboard-unflipped", names)

    def test_waveshare_screens(self):
        self.assertIn("usb", self.render("BOARD_WAVESHARE_P4_101"))


if __name__ == "__main__":
    unittest.main()
