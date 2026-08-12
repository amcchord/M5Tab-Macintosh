#!/usr/bin/env python3
"""Run and score Speedometer 4.02 through the @B2 control protocol.

The script keeps one serial connection open for the entire run, launches the
desktop copy of Speedometer, starts ``Run All Tests`` with Command-A, captures
periodic screenshots, OCRs them with macOS Vision, and writes a machine-readable
``result.json`` beside the screenshots. ``--suite color`` runs only Color
QuickDraw (Command-G) and records a ranked A-line profile in ``profile.json``.
``--suite mono`` selects only the monochrome Color QuickDraw workload, which
closely matches the headline Performance Rating Graphics test.

Examples:
    python tools/speedometer_benchmark.py --port /dev/cu.usbmodem14201 --label baseline
    python tools/speedometer_benchmark.py --parse artifacts/tab5-speedometer-rating-2.png
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
from datetime import datetime
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import zlib


PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "tools"))

from mac_control import ControlError, MacControl, parse_key_code  # noqa: E402


BASELINE_CPU = 0.553
BASELINE_GRAPHICS = 0.310
BASELINE_PR = 0.549
SPEEDOMETER_ICON = (590, 95)


@dataclass(frozen=True)
class BenchmarkResult:
    cpu: float
    graphics: float
    pr: float
    disk: float | None = None
    math: float | None = None


def run_ocr(image_path: Path) -> list[dict[str, object]]:
    command = ["swift", str(PROJECT_ROOT / "tools" / "ocr_png.swift"), str(image_path)]
    completed = subprocess.run(
        command,
        check=True,
        capture_output=True,
        text=True,
        cwd=PROJECT_ROOT,
    )
    records: list[dict[str, object]] = []
    for line in completed.stdout.splitlines():
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return records


def _number_right_of(
    records: list[dict[str, object]], label_pattern: str, *, row_tolerance: float = 0.025
) -> float | None:
    labels = [
        record
        for record in records
        if re.fullmatch(label_pattern, str(record.get("text", "")).strip(), re.IGNORECASE)
    ]
    number_pattern = re.compile(r"^[0-9]+(?:\.[0-9]+)$")
    for label in labels:
        label_y = float(label["y"])
        label_x = float(label["x"])
        candidates: list[tuple[float, float]] = []
        for record in records:
            text = str(record.get("text", "")).strip()
            if not number_pattern.fullmatch(text):
                continue
            record_x = float(record["x"])
            record_y = float(record["y"])
            if record_x <= label_x or abs(record_y - label_y) > row_tolerance:
                continue
            candidates.append((record_x - label_x, float(text)))
        if candidates:
            return min(candidates)[1]
    return None


def parse_result(records: list[dict[str, object]]) -> BenchmarkResult | None:
    cpu = _number_right_of(records, r"CPU:?")
    graphics = _number_right_of(records, r"Graphics:?")
    # Vision sometimes interprets the bitmap-font colon as a capital A.
    pr = _number_right_of(records, r"PR(?::|A)?")
    if cpu is None or graphics is None or pr is None:
        return None
    return BenchmarkResult(
        cpu=cpu,
        graphics=graphics,
        pr=pr,
        disk=_number_right_of(records, r"Disk:?") or _number_right_of(records, r"Disk"),
        math=_number_right_of(records, r"Math:?") or _number_right_of(records, r"Math"),
    )


def recognized_text(records: list[dict[str, object]]) -> str:
    return "\n".join(str(record.get("text", "")) for record in records)


def capture(control: MacControl, output: Path, *, color: bool = False) -> Path:
    grab = control.screenshot_color_png if color else control.screenshot_png
    png, width, height = grab(timeout=90.0)
    output.write_bytes(png)
    print(f"Captured {output} ({width}x{height})", flush=True)
    return output


def checkbox_checked(image_path: Path, center_x: int, center_y: int) -> bool:
    """Read a classic Mac checkbox from our filter-0 RGB PNG encoder."""
    png = image_path.read_bytes()
    position = 8
    width = 0
    height = 0
    compressed: list[bytes] = []
    while position + 12 <= len(png):
        size = struct.unpack(">I", png[position : position + 4])[0]
        kind = png[position + 4 : position + 8]
        payload = png[position + 8 : position + 8 + size]
        position += 12 + size
        if kind == b"IHDR":
            width, height = struct.unpack(">II", payload[:8])
        elif kind == b"IDAT":
            compressed.append(payload)
        elif kind == b"IEND":
            break
    rows = zlib.decompress(b"".join(compressed))
    stride = width * 3 + 1
    if width <= center_x + 5 or height <= center_y + 5 or len(rows) != stride * height:
        raise ValueError("unexpected checkbox screenshot dimensions")
    black = 0
    # Stay inside the 12x12 border. An empty box is all white here; a checked
    # box has a diagonal mark covering many of these pixels.
    for y in range(center_y - 4, center_y + 5):
        if rows[y * stride] != 0:
            raise ValueError("unsupported PNG row filter")
        row = y * stride + 1
        for x in range(center_x - 4, center_x + 5):
            red, green, blue = rows[row + x * 3 : row + x * 3 + 3]
            if red < 128 and green < 128 and blue < 128:
                black += 1
    return black >= 4


def configure_color_tests(
    control: MacControl, setup_path: Path, *, monochrome_only: bool
) -> None:
    centers = ((246, 119), (246, 140), (246, 161), (246, 182))
    desired = (True, False, False, False) if monochrome_only else (
        False,
        False,
        False,
        True,
    )
    for index, ((x, y), should_be_checked) in enumerate(zip(centers, desired)):
        if checkbox_checked(setup_path, x, y) != should_be_checked:
            held_click(
                control,
                x,
                y,
                label=f"set Color Test checkbox {index + 1}",
                settle_seconds=0.1,
                hold_seconds=0.1,
            )


def retry_control(label: str, action, attempts: int = 3) -> None:
    """Retry a small ordered control action when USB loses one reply."""
    last_error: Exception | None = None
    for attempt in range(1, attempts + 1):
        try:
            action()
            return
        except (ControlError, OSError) as exc:
            last_error = exc
            print(f"{label} attempt {attempt} failed: {exc}", flush=True)
            time.sleep(0.5)
    assert last_error is not None
    raise last_error


def send_benchmark_chord(control: MacControl, chord: str) -> None:
    """Send a chord once and always release modifiers despite lost replies.

    Input is queued before firmware emits its acknowledgement. Repeating an
    entire benchmark shortcut after a read timeout can therefore launch the
    suite twice, while abandoning the sequence can leave Command held down.
    """
    codes = [parse_key_code(part) for part in chord.split("+") if part]
    for code in codes[:-1]:
        try:
            control.key_code(code, "down")
        except (ControlError, OSError) as exc:
            print(f"{chord}: key-down acknowledgement lost: {exc}", flush=True)
    try:
        control.key_code(codes[-1], "tap")
    except (ControlError, OSError) as exc:
        print(f"{chord}: key-tap acknowledgement lost: {exc}", flush=True)
    finally:
        for code in reversed(codes[:-1]):
            try:
                control.key_code(code, "up")
            except (ControlError, OSError) as exc:
                print(f"{chord}: key-up acknowledgement lost: {exc}", flush=True)


def held_click(
    control: MacControl,
    x: int,
    y: int,
    *,
    label: str,
    settle_seconds: float = 0.15,
    hold_seconds: float = 0.12,
) -> None:
    """Generate a guest-visible click with a human-scale button hold."""
    retry_control(f"{label} move", lambda: control.move(x, y))
    # ADB position and button packets are consumed separately. Give the guest
    # one full input-poll interval to install the absolute coordinate before
    # queueing the mouse-down, matching the touchscreen's deferred-down path.
    time.sleep(settle_seconds)
    retry_control(f"{label} press", lambda: control.mouse_button(0, True))
    try:
        time.sleep(hold_seconds)
    finally:
        # Always attempt the release independently so a lost acknowledgement
        # cannot strand the emulated mouse button in its down state.
        retry_control(f"{label} release", lambda: control.mouse_button(0, False))


def wait_for_speedometer(
    control: MacControl,
    run_dir: Path,
    *,
    launch_timeout: float,
) -> None:
    deadline = time.monotonic() + launch_timeout
    boot_attempt = 0
    splash_dismissed = False
    needs_launch = True
    while True:
        name = "00-before-launch.png" if boot_attempt == 0 else f"boot-{boot_attempt:02d}.png"
        try:
            image_path = capture(control, run_dir / name)
        except (ControlError, OSError) as exc:
            if time.monotonic() >= deadline:
                raise
            boot_attempt += 1
            print(
                f"Finder screenshot attempt {boot_attempt} failed; preserving session: {exc}",
                flush=True,
            )
            time.sleep(5.0)
            continue
        records = run_ocr(image_path)
        text = recognized_text(records)
        if "Organization:" in text and "Cancel" in text:
            retry_control("cancel registration form", lambda: control.click(502, 292))
            needs_launch = False
            print("Speedometer registration form cancelled", flush=True)
            break
        if re.search(r"Not\s*yet", text, re.IGNORECASE):
            retry_control("dismiss registration", lambda: control.click(375, 267))
            needs_launch = False
            print("Speedometer registration prompt dismissed", flush=True)
            break
        if "Help" in text and "File" not in text:
            held_click(control, 330, 175, label="dismiss splash screen")
            splash_dismissed = True
            needs_launch = False
            print("Speedometer splash screen dismissed", flush=True)
            # Registration is presented only after this modal disappears.
            # Re-capture before accepting the application menu behind it.
            time.sleep(2.0)
            continue
        # The registration prompt leaves the Speedometer menu bar visible
        # behind its modal window, so only treat the menus as "active" after
        # all startup-modal checks above have failed.
        if "Tests" in text and "Analysis" in text:
            print("Speedometer is already active", flush=True)
            return
        # Vision occasionally reads the final bitmap-font digit as g/z. The
        # stable prefix plus the Finder menu is enough to identify this test
        # desktop without making launch depend on that one glyph.
        # At 1-bit depth Vision often drops the dithered Speedometer icon
        # label entirely. This benchmark image has stable MicroMac/Shared
        # volume names, so accept those as a second, depth-independent Finder
        # signature once the Finder's File/Special menus are also visible.
        finder_menus = "File" in text and "Special" in text
        benchmark_icon = "Speedometer 4.0" in text
        benchmark_desktop = "MicroMac" in text and "Shared" in text
        if finder_menus and (benchmark_icon or benchmark_desktop):
            break
        if time.monotonic() >= deadline:
            raise TimeoutError("Finder desktop did not become ready before launch timeout")
        boot_attempt += 1
        print(f"Waiting for Finder desktop (attempt {boot_attempt})", flush=True)
        time.sleep(20.0)

    if needs_launch:
        # Select the benchmark's icon, then use Finder's canonical Open command.
        # Separate serial CLICK commands cannot guarantee the guest's configured
        # double-click interval because each waits for a protocol round trip.
        retry_control("select benchmark", lambda: control.click(*SPEEDOMETER_ICON))
        send_benchmark_chord(control, "command+o")

    attempt = 0
    while time.monotonic() < deadline:
        time.sleep(20.0)
        attempt += 1
        try:
            image_path = capture(control, run_dir / f"launch-{attempt:02d}.png")
        except (ControlError, OSError) as exc:
            print(
                f"Launch screenshot attempt {attempt} failed; preserving session: {exc}",
                flush=True,
            )
            continue
        records = run_ocr(image_path)
        text = recognized_text(records)
        if (
            not splash_dismissed
            and "Help" in text
            and "File" not in text
        ):
            held_click(control, 330, 175, label="dismiss splash screen")
            splash_dismissed = True
            print("Speedometer splash screen dismissed", flush=True)
            # The app menu is already visible behind the splash. Do not treat
            # that same capture as ready; registration may be next.
            continue
        if "Organization:" in text and "Cancel" in text:
            retry_control("cancel registration form", lambda: control.click(502, 292))
            print("Speedometer registration form cancelled", flush=True)
        elif re.search(r"Not\s*yet", text, re.IGNORECASE):
            retry_control("dismiss registration", lambda: control.click(375, 267))
            print("Speedometer registration prompt dismissed", flush=True)
        elif "Tests" in text and "Analysis" in text:
            print("Speedometer launch detected", flush=True)
            return
    raise TimeoutError("Speedometer did not become active before launch timeout")


def score_payload(
    result: BenchmarkResult,
    *,
    baseline_cpu: float,
    baseline_graphics: float,
    baseline_pr: float,
) -> dict[str, object]:
    cpu_gain = (result.cpu / baseline_cpu - 1.0) * 100.0
    graphics_gain = (result.graphics / baseline_graphics - 1.0) * 100.0
    pr_gain = (result.pr / baseline_pr - 1.0) * 100.0
    return {
        "result": asdict(result),
        "baseline": {
            "cpu": baseline_cpu,
            "graphics": baseline_graphics,
            "pr": baseline_pr,
        },
        "improvement_percent": {
            "cpu": cpu_gain,
            "graphics": graphics_gain,
            "pr": pr_gain,
        },
        "target_met": cpu_gain >= 50.0 or graphics_gain >= 50.0,
    }


def handle_suite_dialog(control: MacControl, text: str) -> bool:
    """Answer a Speedometer Run All setup dialog recognized by OCR."""
    if "Color Tests:" in text and "Monochrome" in text and "Cancel" in text:
        retry_control("accept Color Tests", lambda: control.click(280, 234))
        print("Speedometer Color Tests selection accepted", flush=True)
        return True
    if re.search(r"Not\s*yet", text, re.IGNORECASE):
        retry_control("dismiss registration", lambda: control.click(375, 267))
        print("Speedometer registration prompt dismissed", flush=True)
        return True
    if "Organization:" in text and "Cancel" in text:
        retry_control("cancel registration form", lambda: control.click(502, 292))
        print("Speedometer registration form cancelled", flush=True)
        return True
    if "Are you sure?" in text and "several minutes" in text:
        retry_control("confirm Run All Tests", lambda: control.click(280, 188))
        print("Speedometer Run All confirmation accepted", flush=True)
        return True
    if "Choose which drive to test" in text:
        retry_control("choose Disk test drive", lambda: control.click(420, 211))
        print("Speedometer Disk drive selection accepted", flush=True)
        return True
    if "tests are done" in text.lower():
        retry_control("close tests-complete dialog", lambda: control.click(320, 153))
        time.sleep(0.5)
        for index in range(3):
            # KEY actions are applied before their serial acknowledgement.
            # Retrying Command-W after a lost reply can close the next window
            # too, including the final Performance Rating evidence.
            send_benchmark_chord(control, "command+w")
            time.sleep(0.25)
        print("Speedometer tests-complete dialog closed", flush=True)
        return True
    return False


def read_ranked_profile(control: MacControl, command: str) -> list[str]:
    """Read a ranked firmware profile in short USB-safe records."""
    records: list[str] = []
    for offset in range(0, 16, 3):
        records.append(
            control.request(f"{command} {offset}", f"OK {command}", timeout=15.0)
        )
    return records


def read_trap_profile(control: MacControl) -> dict[str, object]:
    return {
        "traps": read_ranked_profile(control, "TRAPS"),
        "layer_selectors": read_ranked_profile(control, "LAYER"),
        "quickdraw_accel": control.request(
            "QDACCEL", "OK QDACCEL", timeout=15.0
        ),
        "quickdraw_region": control.request(
            "QDREGION", "OK QDREGION", timeout=15.0
        ),
    }


def run_benchmark(args: argparse.Namespace) -> int:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = args.output_root / f"{stamp}-{args.label}"
    run_dir.mkdir(parents=True, exist_ok=False)

    with MacControl(args.port, reset_on_failure=not args.no_reset) as control:
        print(control.connect(timeout=args.connect_timeout), flush=True)
        try:
            print(control.info(), flush=True)
        except (ControlError, OSError) as exc:
            print(f"device info acknowledgement lost; continuing: {exc}", flush=True)
        try:
            retry_control("release input", control.release_all)
        except (ControlError, OSError) as exc:
            # RELEASE_ALL is idempotent and is applied before the firmware
            # transmits its acknowledgement. Boot diagnostics can obscure all
            # duplicate replies without preventing the guest-side release.
            print(f"release input acknowledgement lost; continuing: {exc}", flush=True)
        wait_for_speedometer(
            control,
            run_dir,
            launch_timeout=args.launch_timeout,
        )
        try:
            retry_control(
                "reset trap profile",
                lambda: control.request("TRAPS RESET", "OK TRAPS RESET"),
            )
        except (ControlError, OSError) as exc:
            print(f"trap-profile reset unavailable; continuing: {exc}", flush=True)
        suite_shortcut = (
            "command+g" if args.suite in ("color", "mono") else "command+a"
        )
        send_benchmark_chord(control, suite_shortcut)
        time.sleep(5.0)
        if args.suite in ("color", "mono"):
            # Read the persisted checkbox state before starting the timed loop,
            # so repeated focused runs select an absolute rather than toggled
            # configuration.
            setup_path = capture(control, run_dir / "01-color-setup.png")
            configure_color_tests(
                control, setup_path, monochrome_only=args.suite == "mono"
            )
            retry_control("accept Color Tests setup", lambda: control.click(280, 234))
            print("Speedometer Color Tests setup accepted", flush=True)
        else:
            started_path = capture(control, run_dir / "01-run-started.png")
            started_text = recognized_text(run_ocr(started_path))
            handle_suite_dialog(control, started_text)

        deadline = time.monotonic() + args.benchmark_timeout
        sample = 0
        all_tests_complete = False
        while time.monotonic() < deadline:
            time.sleep(args.poll_interval)
            sample += 1
            try:
                image_path = capture(control, run_dir / f"sample-{sample:02d}.png")
            except (ControlError, OSError) as exc:
                print(
                    f"Sample {sample}: screenshot failed; preserving session: {exc}",
                    flush=True,
                )
                continue
            records = run_ocr(image_path)
            text = recognized_text(records)
            if args.suite in ("color", "mono") and "tests are done" in text.lower():
                retry_control(
                    "close tests-complete dialog", lambda: control.click(320, 153)
                )
                time.sleep(2.0)
                result_image = capture(control, run_dir / "color-result.png")
                payload: dict[str, object] = {
                    "captured_at": datetime.now().isoformat(),
                    "label": args.label,
                    "suite": args.suite,
                    "screenshot": result_image.name,
                }
                try:
                    payload["trap_profile"] = read_trap_profile(control)
                except (ControlError, OSError) as exc:
                    payload["trap_profile_error"] = str(exc)
                profile_path = run_dir / "profile.json"
                profile_path.write_text(
                    json.dumps(payload, indent=2, sort_keys=True) + "\n"
                )
                print(json.dumps(payload, indent=2, sort_keys=True), flush=True)
                return 0

            # Run All updates the Performance Test window after every
            # component. Those intermediate windows contain CPU/Graphics/PR
            # labels and are intentionally parseable-looking, but unrun rows
            # still contain zeroes. Only accept a rating after Speedometer's
            # explicit completion modal has been observed and dismissed.
            if args.suite == "all" and "tests are done" in text.lower():
                handle_suite_dialog(control, text)
                all_tests_complete = True
                continue
            if args.suite == "all" and not all_tests_complete:
                if handle_suite_dialog(control, text):
                    continue
                print(f"Sample {sample}: full suite still running", flush=True)
                continue

            result = parse_result(records)
            if result is None:
                if handle_suite_dialog(control, text):
                    continue
                print(f"Sample {sample}: result dialog not present yet", flush=True)
                continue

            payload = score_payload(
                result,
                baseline_cpu=args.baseline_cpu,
                baseline_graphics=args.baseline_graphics,
                baseline_pr=args.baseline_pr,
            )
            payload["captured_at"] = datetime.now().isoformat()
            payload["label"] = args.label
            payload["screenshot"] = image_path.name
            try:
                payload["trap_profile"] = read_trap_profile(control)
            except (ControlError, OSError) as exc:
                payload["trap_profile_error"] = str(exc)
            result_path = run_dir / "result.json"
            result_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
            print(json.dumps(payload, indent=2, sort_keys=True), flush=True)
            return 0

    raise TimeoutError("Speedometer result dialog was not detected before benchmark timeout")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial device (or BASILISK_PORT)")
    parser.add_argument("--label", default="run")
    parser.add_argument(
        "--suite", choices=("all", "color", "mono"), default="all"
    )
    parser.add_argument(
        "--output-root",
        type=Path,
        default=PROJECT_ROOT / "artifacts" / "performance-runs",
    )
    parser.add_argument("--connect-timeout", type=float, default=120.0)
    parser.add_argument("--launch-timeout", type=float, default=300.0)
    parser.add_argument("--benchmark-timeout", type=float, default=2400.0)
    parser.add_argument("--poll-interval", type=float, default=90.0)
    parser.add_argument("--baseline-cpu", type=float, default=BASELINE_CPU)
    parser.add_argument("--baseline-graphics", type=float, default=BASELINE_GRAPHICS)
    parser.add_argument("--baseline-pr", type=float, default=BASELINE_PR)
    parser.add_argument("--no-reset", action="store_true")
    parser.add_argument(
        "--parse",
        type=Path,
        metavar="IMAGE",
        help="only OCR and score an existing Speedometer result screenshot",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.parse is not None:
        result = parse_result(run_ocr(args.parse))
        if result is None:
            print("No complete Speedometer result found", file=sys.stderr)
            return 1
        print(
            json.dumps(
                score_payload(
                    result,
                    baseline_cpu=args.baseline_cpu,
                    baseline_graphics=args.baseline_graphics,
                    baseline_pr=args.baseline_pr,
                ),
                indent=2,
                sort_keys=True,
            )
        )
        return 0
    return run_benchmark(args)


if __name__ == "__main__":
    raise SystemExit(main())
