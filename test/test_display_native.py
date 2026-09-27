"""Run production display code with injectable task/cache failures and sanitizers."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DisplayNativeTests(unittest.TestCase):
    def compile_run(self, source, board, extras=()):
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / "display-test"
            subprocess.run([
                "clang++", "-std=c++17", "-O1", "-g", "-pthread",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                f"-D{board}=1", "-I", str(ROOT / "test/display_stubs"),
                "-I", str(ROOT / "src/board"),
                "-I", str(ROOT / "src/basilisk"),
                "-I", str(ROOT / "src/basilisk/include"),
                "-I", str(ROOT / "src/generated"),
                str(ROOT / source), *[str(ROOT / f) for f in extras],
                "-o", str(binary)
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_tab5_display_pipeline(self):
        self.compile_run("test/test_display_pipeline.cpp", "BOARD_M5STACK_TAB5", (
            "src/board/board_display_surface.cpp", "src/board/mini_gfx/mini_gfx.cpp"))

    def test_waveshare_display_pipeline(self):
        self.compile_run("test/test_display_pipeline.cpp", "BOARD_WAVESHARE_P4_101", (
            "src/board/board_display_surface.cpp", "src/board/mini_gfx/mini_gfx.cpp"))

    def test_overlay_publication(self):
        subprocess.run(["python3", str(ROOT / "scripts/build_assets.py")], check=True)
        self.compile_run("test/test_overlay_pipeline.cpp", "BOARD_M5STACK_TAB5")

    def test_quickdraw_fallback_guards(self):
        self.compile_run("test/test_quickdraw_guards.cpp", "BOARD_M5STACK_TAB5")

    def test_hid_reports(self):
        self.compile_run("test/test_hid_reports.cpp", "BOARD_M5STACK_TAB5", ("src/basilisk/hid_descriptor.cpp",))

    def test_concurrent_input_claims(self):
        self.compile_run("test/test_input_claims.cpp", "BOARD_M5STACK_TAB5")
