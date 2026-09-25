"""Names and validation for merged firmware downloads (flash at 0x0)."""
import argparse
from pathlib import Path
import struct

TARGETS = {
    "esp32p4_pioarduino": "M5Tab-Macintosh",
    "esp32p4_pioarduino_rev3": "M5Tab-Macintosh-Rev3",
    "waveshare_p4_101": "M5Tab-Macintosh-Waveshare-P4-10.1",
    "waveshare_p4_101_rev3": "M5Tab-Macintosh-Waveshare-P4-10.1-Rev3",
}


def validate_merged(merged: Path, build_dir: Path) -> None:
    image = merged.read_bytes()
    for name, offset in (("bootloader.bin", 0x2000), ("partitions.bin", 0x8000),
                         ("firmware.bin", 0x10000)):
        component = (build_dir / name).read_bytes()
        if not component or image[offset:offset + len(component)] != component:
            raise ValueError(f"{merged.name}: {name} missing or differs at {offset:#x}")
        if name != "partitions.bin":
            if len(component) < 24 or component[0] != 0xE9:
                raise ValueError(f"{name}: invalid ESP image header")
            if struct.unpack_from("<H", component, 12)[0] != 18:
                raise ValueError(f"{name}: not an ESP32-P4 image")
        elif component[:2] != b"\xaa\x50":
            raise ValueError("invalid partition table")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("environment", choices=TARGETS)
    parser.add_argument("--merged", type=Path)
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()
    if args.merged:
        if not args.build_dir:
            parser.error("--merged requires --build-dir")
        validate_merged(args.merged, args.build_dir)
        print(f"Verified bootloader, partitions and application: {args.merged.name}")
    else:
        print(TARGETS[args.environment])
