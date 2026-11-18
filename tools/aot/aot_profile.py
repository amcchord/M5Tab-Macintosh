#!/usr/bin/env python3
"""Collect ROM instruction execution counts from a profiling build.

    aot_profile.py start [--port PORT]
    aot_profile.py stop --out counts.txt [--ram ram.txt] [--port PORT]
    aot_profile.py merge --out hot.txt counts.txt [counts.txt ...]

Use the AOT profiling build for the board's silicon revision. `start` clears the
counters and begins counting; run the workload (for example
tools/speedometer_benchmark.py --suite rating); `stop` ends counting and
writes "ROM offset (hex) count" lines. `merge` adds several count files into
the hot list that aot_translate.py reads.

With --ram, `stop` also writes the code that ran in guest RAM: lines
"C address count" for every instruction executed and "B address hexbytes"
for the memory around it as it is when the profile is read.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

CHUNK = 64


def ask(control, command: str, digits: int | None = None) -> str:
    """Send a command and return the hex field of its reply.

    Diagnostic output from other firmware tasks can run into a reply, so the
    field is taken by position and length and the request repeated if it is
    not there.
    """
    header = "OK " + " ".join(command.split()[:2])
    arguments = len(command.split()) - 2
    for _ in range(5):
        fields = control.request(command, header, timeout=15.0).split()
        try:
            start = fields.index("OK") + 3 + arguments
        except ValueError:
            continue
        for field in fields[start:start + 2]:
            if (digits is None or len(field) == digits) and re.fullmatch(r"[0-9A-Fa-f]+", field):
                return field
    raise SystemExit(f"no usable reply to {command}")


def read_counts(path: Path) -> Counter:
    counts: Counter = Counter()
    for line in path.read_text().splitlines():
        if line and not line.startswith("#"):
            offset, count = line.split()
            counts[int(offset, 16)] += int(count)
    return counts


def write_counts(path: Path, counts: Counter, title: str) -> None:
    lines = [f"# {title}", "# ROM offset (hex) and times the instruction there was executed"]
    lines += [f"{offset:x} {count}" for offset, count in sorted(counts.items()) if count]
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=("start", "stop", "merge"))
    parser.add_argument("files", nargs="*", type=Path)
    parser.add_argument("--port")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--merge", type=Path, action="append", default=[])
    parser.add_argument("--ram", type=Path, help="with stop: also write the RAM code profile here")
    args = parser.parse_args()

    if args.action == "merge":
        total: Counter = Counter()
        for path in args.files:
            total += read_counts(path)
        write_counts(args.out, total, "merged ROM execution counts")
        print(f"{len(total)} instructions, {sum(total.values())} executions")
        return 0

    from mac_control import MacControl

    with MacControl(args.port, reset_on_failure=False) as control:
        control.connect(30.0)
        if args.action == "start":
            print(control.request("AOTPROF START", "OK AOTPROF START", timeout=15.0))
            return 0
        reply = control.request("AOTPROF STOP", "OK AOTPROF STOP", timeout=15.0)
        chunks = int(reply.rsplit("chunks=", 1)[1])
        used = []
        for first in range(0, chunks, 1024):
            bitmap = bytes.fromhex(ask(control, f"AOTPROF MAP {first}"))
            for index in range(len(bitmap) * 8):
                if bitmap[index // 8] & (1 << (index % 8)) and first + index < chunks:
                    used.append(first + index)
        counts: Counter = Counter()
        for chunk in used:
            data = ask(control, f"AOTPROF READ {chunk}", CHUNK * 8)
            for slot in range(CHUNK):
                value = int(data[slot * 8:slot * 8 + 8], 16)
                if value:
                    counts[(chunk * CHUNK + slot) * 2] = value
        if args.ram:
            read_ram(control, args.ram)
    for path in args.merge:
        counts += read_counts(path)
    write_counts(args.out, counts, "ROM execution counts")
    print(f"{len(counts)} instructions, {sum(counts.values())} executions, {len(used)} chunks read")
    return 0


def read_ram(control, path: Path) -> None:
    pages = []
    first = 0
    total = None
    while total is None or first < total:
        fields = control.request(f"AOTPROF RAMMAP {first}", "OK AOTPROF RAMMAP", timeout=15.0).split()
        start = fields.index("RAMMAP")
        total = int(fields[start + 2])
        bitmap = bytes.fromhex(fields[start + 3]) if len(fields) > start + 3 else b""
        for index in range(len(bitmap) * 8):
            if bitmap[index // 8] & (1 << (index % 8)) and first + index < total:
                pages.append(first + index)
        first += 1024
    lines = ["# RAM code profile: C address count / B address bytes"]
    executed = 0
    for page in pages:
        bits = int(ask(control, f"AOTPROF RAMCHUNKS {page}", 8), 16)
        for chunk in range(32):
            if not bits & (1 << chunk):
                continue
            data = ask(control, f"AOTPROF RAMREAD {page} {chunk}", CHUNK * 8)
            base = (page << 12) + chunk * CHUNK * 2
            for slot in range(CHUNK):
                value = int(data[slot * 8:slot * 8 + 8], 16)
                if value:
                    lines.append(f"C {base + slot * 2:x} {value}")
                    executed += 1
            # The chunk's memory plus room for the last instruction's operands.
            length = min(CHUNK * 2 + 16, total * 4096 - base)
            memory = None
            for _ in range(5):
                fields = control.request(f"PEEK {base:X} {length}", "OK PEEK", timeout=15.0).split()
                position = fields.index("PEEK") + 2
                if (len(fields) > position and len(fields[position]) == length * 2
                        and re.fullmatch(r"[0-9A-Fa-f]+", fields[position])):
                    memory = fields[position]
                    break
            if memory is None:
                raise SystemExit(f"no usable reply to PEEK {base:X}")
            lines.append(f"B {base:x} {memory}")
    path.write_text("\n".join(lines) + "\n")
    print(f"RAM: {executed} instructions in {len(pages)} pages")


if __name__ == "__main__":
    raise SystemExit(main())
