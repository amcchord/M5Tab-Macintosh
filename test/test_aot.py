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


class AotOptimizationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import os
        import subprocess
        import tempfile
        from pathlib import Path

        cls.repo = Path(__file__).resolve().parent.parent
        cls.tmp = tempfile.TemporaryDirectory(prefix="test_aot_opt_")
        cls.work = Path(cls.tmp.name)
        cls.host_build = cls.work / "host_build"
        cls.host_build.mkdir()

        # Build optable.tsv once from dump_optable.
        dump_bin = cls.work / "dump_optable"
        subprocess.run(
            [
                "c++", "-std=gnu++17", "-O1", "-w",
                "-DAOT_HOST_TEST=1",
                "-DSIZEOF_SHORT=2", "-DSIZEOF_INT=4", "-DSIZEOF_LONG=8",
                "-DSIZEOF_LONG_LONG=8", "-DSIZEOF_VOID_P=8",
                "-I", str(cls.repo / "test" / "aot_host" / "stubs"),
                "-I", str(cls.repo / "tools" / "aot"),
                "-I", str(cls.repo / "src" / "basilisk"),
                "-I", str(cls.repo / "src" / "basilisk" / "include"),
                "-I", str(cls.repo / "src" / "basilisk" / "uae_cpu"),
                "-I", str(cls.repo / "src" / "basilisk" / "uae_cpu" / "generated"),
                str(cls.repo / "tools" / "aot" / "dump_optable.cpp"),
                str(cls.repo / "src" / "basilisk" / "uae_cpu" / "readcpu.cpp"),
                str(cls.repo / "src" / "basilisk" / "uae_cpu" / "generated" / "cpudefs.cpp"),
                "-o", str(dump_bin),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        cls.optable = cls.work / "optable.tsv"
        cls.optable.write_text(
            subprocess.run([str(dump_bin)], check=True, capture_output=True, text=True).stdout
        )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def _build_synthetic_rom(self, streams, size=65536):
        import struct

        rom = bytearray(size)
        for off in range(0, size, 2):
            struct.pack_into(">H", rom, off, 0x4E75)  # RTS fill
        struct.pack_into(">II", rom, 0, 0x00004000, 0x40800400)
        hot = []
        for start_off, items in streams:
            off = start_off
            for word_seq, count in items:
                for wi, w in enumerate(word_seq):
                    struct.pack_into(">H", rom, off + wi * 2, w & 0xFFFF)
                if count > 0:
                    hot.append(f"{off:x} {count}")
                off += len(word_seq) * 2
        return bytes(rom), "\n".join(hot) + "\n"

    def _translate_and_diff(self, name, rom_bytes, hot_text, extra_args=(), trials=4):
        import json
        import os
        import subprocess
        import sys

        sub = self.work / name
        sub.mkdir(exist_ok=True)
        rom_path = sub / "test.rom"
        hot_path = sub / "hot.txt"
        out_cpp = self.work / "aot_gen.cpp"
        report_path = sub / "report.json"
        rom_path.write_bytes(rom_bytes)
        hot_path.write_text(hot_text)

        cmd = [
            sys.executable,
            str(self.repo / "tools" / "aot" / "aot_translate.py"),
            "--rom", str(rom_path),
            "--optable", str(self.optable),
            "--hot", str(hot_path),
            "--min-count", "10",
            "--min-region-insns", "1",
            "--max-rom-entries", "1500",
            "--out", str(out_cpp),
            "--report", str(report_path),
            *extra_args,
        ]
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        generated = out_cpp.read_text()
        report = json.loads(report_path.read_text())

        subprocess.run(
            ["bash", str(self.repo / "test" / "aot_host" / "build.sh"), str(self.host_build), str(out_cpp)],
            check=True,
            capture_output=True,
            text=True,
        )
        diff = subprocess.run(
            [str(self.host_build / "aot_diff"), str(rom_path), "--trials", str(trials)],
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertIn("PASS:", diff.stdout)
        self.assertIn("0 failures", diff.stdout)
        return generated, report, diff.stdout

    def test_parse_cpuemu_multiblock_rol_and_register_use(self):
        import sys

        sys.path.insert(0, str(self.repo / "tools" / "aot"))
        import aot_translate

        bodies = aot_translate.parse_cpuemu(
            self.repo / "src" / "basilisk" / "uae_cpu" / "generated" / "cpuemu.cpp"
        )
        for sym in ("op_e118_0", "op_e158_0", "op_e198_0", "op_4e73_0"):
            self.assertIn(sym, bodies)
            _, body = bodies[sym]
            self.assertIn("cpuop_end();", body)
            self.assertEqual(body.count("{"), body.count("}"), f"unbalanced braces in {sym}")
        self.assertIn("SET_NFLG", bodies["op_e198_0"][1])

        reads, writes = aot_translate.register_use(
            "uae_s32 d = get_iword_s16(0); m68k_dreg_w_set(regs, 2, d); m68k_dreg_b_set(regs, 3, d);"
        )
        self.assertEqual(reads, frozenset({2, 3}))
        self.assertEqual(writes, frozenset({2, 3}))

    def test_analyze_code_flags_and_codegen_macros(self):
        import os
        import subprocess
        import sys

        sys.path.insert(0, str(self.repo / "tools" / "aot"))
        import aot_translate

        ef_zn = aot_translate.analyze_code_flags("SET_CZNV_ZN(z, n);")
        self.assertEqual(ef_zn.reads, frozenset())
        self.assertEqual(ef_zn.must_w, frozenset({"c", "z", "n", "v"}))

        ef_all = aot_translate.analyze_code_flags("SET_CZNV_ALL(c, z, n, v);")
        self.assertEqual(ef_all.must_w, frozenset({"c", "z", "n", "v"}))

        ef_clr = aot_translate.analyze_code_flags("CLEAR_CZNV;")
        self.assertEqual(ef_clr.must_w, frozenset({"c", "z", "n", "v"}))

        ef_mull = aot_translate.analyze_code_flags("AOT_MULL(a, b, c);")
        self.assertTrue(ef_mull.has_mull)

        ef_exc = aot_translate.analyze_code_flags("AOT_EXCEPTION(5);")
        self.assertTrue(ef_exc.exits)

        ef_beq = aot_translate.analyze_code_flags("cctrue(7)")
        self.assertEqual(ef_beq.reads, frozenset({"z"}))

        # Compile and run a direct check on aot_codegen.h overrides for both
        # AOT_REGS_IS_LOCAL == 0 and AOT_REGS_IS_LOCAL == 1.
        test_cpp = self.work / "codegen_macros_test.cpp"
        test_bin = self.work / "codegen_macros_test"
        test_cpp.write_text(
            """
            #include <cassert>
            #include <cstdint>
            #include <cstring>
            #include "aot_runtime.h"

            // Minimal globals to instantiate aot_codegen.h cleanly.
            uae_u8 *RAMBaseHost = nullptr;
            uae_u32 RAMSize = 0;
            uae_u8 *ROMBaseHost = nullptr;
            uae_u32 ROMBaseMac = 0x40800000u;
            uae_u32 ROMSize = 0x10000u;
            static cpuop_func *cpufunctbl_storage[65536] = {};
            cpuop_func **cpufunctbl = cpufunctbl_storage;
            regstruct regs;
            flag_struct regflags;
            void Exception(int, uaecptr) {}
            void m68k_mull(uae_u32, uae_u32, uae_u16) {}
            void AotRomEntry(uae_u32, uae_u32, uae_u32) {}
            uae_u32 AotRamEntrySlow(uae_u32, uae_u32) { return 0; }
            const AotEntryInfo *aot_entry_infos_runtime = nullptr;
            uae_u32 aot_flush_epoch = 1;
            int32 aot_extra_instructions = 0;
            uae_u32 aot_region_calls = 0;
            uae_u32 aot_trap_vec_cached = 0;
            bool aot_trap_vec_native = false;
            uae_u32 mem_slow_lget(uaecptr) { return 0; }
            uae_u32 mem_slow_wget(uaecptr) { return 0; }
            uae_u32 mem_slow_bget(uaecptr) { return 0; }
            void mem_slow_lput(uaecptr, uae_u32) {}
            void mem_slow_wput(uaecptr, uae_u32) {}
            void mem_slow_bput(uaecptr, uae_u32) {}

            #define AOT_GENERATED_CODE 1
            #define AOT_ROM_BASE 0x40800000u
            #include "aot_codegen.h"

            #define AOT_REGS_IS_LOCAL 0
            #define AOT_REGS regs.regs
            static void test_uncached_dreg_sets() {
                regs.regs[2] = 0x12345678u;
                m68k_dreg_w_set(regs, 2, 0xABCDu);
                assert(m68k_dreg(regs, 2) == 0x1234ABCDu);
                m68k_dreg_b_set(regs, 2, 0xEFu);
                assert(m68k_dreg(regs, 2) == 0x1234ABEFu);
            }
            #undef AOT_REGS
            #undef AOT_REGS_IS_LOCAL

            #define AOT_REGS_IS_LOCAL 1
            #define AOT_REGS local_regs
            static void test_cached_dreg_sets() {
                uae_u32 local_regs[16] = {};
                local_regs[2] = 0x87654321u;
                m68k_dreg_w_set(regs, 2, 0x1234u);
                assert(m68k_dreg(regs, 2) == 0x87651234u);
                m68k_dreg_b_set(regs, 2, 0x56u);
                assert(m68k_dreg(regs, 2) == 0x87651256u);
            }
            #undef AOT_REGS
            #undef AOT_REGS_IS_LOCAL

            int main() {
                uae_u32 aot_c = 1, aot_z = 1, aot_n = 1, aot_v = 1, aot_x = 1;
                CLEAR_CZNV;
                assert(GET_CFLG == 0 && GET_ZFLG == 0 && GET_NFLG == 0 && GET_VFLG == 0 && GET_XFLG == 1);
                SET_CZNV_ZN(1, 0);
                assert(GET_CFLG == 0 && GET_ZFLG == 1 && GET_NFLG == 0 && GET_VFLG == 0);
                SET_CZNV_ALL(1, 0, 1, 1);
                assert(GET_CFLG == 1 && GET_ZFLG == 0 && GET_NFLG == 1 && GET_VFLG == 1);
                aot_store_cznv(regflags, aot_c, aot_z, aot_n, aot_v);
                assert(regflags.c == 1 && regflags.z == 0 && regflags.n == 1 && regflags.v == 1);
                uae_u32 lc = 0, lz = 0, ln = 0, lv = 0;
                aot_load_cznv(regflags, lc, lz, ln, lv);
                assert(lc == 1 && lz == 0 && ln == 1 && lv == 1);
                test_uncached_dreg_sets();
                test_cached_dreg_sets();
                return 0;
            }
            """
        )
        subprocess.run(
            [
                "c++", "-std=gnu++17", "-O1", "-Wall", "-Wextra",
                "-DAOT_HOST_TEST=1",
                "-DSIZEOF_SHORT=2", "-DSIZEOF_INT=4", "-DSIZEOF_LONG=8", "-DSIZEOF_LONG_LONG=8", "-DSIZEOF_VOID_P=8",
                "-I", str(self.repo / "test" / "aot_host" / "stubs"),
                "-I", str(self.repo / "src" / "basilisk"),
                "-I", str(self.repo / "src" / "basilisk" / "include"),
                "-I", str(self.repo / "src" / "basilisk" / "uae_cpu"),
                "-I", str(self.repo / "src" / "basilisk" / "uae_cpu" / "generated"),
                "-I", str(self.repo / "src" / "basilisk" / "aot"),
                str(test_cpp), "-o", str(test_bin),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run([str(test_bin)], check=True)

    def test_synthetic_rom_rol_live_flags_single_entry_jmp_chain_hot_loop(self):
        import re

        streams = [
            # Multi-block ROL.B/W/L + ROR.L handlers
            (0x0400, [
                ([0xE398], 100),  # ROL.L #1, D0
                ([0xE359], 100),  # ROL.W #1, D1
                ([0xE31A], 100),  # ROL.B #1, D2
                ([0xE7B8], 100),  # ROL.L D3, D0
                ([0xE29C], 100),  # ROR.L #1, D4
                ([0x4E75], 100),  # RTS
            ]),
            # Dead live-in flags: MOVEQ kills CZNV, CMP writes CZNV (not X) -> AOT_FLAGS_IN_NONE
            (0x0500, [
                ([0x7007], 100),  # MOVEQ #7, D0
                ([0xB081], 100),  # CMP.L D1, D0
                ([0x4E75], 100),  # RTS
            ]),
            # Live-in Z and X flags via ADDX.L D1, D0
            (0x0540, [
                ([0xD181], 100),  # ADDX.L D1, D0
                ([0x4E75], 100),  # RTS
            ]),
            # AOT_MULL + DIVS.W exception live-flag flush
            (0x0580, [
                ([0x4C01, 0x0800], 100),  # MULS.L D1, D0
                ([0x81C2], 100),          # DIVS.W D2, D0
                ([0x4E75], 100),          # RTS
            ]),
            # Static JMP abs.L merging into 0x0620
            (0x0600, [
                ([0x7007], 100),                 # MOVEQ #7, D0
                ([0x4EF9, 0x4080, 0x0620], 100), # JMP 0x40800620
            ]),
            (0x0620, [
                ([0xD081], 100),  # ADD.L D1, D0
                ([0x4E75], 100),  # RTS
            ]),
            # Intra-blob BSR chaining into multi-entry target 0x0740
            (0x0700, [
                ([0x7007], 100),          # MOVEQ #7, D0
                ([0x6100, 0x003C], 100),  # BSR.W 0x0740
                ([0x5282], 100),          # ADDQ.L #1, D2
                ([0x4E75], 100),          # RTS
            ]),
            (0x0740, [
                ([0xD081], 100),  # ADD.L D1, D0
                ([0xB081], 100),  # CMP.L D1, D0 (second entry via 0x0760)
                ([0x4E75], 100),  # RTS
            ]),
            (0x0760, [
                ([0x6100, 0xFFE0], 100),  # BSR.W 0x0742
                ([0x4E75], 100),          # RTS
            ]),
            # Hot inner loop: triggers AOT_HOT_REGION_ATTR + AOT_INLINE_MEM_FLAG
            (0x0800, [
                ([0x7003], 100),          # MOVEQ #3, D0
                ([0x2218], 10000),        # MOVE.L (A0)+, D1
                ([0x22C1], 10000),        # MOVE.L D1, (A1)+
                ([0x51C8, 0xFFFA], 10000),# DBF D0, 0x0802
                ([0x4E75], 100),          # RTS
            ]),
        ]
        rom_bytes, hot_text = self._build_synthetic_rom(streams)

        for cache_run in ("100", "0"):
            gen_chain, report_chain, _ = self._translate_and_diff(
                f"synth_chain_c{cache_run}",
                rom_bytes,
                hot_text,
                extra_args=(
                    "--cache-run", cache_run,
                    "--chain",
                    "--hot-inline-limit", "4096",
                    "--hot-inline-min-run", "10",
                    "--hot-inline-max-insns", "24",
                ),
                trials=8,
            )
            # Single-entry optimization + multi-entry table check
            self.assertIn("AOT_ROM_ENTER_1(", gen_chain)
            self.assertIn("AOT_ROM_ENTER(", gen_chain)
            self.assertGreater(report_chain.get("rom_single_entry_regions", 0), 0)
            # Live-flag elimination checks
            self.assertIn("#define AOT_FLAGS_IN() do { } while (0)", gen_chain)
            self.assertIn("aot_x = regflags.x;", gen_chain)
            self.assertIn("aot_z = regflags.z;", gen_chain)
            # Static JMP merge (0x40800620 appears inside 0x0600's region via goto, not a separate entry)
            self.assertIn("goto L620;", gen_chain)
            # Chaining check
            self.assertIn("AOT_CHAIN_POLL()", gen_chain)
            self.assertGreater(report_chain.get("rom_chain_sites", 0), 0)
            # Hybrid hot-loop -O2 + inline memory check
            self.assertIn("AOT_HOT_REGION_ATTR", gen_chain)
            self.assertIn("#define AOT_INLINE_MEM_FLAG 1", gen_chain)
            self.assertGreaterEqual(report_chain.get("hot_inline_regions", 0), 1)
            # ROL.L / ROL.W handlers translated cleanly (not failed back to exit)
            self.assertIn("/* 40800400: e398 ROL", gen_chain)
            self.assertIn("/* 40800402: e359 ROL", gen_chain)

        # Verify --no-chain and --hot-inline-limit 0 toggles disable those features cleanly.
        gen_nochain, report_nochain, _ = self._translate_and_diff(
            "synth_nochain",
            rom_bytes,
            hot_text,
            extra_args=("--no-chain", "--hot-inline-limit", "0"),
            trials=4,
        )
        self.assertNotIn("AOT_CHAIN_POLL()", gen_nochain)
        self.assertEqual(report_nochain.get("rom_chain_sites", 0), 0)
        self.assertNotIn("#define AOT_INLINE_MEM_FLAG 1", gen_nochain)
        self.assertEqual(report_nochain.get("hot_inline_regions", 0), 0)

    def test_inline_aline_translation_and_diff(self):
        streams = [
            (0x0400, [
                ([0x7001], 100),  # MOVEQ #1, D0
                ([0xA912], 100),  # Toolbox A-line trap
                ([0x4E75], 100),  # RTS
            ]),
        ]
        rom_bytes, hot_text = self._build_synthetic_rom(streams)
        generated, _, _ = self._translate_and_diff(
            "synth_aline",
            rom_bytes,
            hot_text,
            extra_args=("--inline-aline", "--hot-inline-limit", "0"),
            trials=4,
        )
        self.assertIn("AOT_TOOLBOX_TRAP(", generated)

    def test_runtime_selective_invalidation_and_cpusha_hlk(self):
        import os
        import subprocess

        rt_cpp = self.work / "runtime_bitmap_cpusha_test.cpp"
        rt_bin = self.work / "runtime_bitmap_cpusha_test"
        rt_cpp.write_text(
            """
            #include <cassert>
            #include <cstdint>
            #include <cstdlib>
            #include <cstring>
            #include "aot_runtime.h"

            extern void HostSetRam(uint8 *ram, uint32 size);
            extern void HostSetRom(uint8 *rom, uint32 base, uint32 size);

            // Symbols expected by aot_runtime.cpp / host_env.cpp.
            static cpuop_func *cpufunctbl_storage[65536] = {};
            cpuop_func **cpufunctbl = cpufunctbl_storage;
            regstruct regs;
            flag_struct regflags;
            void REGPARAM2 op_illg_1(uae_u32) {}
            void Exception(int, uaecptr) {}
            void m68k_mull(uae_u32, uae_u32, uae_u16) {}

            static uae_u32 fnv_sum16(const uae_u8 *buf, uae_u32 len) {
                uae_u32 v = 0x811c9dc5u;
                for (uae_u32 i = 0; i + 1 < len; i += 2) {
                    v ^= (uae_u32)buf[i] | ((uae_u32)buf[i + 1] << 8);
                    v *= 0x01000193u;
                }
                return v;
            }

            static uae_u32 crc32_bytes(const uae_u8 *buf, uae_u32 len) {
                uae_u32 crc = ~0u;
                for (uae_u32 i = 0; i < len; ++i) {
                    crc ^= buf[i];
                    for (int b = 0; b < 8; ++b)
                        crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
                }
                return ~crc;
            }

            static void put_be16(uae_u8 *buf, uae_u32 off, uae_u16 val) {
                buf[off] = (uae_u8)(val >> 8);
                buf[off + 1] = (uae_u8)val;
            }

            static void put_be32(uae_u8 *buf, uae_u32 off, uae_u32 val) {
                put_be16(buf, off, (uae_u16)(val >> 16));
                put_be16(buf, off + 2, (uae_u16)val);
            }

            static void dummy_handler(uae_u32) {}

            // Synthetic AotImage fixtures initialized in main before AotInstall().
            static AotRange g_rom_ranges[1];
            static AotRegion g_rom_regions[1];
            static AotEntry g_rom_entries[1];
            static const uae_u16 g_patch_opcodes[3] = { 0x7300u, 0x7301u, 0x7302u };

            // Module 0: [0x0..0xc) at 0x10000 (single page 0x10)
            // Module 1: [0x0..0x8) and [0x1040..0x1048) at 0x20000 (pages 0x20 & 0x21)
            static AotRange g_ram_ranges[3];
            static AotModule g_modules[2];
            static AotEntry g_ram_entries[2];
            static AotModuleState g_module_state[2];
            static const AotEntryInfo g_entry_infos[1] = { { 0u, 0u } };

            extern const AotImage aot_image = {
                0x40800000u, 0x100000u,
                g_rom_ranges, g_rom_regions, g_rom_entries, g_patch_opcodes,
                1u, 1u, 1u,
                g_ram_ranges, g_modules, g_ram_entries, nullptr, g_module_state,
                3u, 2u, 2u, 0u,
                nullptr, nullptr,
                g_entry_infos, 0u,
            };

            int main() {
                const uae_u32 kRamSize = 0x100000u;
                const uae_u32 kRomSize = 0x100000u;
                const uae_u32 kRomBase = 0x40800000u;
                uae_u8 *ram = (uae_u8 *)calloc(kRamSize + 64u, 1);
                uae_u8 *rom = (uae_u8 *)calloc(kRomSize + 64u, 1);
                assert(ram && rom);
                for (uae_u32 i = 0; i < kRomSize; i += 2) put_be16(rom, i, 0x4E71u);
                for (int i = 0; i < 65536; ++i) cpufunctbl[i] = op_illg_1;

                // jCacheFlush at +0x85032: CPUSHA DC/IC (0xf4f8); RTS (0x4e75)
                put_be16(rom, 0x85032u, 0xF4F8u);
                put_be16(rom, 0x85034u, 0x4E75u);

                // Q650 32-bit _HLock tail ending at +0x87824 (20 bytes from +0x87810)
                static const uae_u16 kHLockSeq[10] = {
                    0x08E8u, 0x0007u, 0xFFF5u, 0x0C38u, 0x0004u,
                    0x012Fu, 0x6606u, 0x4EB0u, 0x81E1u, 0x06F4u,
                };
                for (int i = 0; i < 10; ++i) put_be16(rom, 0x87810u + (uae_u32)i * 2u, kHLockSeq[i]);

                // ROM region 0 covers the first word of _HLock (+0x87810 = 0x08e8) so we test patched-ROM lookup!
                g_rom_ranges[0] = { 0x87810u, 2u };
                g_rom_regions[0] = { 0u, 1u, crc32_bytes(rom + 0x87810u, 2u) };
                g_rom_entries[0] = { 0x87810u, 0x08E8u, 0u, dummy_handler };

                // Module 0 at 0x10000: 12 bytes of NOPs + RTS; range excludes 2-byte entry opcode at offset 0.
                const uae_u32 m0_base = 0x10000u;
                for (uae_u32 o = 0; o < 10; o += 2) put_be16(ram, m0_base + o, 0x4E71u);
                put_be16(ram, m0_base + 10u, 0x4E75u);
                g_ram_ranges[0] = { 2u, 10u };
                g_modules[0] = { 0u, 1u, fnv_sum16(ram + m0_base + 2u, 10u), 0u, 1u, 12u };
                g_ram_entries[0] = { 0u, 0x4E71u, 0u, dummy_handler };

                // Module 1 at 0x20000: spans two 4 KiB pages (0x20 and 0x21); ranges exclude entry opcode at offset 0.
                const uae_u32 m1_base = 0x20000u;
                for (uae_u32 o = 0; o < 8; o += 2) put_be16(ram, m1_base + o, 0x4E71u);
                for (uae_u32 o = 0x1040u; o < 0x1048u; o += 2) put_be16(ram, m1_base + o, 0x4E71u);
                uae_u32 c1 = 0x811c9dc5u;
                for (uae_u32 i = 2u; i < 8u; i += 2) {
                    c1 ^= (uae_u32)ram[m1_base + i] | ((uae_u32)ram[m1_base + i + 1] << 8);
                    c1 *= 0x01000193u;
                }
                for (uae_u32 i = 0x1040u; i < 0x1048u; i += 2) {
                    c1 ^= (uae_u32)ram[m1_base + i] | ((uae_u32)ram[m1_base + i + 1] << 8);
                    c1 *= 0x01000193u;
                }
                g_ram_ranges[1] = { 2u, 6u };
                g_ram_ranges[2] = { 0x1040u, 8u };
                g_modules[1] = { 1u, 2u, c1, 1u, 1u, 0x1048u };
                g_ram_entries[1] = { 0u, 0x4E71u, 1u, dummy_handler };

                HostSetRam(ram, kRamSize);
                HostSetRom(rom, kRomBase, kRomSize);
                AotInstall();

                // Verify ROM +0x87810 got patched to 0x7300 and AotOriginalWord recovers 0x08e8.
                assert(((uae_u16)rom[0x87810u] << 8 | rom[0x87811u]) == 0x7300u);
                assert(AotOriginalWord(kRomBase + 0x87810u) == 0x08E8u);

                // Install both RAM modules.
                assert(AotRamInstallAt(0u, m0_base));
                assert(AotRamInstallAt(1u, m1_base));
                const uae_u32 e0 = aot_flush_epoch;
                assert(g_module_state[0].epoch == e0);
                assert(g_module_state[1].epoch == e0);

                // 1. Page-bitmap O(1) skip on unrelated guest & host ranges.
                AotCodeChangedGuestRange(0x05000u, 0x100u);
                AotCodeChangedRange(ram + 0x15000u, 0x200u);
                AotCodeChangedRange(rom + 0x100u, 0x100u);  // out of RAM
                assert(aot_flush_epoch == e0);
                assert(g_module_state[0].epoch == e0 && g_module_state[1].epoch == e0);
                AotStats stats;
                AotReadStats(&stats);
                assert(stats.range_flushes_skipped >= 2u);
                assert(stats.range_invalidations == 0u);

                // 2. Selective invalidation on Module 1's second page (0x21040): preserves Module 0!
                AotCodeChangedGuestRange(0x21042u, 4u);
                assert(aot_flush_epoch == e0);
                assert(g_module_state[1].epoch == 0u);
                assert(g_module_state[0].epoch == e0);
                AotReadStats(&stats);
                assert(stats.range_invalidations == 1u);

                // 3. Re-enter Module 1 via AotRamEntrySlow while unchanged -> re-validates & re-arms bitmap!
                regs.pc_p = ram + m1_base;
                assert(AotRamEntrySlow(1u, 0u) == m1_base);
                assert(g_module_state[1].epoch == e0);
                AotCodeChangedGuestRange(0x21042u, 2u);
                assert(g_module_state[1].epoch == 0u);

                // 4. Mutate Module 1 and re-enter -> drops base to AOT_NO_BASE & restores original opcode!
                put_be16(ram, m1_base + 0x1040u, 0x4E75u);
                assert(AotRamEntrySlow(1u, 0u) == AOT_NO_BASE);
                assert(g_module_state[1].base == AOT_NO_BASE);
                assert(((uae_u16)ram[m1_base] << 8 | ram[m1_base + 1u]) == 0x4E71u);
                assert(g_module_state[0].epoch == e0);

                // 5. Global AotCodeChanged() bumps epoch and clears bitmap so subsequent unrelated range skips.
                AotCodeChanged();
                const uae_u32 e1 = aot_flush_epoch;
                assert(e1 > e0);
                AotCodeChangedGuestRange(m0_base, 4u);
                assert(aot_flush_epoch == e1);  // bitmap was cleared, range didn't re-scan

                // Re-arm Module 0 for CPUSHA tests.
                for (uae_u32 o = 0; o < 10; o += 2) put_be16(ram, m0_base + o, 0x4E71u);
                assert(AotRamInstallAt(0u, m0_base));
                const uae_u32 e2 = aot_flush_epoch;
                assert(g_module_state[0].epoch == e2);

                // Setup valid jCacheFlush CPUSHA caller state (including patched _HLock entry!).
                const uae_u32 sp = 0x8000u;
                m68k_areg(regs, 7) = sp;
                put_be32(ram, sp, kRomBase + 0x87824u);
                regs.pc_p = rom + 0x85032u;

                // 6a. Positive narrow match: unrelated 32-bit relocatable handle at ptr = 0x30000.
                const uae_u32 h_unrelated = 0x30000u;
                put_be32(ram, h_unrelated - 12u, 0x80000000u);  // relocatable tag
                put_be32(ram, h_unrelated - 8u, 0x200u);        // physical size
                m68k_areg(regs, 0) = h_unrelated;
                AotHandleCpusha();
                assert(aot_cpusha_narrowed == 1u && aot_cpusha_global == 0u);
                assert(aot_flush_epoch == e2);
                assert(g_module_state[0].epoch == e2);

                // 6b. Positive narrow match: same-page non-overlapping handle [0x10014, 0x10114) preserves Module 0!
                const uae_u32 h_same_page = 0x10020u;
                put_be32(ram, h_same_page - 12u, 0x80000000u);
                put_be32(ram, h_same_page - 8u, 0x100u);
                m68k_areg(regs, 0) = h_same_page;
                AotHandleCpusha();
                assert(aot_cpusha_narrowed == 2u && aot_cpusha_global == 0u);
                assert(aot_flush_epoch == e2 && g_module_state[0].epoch == e2);

                // 6c. Positive narrow match: byte-overlapping handle [0x10004, 0x10104) invalidates Module 0!
                const uae_u32 h_overlap = 0x10010u;
                put_be32(ram, h_overlap - 12u, 0x80000000u);
                put_be32(ram, h_overlap - 8u, 0x100u);
                m68k_areg(regs, 0) = h_overlap;
                AotHandleCpusha();
                assert(aot_cpusha_narrowed == 3u && aot_cpusha_global == 0u);
                assert(aot_flush_epoch == e2 && g_module_state[0].epoch == 0u);

                // Restore Module 0 bytes and re-arm (test handle *containing* Module 0: [0x0fff0..0x100f0)).
                for (uae_u32 o = 0; o < 10; o += 2) put_be16(ram, m0_base + o, 0x4E71u);
                put_be16(ram, m0_base + 10u, 0x4E75u);
                assert(AotRamInstallAt(0u, m0_base));
                const uae_u32 h_contain = 0x0FFF0u + 12u;
                put_be32(ram, h_contain - 12u, 0xA0000000u);  // bit 7 set
                put_be32(ram, h_contain - 8u, 0x100u);
                m68k_areg(regs, 0) = h_contain;
                AotHandleCpusha();
                assert(aot_cpusha_narrowed == 4u && aot_cpusha_global == 0u);
                assert(aot_flush_epoch == e2 && g_module_state[0].epoch == 0u);

                // 7. All fallback paths must increment aot_cpusha_global and bump aot_flush_epoch.
                uae_u32 expected_global = 0u;
                auto check_fallback = [&]() {
                    const uae_u32 before_epoch = aot_flush_epoch;
                    const uae_u32 before_narrow = aot_cpusha_narrowed;
                    AotHandleCpusha();
                    ++expected_global;
                    assert(aot_cpusha_global == expected_global);
                    assert(aot_cpusha_narrowed == before_narrow);
                    assert(aot_flush_epoch == before_epoch + 1u);
                };

                // (i) regs.pc_p outside ROM
                m68k_areg(regs, 0) = h_unrelated;
                regs.pc_p = ram + 0x100u;
                check_fallback();

                // (ii) regs.pc_p not pointing at CPUSHA+RTS
                regs.pc_p = rom + 0x85000u;
                check_fallback();
                regs.pc_p = rom + 0x85032u;

                // (iii) SP out of bounds
                m68k_areg(regs, 7) = kRamSize - 2u;
                check_fallback();
                m68k_areg(regs, 7) = sp;

                // (iv) Return address outside ROM
                put_be32(ram, sp, 0x1000u);
                check_fallback();

                // (v) Mismatched _HLock caller bytes
                put_be32(ram, sp, kRomBase + 0x86000u);
                check_fallback();
                put_be32(ram, sp, kRomBase + 0x87824u);

                // (vi) A0 < 12 or >= RAMSize
                m68k_areg(regs, 0) = 8u;
                check_fallback();
                m68k_areg(regs, 0) = kRamSize;
                check_fallback();
                m68k_areg(regs, 0) = h_unrelated;

                // (vii) Non-relocatable tag byte (high byte bit 7 == 0)
                put_be32(ram, h_unrelated - 12u, 0x00000000u);
                check_fallback();
                put_be32(ram, h_unrelated - 12u, 0x80000000u);

                // (viii) Bogus hdr_size (< 12 or overflows RAMSize)
                put_be32(ram, h_unrelated - 8u, 8u);
                check_fallback();
                put_be32(ram, h_unrelated - 8u, kRamSize);
                check_fallback();

                AotReadStats(&stats);
                assert(stats.cpusha_narrowed == aot_cpusha_narrowed);
                assert(stats.cpusha_global == aot_cpusha_global);
                assert(stats.cpusha_narrowed == 4u && stats.cpusha_global == 10u);

                free(ram);
                free(rom);
                return 0;
            }
            """
        )
        subprocess.run(
            [
                "c++", "-std=gnu++17", "-O1", "-g", "-Wall", "-Wextra",
                "-fsanitize=address,undefined",
                "-DEMULATED_68K=1", "-DREAL_ADDRESSING=0", "-DDIRECT_ADDRESSING=0", "-DSAVE_MEMORY_BANKS=1",
                "-DAOT_HOST_TEST=1",
                "-DSIZEOF_SHORT=2", "-DSIZEOF_INT=4", "-DSIZEOF_LONG=8", "-DSIZEOF_LONG_LONG=8", "-DSIZEOF_VOID_P=8",
                "-I", str(self.repo / "test" / "aot_host" / "stubs"),
                "-I", str(self.repo / "src" / "basilisk"),
                "-I", str(self.repo / "src" / "basilisk" / "include"),
                "-I", str(self.repo / "src" / "basilisk" / "uae_cpu"),
                "-I", str(self.repo / "src" / "basilisk" / "uae_cpu" / "generated"),
                "-I", str(self.repo / "src" / "basilisk" / "aot"),
                str(self.repo / "src" / "basilisk" / "aot" / "aot_runtime.cpp"),
                str(self.repo / "test" / "aot_host" / "host_env.cpp"),
                str(rt_cpp), "-o", str(rt_bin),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run([str(rt_bin)], check=True, env={**os.environ, "ASAN_OPTIONS": "symbolize=0"})


if __name__ == "__main__":
    unittest.main()
