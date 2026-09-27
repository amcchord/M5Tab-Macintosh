#!/usr/bin/env python3
"""Summarize a firmware PERF sampler run.

    python tools/perf_report.py RUN_DIR .pio/build/ENV/firmware.elf [--top N]

RUN_DIR holds perf.json and perf-samples.bin written by
``speedometer_benchmark.py --suite rating --profile HZ``. The ELF must be the
exact firmware that was running. Reports hardware counters, the host
functions where samples landed, time-weighted 68k opcodes and hot 68k PC
pages. The cache "miss" counters advance per stall cycle, not per line fill
(see src/basilisk/include/perf_sampler.h), so read them as stall proxies.
"""

from __future__ import annotations

import argparse
import bisect
from collections import Counter
import json
from pathlib import Path
import shutil
import struct
import subprocess

SAMPLE = struct.Struct("<IIIHH")  # host pc, host ra, 68k pc, raw opcode, flags


def find_nm() -> str:
    # pioarduino installs toolchains under tools/; older platforms use packages/.
    candidates = [shutil.which("riscv32-esp-elf-nm")] + [
        str(Path.home() / f".platformio/{root}/toolchain-riscv32-esp/bin/riscv32-esp-elf-nm")
        for root in ("tools", "packages")]
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    raise SystemExit("riscv32-esp-elf-nm not found")


def load_symbols(elf: Path) -> list[tuple[int, int, str]]:
    output = subprocess.run([find_nm(), "-S", "-C", "--defined-only", str(elf)],
                            capture_output=True, text=True, check=True).stdout
    symbols = []
    for line in output.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4 and parts[2] in "tTwW":
            symbols.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
    symbols.sort()
    return symbols


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run", type=Path)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--top", type=int, default=30)
    args = parser.parse_args()

    blob = (args.run / "perf-samples.bin").read_bytes()
    counters = {k: int(v) for k, v in json.loads((args.run / "perf.json").read_text()).items()
                if str(v).isdigit()}
    samples = [SAMPLE.unpack_from(blob, offset) for offset in range(0, len(blob) - SAMPLE.size + 1, SAMPLE.size)]
    total = max(len(samples), 1)
    symbols = load_symbols(args.elf)
    starts = [symbol[0] for symbol in symbols]

    def function(pc: int) -> str:
        index = bisect.bisect_right(starts, pc) - 1
        if index >= 0:
            start, size, name = symbols[index]
            if start <= pc < start + max(size, 1):
                return name
        return f"?{pc:08x}"

    print(f"samples={len(samples)} window={counters.get('us', 0) / 1e6:.2f}s "
          f"cycles={counters.get('cycles', 0)} instret={counters.get('instret', 0)}")
    if counters.get("m68k") and counters.get("cycles"):
        m68k = counters["m68k"]
        print(f"68k MIPS={m68k / counters['us']:.3f} cycles/68k={counters['cycles'] / m68k:.1f} "
              f"host instructions/68k={counters['instret'] / m68k:.1f} "
              f"IPC={counters['instret'] / counters['cycles']:.3f}")

    def ranked(title: str, counter: Counter, formatter) -> None:
        print(f"\n== {title} ==")
        running = 0
        for key, count in counter.most_common(args.top):
            running += count
            print(f"{100 * count / total:6.2f}% {100 * running / total:6.2f}%  {formatter(key)}")

    ranked("host functions", Counter(function(s[0]) for s in samples), str)
    ranked("68k opcodes (time weighted)",
           Counter(((s[3] & 0xff) << 8) | (s[3] >> 8) for s in samples), lambda op: f"{op:04x}")
    ranked("68k PC pages (256 bytes)", Counter(s[2] & ~0xff for s in samples), lambda pc: f"{pc:08x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
