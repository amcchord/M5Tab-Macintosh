#!/usr/bin/env python3
"""Read the shutdown flags of classic HFS disk images, including images in a ZIP."""

import argparse
from pathlib import Path, PurePosixPath
import struct
import sys
import zipfile
import zlib


SECTOR = 512
DISK_SUFFIXES = {".dsk", ".img", ".hfv"}


def read_sector(stream, block):
    stream.seek(block * SECTOR)
    data = stream.read(SECTOR)
    if len(data) != SECTOR:
        raise ValueError("incomplete disk image")
    return data


def volume_flags(stream, size):
    start = 0
    sectors = size // SECTOR
    # The emulator uses the first Apple_HFS entry in the first 64 sectors.
    entry = read_sector(stream, 1)
    if entry[:2] == b"PM":
        entries = struct.unpack_from(">I", entry, 4)[0]
        for index in range(1, min(entries, sectors - 1, 63) + 1):
            if index > 1:
                entry = read_sector(stream, index)
            if entry[:2] != b"PM":
                raise ValueError("invalid Apple partition map")
            if entry[48:80].split(b"\0", 1)[0] == b"Apple_HFS":
                start, length = struct.unpack_from(">II", entry, 8)
                if start <= entries or start + length > sectors:
                    raise ValueError("HFS partition lies outside the data area")
                sectors = length
                break
        else:
            raise ValueError("no Apple_HFS partition in the supported partition map")

    if sectors < 5:
        raise ValueError("volume is too small for HFS")
    header = read_sector(stream, start + 2)
    if header[:2] in (b"H+", b"HX") or (header[:2] == b"BD" and header[124:126] == b"H+"):
        raise ValueError("HFS Plus volumes are outside this classic HFS check")
    if header[:2] != b"BD":
        raise ValueError("classic HFS volume header not found")
    blocks, block_size = struct.unpack_from(">HI", header, 18)
    first_block = struct.unpack_from(">H", header, 28)[0]
    if (not blocks or not block_size or block_size % SECTOR or first_block < 3
            or first_block * SECTOR + blocks * block_size > (sectors - 2) * SECTOR):
        raise ValueError("invalid HFS allocation geometry")
    return struct.unpack_from(">H", header, 10)[0]


def check_stream(label, stream, size):
    try:
        flags = volume_flags(stream, size)
    except ValueError as exc:
        print(f"{label}: could not check ({exc})", file=sys.stderr)
        return 2
    if flags & 0x0800 or not flags & 0x0100:
        state = "marked inconsistent" if flags & 0x0800 else "missing clean-shutdown flag"
        print(f"{label}: {state} (volume flags 0x{flags:04x})")
        return 1
    print(f"{label}: marked cleanly unmounted (volume flags 0x{flags:04x})")
    return 0


def check_path(path):
    if path.suffix.lower() != ".zip":
        with path.open("rb") as stream:
            size = stream.seek(0, 2)
            return check_stream(path, stream, size)

    results = []
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            name = PurePosixPath(info.filename)
            if (info.is_dir() or "__MACOSX" in name.parts or name.name.startswith("._")
                    or name.suffix.lower() not in DISK_SUFFIXES):
                continue
            with archive.open(info) as stream:
                results.append(check_stream(f"{path}!{name}", stream, info.file_size))
    if not results:
        raise ValueError("ZIP contains no .dsk, .img or .hfv disk images")
    return max(results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path, metavar="IMAGE_OR_ZIP")
    status = 0
    for path in parser.parse_args().paths:
        try:
            status = max(status, check_path(path))
        except (OSError, ValueError, EOFError, RuntimeError, zipfile.BadZipFile, zlib.error) as exc:
            print(f"{path}: could not check ({exc})", file=sys.stderr)
            status = 2
    return status


if __name__ == "__main__":
    sys.exit(main())
