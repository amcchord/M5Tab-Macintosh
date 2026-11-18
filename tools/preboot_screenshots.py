#!/usr/bin/env python3
"""Capture diagnostic screenshots of every preboot screen over USB serial.

Reboots once into the preboot settings (unless the device is already there),
then walks the screens with injected touches and saves, for each step:

    <step>.png          the screen at the Mac's logical resolution
    <step>-panel.png    full-resolution panel readback (with --panel)

plus manifest.json with the boot status, PANEL VERIFY result (does the panel
hold the screen exactly pixel-doubled?) and capture timings.

The walk only changes in-memory selections; it reloads the saved settings at
the end, so nothing stored on the SD card changes. The device is left on the
settings screen unless --boot is given. USB disk mode unmounts the SD card
while it is shown, so it is visited only with --usb.

Example:
    python3 tools/preboot_screenshots.py out/preboot --panel
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import time
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mac_control import ControlError, MacControl  # noqa: E402


class Tour:
    def __init__(self, control: MacControl, out: Path, panel: bool) -> None:
        self.control = control
        self.out = out
        self.panel = panel
        self.steps: list[dict[str, Any]] = []

    def wait_screen(self, screen: str, timeout: float = 20.0) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        status: dict[str, Any] = {}
        while time.monotonic() < deadline:
            status = self.control.boot_status()
            if status["screen"] == screen:
                return status
            time.sleep(0.2)
        raise ControlError(f"expected the {screen} screen, still on {status.get('screen')}")

    def capture(self, step: str, settle: float = 0.3) -> None:
        time.sleep(settle)   # let the GUI repaint after the last touch
        record: dict[str, Any] = {"step": step, "status": self.control.boot_status()}
        started = time.monotonic()
        png, width, height = self.control.screenshot_color_png()
        (self.out / f"{step}.png").write_bytes(png)
        record["screenshot"] = {"file": f"{step}.png", "size": [width, height],
                                "seconds": round(time.monotonic() - started, 2)}
        record["panel_verify"] = self.control.panel_verify()
        if self.panel:
            started = time.monotonic()
            png, width, height = self.control.screenshot_panel_png()
            (self.out / f"{step}-panel.png").write_bytes(png)
            record["panel"] = {"file": f"{step}-panel.png", "size": [width, height],
                               "seconds": round(time.monotonic() - started, 2)}
        verify = record["panel_verify"]
        print(f"{step:28s} {width}x{height}  panel mismatches {verify['mismatched']}"
              f"/{verify['checked']}")
        self.steps.append(record)

    def run(self, visit_usb: bool, scan_seconds: float) -> None:
        c = self.control
        status = c.boot_status()
        if status["phase"] != "preboot" or status["screen"] != "settings":
            print("entering preboot settings (one reboot)...")
            c.boot_enter()
        self.wait_screen("settings")
        self.capture("settings")

        # A push button while the finger is down, released outside (no action).
        start = c.boot_hit("start")
        c.touch("down", *start["center"])
        self.capture("settings-start-pressed", settle=0.2)
        c.touch("move", 4, 120)
        c.touch("up")

        # Scroll and select in the CD-ROM list, then the RAM radios.
        cdrom = c.boot_hit("cdrom")
        c.touch("tap", cdrom["x"] + 40, cdrom["y"] + 1 + 22 + 11)   # second row
        c.tap_control("ram4")
        self.capture("settings-edited")

        c.tap_control("wifi")
        self.wait_screen("wifi")
        time.sleep(scan_seconds)   # let the network scan finish
        self.capture("wifi")
        c.tap_control("password")
        self.wait_screen("keyboard")
        self.capture("keyboard")
        for key in ("key:m", "key:a", "key:c", "shift", "key:o", "key:s"):
            c.tap_control(key)
        self.capture("keyboard-typed")
        c.tap_control("cancel")          # discard the typed text
        self.wait_screen("wifi")
        c.tap_control("back")
        self.wait_screen("settings")

        c.boot_action("reload")          # discard the walk's edits
        self.capture("settings-reloaded")

        if visit_usb:
            c.tap_control("usb")
            self.wait_screen("usb")
            self.capture("usb-disk", settle=1.0)
            done = c.boot_hit("done")
            try:
                c.touch("tap", *done["center"])
            except (ControlError, OSError):
                # The Tab5 can restart before acknowledging the touch. The
                # settings-screen check below confirms that Done took effect.
                pass
            # The Tab5 restarts to release the USB port and comes back to
            # the settings screen; the Waveshare returns without a restart.
            deadline = time.monotonic() + 60.0
            while True:
                try:
                    self.wait_screen("settings", timeout=5.0)
                    break
                except (ControlError, OSError):
                    if time.monotonic() > deadline:
                        raise
                    try:
                        c.connect(timeout=5.0)
                    except (ControlError, OSError):
                        time.sleep(1.0)
            self.capture("settings-after-usb")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", type=Path, help="directory for PNGs and manifest.json")
    parser.add_argument("--port", help="serial device (or BASILISK_PORT)")
    parser.add_argument("--panel", action="store_true",
                        help="also read back the full-resolution panel for every step")
    parser.add_argument("--usb", action="store_true",
                        help="visit USB disk mode (unmounts the SD card while shown)")
    parser.add_argument("--scan-seconds", type=float, default=6.0,
                        help="time allowed for the Wi-Fi scan before capturing")
    parser.add_argument("--boot", action="store_true", help="start the emulator afterwards")
    args = parser.parse_args(argv)
    args.output.mkdir(parents=True, exist_ok=True)
    try:
        with MacControl(args.port, reset_on_failure=False) as control:
            control.connect()
            info = control.info()
            tour = Tour(control, args.output, args.panel)
            try:
                tour.run(args.usb, args.scan_seconds)
            finally:
                manifest = {"info": info, "captured_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
                            "steps": tour.steps}
                (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            bad = [s["step"] for s in tour.steps if s["panel_verify"]["mismatched"]]
            if args.boot:
                control.boot_start()
            if bad:
                print(f"panel does not match the screen for: {', '.join(bad)}", file=sys.stderr)
                return 2
        print(f"wrote {len(tour.steps)} steps to {args.output}")
        return 0
    except (ControlError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
