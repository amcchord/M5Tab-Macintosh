"""Ahead-of-time translated 68k code against the interpreter.

The repository cannot carry a Macintosh ROM, so the translator is tested on
generated code: tools/aot/make_test_rom.py writes instances of every opcode
handler the translator accepts plus hand-assembled loops, branches and calls,
as a ROM and as code loaded into RAM. The differential test (test/aot_host)
then runs every region entry from random states and compares it with the
interpreter built from the same sources.
"""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools/aot"
sys.path.insert(0, str(TOOLS))
from aot_profile import read_ram


class AotProfileTests(unittest.TestCase):
    def test_read_code_at_end_of_guest_ram(self):
        class LastPage:
            def request(self, command, expected, timeout):
                return {
                    "AOTPROF RAMMAP 0": "OK AOTPROF RAMMAP 0 1 01",
                    "AOTPROF RAMCHUNKS 0": "OK AOTPROF RAMCHUNKS 0 80000000",
                    "AOTPROF RAMREAD 0 31": "OK AOTPROF RAMREAD 0 31 " + "00000000" * 63 + "00000002",
                    "PEEK F80 128": "OK PEEK 00000F80 " + "45" * 128,
                }[command]

        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "ram.txt"
            read_ram(LastPage(), path)
            self.assertEqual(path.read_text().splitlines()[1:], ["C ffe 2", "B f80 " + "45" * 128])


class AotTranslationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.work = Path(cls.directory.name)
        subprocess.run(["c++", "-w", "-I", str(ROOT / "src/basilisk/uae_cpu"), "-I", str(ROOT / "tools/cpu_gen"),
                        "-o", str(cls.work / "dump_optable"), str(TOOLS / "dump_optable.cpp"),
                        str(ROOT / "src/basilisk/uae_cpu/readcpu.cpp"),
                        str(ROOT / "src/basilisk/uae_cpu/generated/cpudefs.cpp")], check=True)
        with open(cls.work / "optable.tsv", "w") as table:
            subprocess.run([str(cls.work / "dump_optable")], stdout=table, check=True)
        subprocess.run(["python3", str(TOOLS / "make_test_rom.py"), "--optable", str(cls.work / "optable.tsv"),
                        "--rom", str(cls.work / "test.rom"), "--hot", str(cls.work / "hot.txt"),
                        "--ram-profile", str(cls.work / "ram.txt"), "--per-handler", "2"],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def translate(self, *options):
        return subprocess.run(
            ["python3", str(TOOLS / "aot_translate.py"), "--rom", str(self.work / "test.rom"),
             "--optable", str(self.work / "optable.tsv"), "--hot", str(self.work / "hot.txt"),
             "--out", str(self.work / "aot_rom_generated.cpp"), "--min-region-insns", "1",
             "--max-rom-entries", "1500", *options],
            check=True, capture_output=True, text=True).stdout

    def compare_with_interpreter(self, *options):
        report = self.translate("--ram-profile", str(self.work / "ram.txt"), *options)
        self.assertIn('"rom_translated_share": 1.0', report)
        self.assertIn('"ram_translated_share": 1.0', report)
        subprocess.run(["bash", str(ROOT / "test/aot_host/build.sh"), str(self.work / "build"),
                        str(self.work / "aot_rom_generated.cpp")], check=True, capture_output=True)
        result = subprocess.run([str(self.work / "build/aot_diff"), str(self.work / "test.rom"), "--trials", "6"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout[-2000:] + result.stderr[-4000:])
        self.assertRegex(result.stdout, r"PASS: \d{4,} runs")
        return report

    def test_translated_code_matches_the_interpreter(self):
        # Guest registers in the interpreter's register array.
        report = self.compare_with_interpreter("--cache-run", "1000000")
        self.assertIn('"rom_regions_with_cached_registers": 0', report)

    def test_translated_code_with_cached_registers_matches_the_interpreter(self):
        # Guest registers in locals, in every region.
        report = self.compare_with_interpreter("--cache-run", "0")
        self.assertNotIn('"rom_regions_with_cached_registers": 0', report)

    def test_hot_untranslated_report_respects_entry_budget(self):
        report = json.loads(self.translate("--ram-profile", str(self.work / "ram.txt"),
                                           "--max-rom-entries", "0", "--max-ram-entries", "0"))
        for space in ("rom", "ram"):
            self.assertEqual(report[f"{space}_installed_share"], 0.0)
            self.assertEqual(report[f"{space}_translated_share"], 1.0)
            hot = report[f"{space}_hot_untranslated"]
            self.assertEqual(len(hot), 20)
            self.assertEqual(hot, sorted(hot, key=lambda row: (-row["executions"], row["guest_pc"])))
            self.assertEqual(hot[0]["reason"], "no emitted region entry or module")

    def test_entry_points_are_only_placed_on_executed_instruction_starts(self):
        # A profile entry inside another instruction (the second word of the
        # first snippet's DBF) must not become an entry point: patching it
        # would change the instruction that contains it.
        hot = (self.work / "hot.txt").read_text()
        first = min(int(line.split()[0], 16) for line in hot.splitlines() if not line.startswith("#"))
        inside = first + 4   # MOVE.B (A0)+,(A1)+ ; DBF D0 -> the DBF's displacement word
        (self.work / "hot-inside.txt").write_text(hot + f"{inside:x} 100000\n")
        subprocess.run(
            ["python3", str(TOOLS / "aot_translate.py"), "--rom", str(self.work / "test.rom"),
             "--optable", str(self.work / "optable.tsv"), "--hot", str(self.work / "hot-inside.txt"),
             "--out", str(self.work / "inside.cpp"), "--min-region-insns", "1"],
            check=True, capture_output=True, text=True)
        generated = (self.work / "inside.cpp").read_text()
        entries = generated[generated.index("static const AotEntry aot_entries[]"):]
        entries = entries[:entries.index("};")]
        self.assertNotIn(f"{{ 0x{inside:x}u,", entries)
        self.assertIn(f"{{ 0x{first:x}u,", entries)


if __name__ == "__main__":
    unittest.main()
