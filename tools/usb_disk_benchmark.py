#!/usr/bin/env python3
"""Time a USB Disk file copy and verify it after the host ejects and remounts the card."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import time

BLOCK = 1024 * 1024


def checksum(path):
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while block := source.read(BLOCK):
            size += len(block)
            digest.update(block)
    return size, digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("write", "read"))
    parser.add_argument("reference", type=Path, help="existing test payload on the host")
    parser.add_argument("card_file", type=Path, help="regular file on the mounted SD volume")
    args = parser.parse_args()
    if not args.reference.is_file() or not args.reference.stat().st_size:
        parser.error("reference must be a nonempty regular file")
    if args.action == "read" and (not args.card_file.is_file() or args.reference.samefile(args.card_file)):
        parser.error("read requires a separate regular file on the card")

    expected_size, expected_hash = checksum(args.reference)
    start = time.monotonic()
    if args.action == "write":
        with args.reference.open("rb") as source, args.card_file.open("xb") as destination:
            shutil.copyfileobj(source, destination, BLOCK)
            destination.flush()
            os.fsync(destination.fileno())
        size = args.card_file.stat().st_size
        observed_hash = None  # Reading it now would use the host's cache.
    else:
        size, observed_hash = checksum(args.card_file)
    seconds = time.monotonic() - start
    print(json.dumps({
        "operation": args.action, "bytes": size, "seconds": seconds,
        "MiB_per_second": size / (1024 * 1024 * seconds),
        "reference_sha256": expected_hash, "read_sha256": observed_hash,
    }, indent=2))
    return int(size != expected_size or (args.action == "read" and observed_hash != expected_hash))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OSError as error:
        sys.exit(str(error))
