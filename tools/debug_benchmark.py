#!/usr/bin/env python3
"""Measure an attached device's serial control and screenshot reliability.

Keeps one connection open, never resets the device, and never injects input.
Writes machine-readable results plus the last successful PNG of each mode.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import statistics
import time

from mac_control import MacControl, ControlError


def summarize(rows: list[dict]) -> dict:
    summary = {}
    for kind in sorted({row["operation"] for row in rows}):
        selected = [row for row in rows if row["operation"] == kind]
        times = sorted(row["seconds"] for row in selected if row["ok"])
        summary[kind] = {
            "passed": len(times), "failed": len(selected) - len(times),
            "median_seconds": statistics.median(times) if times else None,
            "p95_seconds": times[math.ceil(len(times) * .95) - 1] if times else None,
        }
    return summary


def benchmark(control: MacControl, samples: int, output: Path, timeout: float) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    rows = []
    report = {"protocol": control.protocol_version, "samples": rows}
    for index in range(samples):
        for operation in ("ping", "mono", "color"):
            started = time.monotonic()
            row = {"sample": index, "operation": operation}
            try:
                if operation == "ping":
                    control.request("PING", "OK PONG", timeout=min(timeout, 2))
                else:
                    # Force serial, including on v3. A WiFi fallback would
                    # conceal a broken USB connection in a regression report.
                    png, width, height = control._screenshot_png_once(timeout, monochrome=operation == "mono")
                    (output / f"{operation}.png").write_bytes(png)
                    row.update(control.last_screenshot_stats)
                    row.update(width=width, height=height)
                row["ok"] = True
            except (ControlError, OSError) as exc:
                row.update(ok=False, error=str(exc))
            row["seconds"] = time.monotonic() - started
            rows.append(row)
            print(json.dumps(row), flush=True)
            report["summary"] = summarize(rows)
            (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port")
    parser.add_argument("--samples", type=int, default=10)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--output", type=Path, default=Path("artifacts/serial-debug/benchmark"))
    args = parser.parse_args()
    if args.samples < 1 or args.timeout <= 0:
        parser.error("samples and timeout must be positive")
    with MacControl(args.port, reset_on_failure=False) as control:
        control.connect()
        report = benchmark(control, args.samples, args.output, args.timeout)
    print(json.dumps(report["summary"], indent=2))
    return int(any(not row["ok"] for row in report["samples"]))


if __name__ == "__main__":
    raise SystemExit(main())
