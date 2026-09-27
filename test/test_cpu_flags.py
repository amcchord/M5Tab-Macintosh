import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class CpuFlagTests(unittest.TestCase):
    def test_add_sub_cmp_flags_match_classic_formulas(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "cpu-flags"
            subprocess.run(["clang++", "-std=c++17", "-O2", "-fsanitize=undefined",
                            str(ROOT / "test/test_cpu_flags.cpp"), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
