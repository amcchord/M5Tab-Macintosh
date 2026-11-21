#!/usr/bin/env python3
"""Install a P4 application from a computer on the same LAN as the Tab5."""
import argparse
import functools
import hashlib
import http.server
import ipaddress
import sys
import threading
import time
from pathlib import Path

from mac_control import ControlError, MacControl

APP_TAG_OFFSET = 288
BOARD_TAGS = {
    "M5Stack Tab5": b"M5Tab-Macintosh/Tab5/1\0",
    "Waveshare P4 10.1": b"M5Tab-Macintosh/Waveshare-P4-10.1/1\0",
}


class ImageHandler(http.server.BaseHTTPRequestHandler):
    def __init__(self, *args, image, **kwargs):
        self.image = image
        super().__init__(*args, **kwargs)

    def do_GET(self):
        if self.path != "/firmware.bin":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(self.image)))
        self.end_headers()
        self.connection.settimeout(15)
        try:
            self.wfile.write(self.image)
        except (TimeoutError, ConnectionError):
            pass

    def log_message(self, *_):
        pass


def status(control):
    reply = control.request("BOOT UPDATE STATUS", "OK BOOT UPDATE STATUS", timeout=8)
    return dict(item.split("=", 1) for item in reply.split()[4:] if "=" in item)


def install(control, path, address, reboot):
    if path.stat().st_size > 0x600000:
        raise ValueError("File exceeds the 6 MiB application slot. Select the application-only firmware.bin.")
    image = path.read_bytes()
    if image[:1] != b'\xe9' or len(image) < APP_TAG_OFFSET + 44:
        raise ValueError("Select an application-only ESP32-P4 firmware.bin.")
    # INFO returns the board name followed by width and height.
    info = control.info().split()
    board = " ".join(info[4:-2])
    tag = BOARD_TAGS.get(board)
    if not tag or not image.startswith(tag, APP_TAG_OFFSET):
        raise ValueError(f"The file has no compatible Wi-Fi update descriptor for {board}.")
    if control.boot_status().get("screen") != "settings":
        raise ValueError("Shut down in Finder, then enter Startup Settings.")
    initial = status(control)
    if initial.get("layout") != "ab":
        raise ValueError("This board needs the optional A/B partition layout provisioned over USB.")
    current = initial["running"]
    expected_slot = {"ota_0": "ota_1", "ota_1": "ota_0"}[current]
    digest = hashlib.sha256(image).hexdigest()
    handler = functools.partial(ImageHandler, image=image)
    with http.server.ThreadingHTTPServer((address, 0), handler) as server:
        port = server.server_address[1]
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            print(f"Sending {len(image)} bytes to {expected_slot}. SHA-256 {digest}", flush=True)
            url = f"http://{address}:{port}/firmware.bin"
            control.request(f"BOOT UPDATE START {len(image)} {digest} {url}", "OK BOOT UPDATE START")
            deadline = time.monotonic() + 16 * 60
            last = None
            while time.monotonic() < deadline:
                state = status(control)
                if state["state"] == "failed":
                    raise ControlError("Device rejected the update: " + state["result"])
                if state["state"] == "ready":
                    if state["boot"] != expected_slot or int(state["verified"]) != len(image):
                        raise ControlError("Device reported an unexpected boot slot or verified byte count")
                    break
                progress = (state["state"], (int(state["bytes"]) + int(state["verified"])) // 262144)
                if progress != last:
                    print(f"{state['state']}  received {state['bytes']}  verified {state['verified']}", flush=True)
                    last = progress
                time.sleep(1)
            else:
                raise ControlError("Timed out waiting for the device to verify the update")
        finally:
            server.shutdown()
            thread.join()
    print("Flash readback verified. Selected " + expected_slot + " for the next boot.")
    if reboot:
        control.request("BOOT UPDATE REBOOT", "OK BOOT UPDATE REBOOT")
    return expected_slot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--host", required=True, type=ipaddress.IPv4Address,
                        help="LAN IPv4 address of this computer reachable from the device")
    parser.add_argument("--port", help="USB serial port")
    parser.add_argument("--reboot", action="store_true", help="Restart into Startup Settings after verification")
    args = parser.parse_args()
    try:
        with MacControl(args.port, reset_on_failure=False) as control:
            control.connect()
            slot = install(control, args.image, str(args.host), args.reboot)
            if args.reboot:
                previous = control.session_id
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    try:
                        control.connect(timeout=4)
                        if control.session_id != previous and status(control).get("running") == slot:
                            print("Running " + slot)
                            return 0
                    except (ControlError, OSError):
                        pass
                    time.sleep(1)
                raise ControlError("Update staged. Could not confirm the new boot over USB.")
        return 0
    except (ValueError, OSError, ControlError) as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
