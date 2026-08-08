#!/bin/bash
#
# Build Release Binaries for BasiliskII ESP32
#
# Builds firmware for one or both supported boards and produces merged
# single-file binaries that include bootloader + partition table +
# application, ready to be flashed via esptool in one command.
#
# Usage:
#   ./scripts/build_release.sh [version] [env]
#
# Examples:
#   ./scripts/build_release.sh                  # both boards, release/M5Tab-Macintosh.bin and release/M5Tab-Macintosh-Waveshare-P4-10.1.bin
#   ./scripts/build_release.sh v3.2             # both boards, versioned filenames
#   ./scripts/build_release.sh v3.2 tab5        # only M5Stack Tab5
#   ./scripts/build_release.sh v3.2 waveshare   # only Waveshare P4 10.1
#
# Board shortcuts recognized for the env argument:
#   tab5, m5tab5, esp32p4_pioarduino -> env:esp32p4_pioarduino
#   waveshare, waveshare_p4_101, ws  -> env:waveshare_p4_101
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
OUTPUT_DIR="$PROJECT_DIR/release"

# ESP32-P4 flash offsets
BOOTLOADER_OFFSET="0x2000"
PARTITION_OFFSET="0x8000"
APP_OFFSET="0x10000"

VERSION="${1:-}"
ENV_ARG="${2:-all}"

# Normalize env selection
case "$ENV_ARG" in
    tab5|m5tab5|esp32p4_pioarduino)           ENVS=("esp32p4_pioarduino") ;;
    waveshare|waveshare_p4_101|ws|waveshare101) ENVS=("waveshare_p4_101") ;;
    all|both|"")                                ENVS=("esp32p4_pioarduino" "waveshare_p4_101") ;;
    *) echo "ERROR: unknown env '$ENV_ARG'. Use tab5, waveshare, or all."; exit 1 ;;
esac

# Per-env output filename prefixes
name_for_env() {
    case "$1" in
        esp32p4_pioarduino)  echo "M5Tab-Macintosh" ;;
        waveshare_p4_101)    echo "M5Tab-Macintosh-Waveshare-P4-10.1" ;;
        *) echo "$1" ;;
    esac
}

# Ensure PlatformIO's virtual environment is on PATH in fresh shells. It
# normally provides both pio and esptool, so do this before locating either.
if [ -x "$HOME/.platformio/penv/bin/pio" ]; then
    export PATH="$HOME/.platformio/penv/bin:$PATH"
fi
if ! command -v pio &>/dev/null; then
    echo "ERROR: pio not found. Is PlatformIO installed?"
    exit 1
fi

# Locate esptool after adding PlatformIO's virtual environment to PATH.
if command -v esptool &>/dev/null; then
    ESPTOOL_CMD="esptool"
elif command -v esptool.py &>/dev/null; then
    ESPTOOL_CMD="esptool.py"
else
    echo "ERROR: esptool not found. Install with: pip install esptool"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
PRODUCED=()

echo "========================================"
echo "  BasiliskII ESP32 - Release Builder"
echo "  Version: ${VERSION:-(none)}"
echo "  Envs:    ${ENVS[*]}"
echo "========================================"
echo ""

cd "$PROJECT_DIR"

for env in "${ENVS[@]}"; do
    echo "----------------------------------------"
    echo "  Building env: $env"
    echo "----------------------------------------"
    BUILD_DIR="$PROJECT_DIR/.pio/build/$env"
    BOOTLOADER="$BUILD_DIR/bootloader.bin"
    PARTITIONS="$BUILD_DIR/partitions.bin"
    FIRMWARE="$BUILD_DIR/firmware.bin"

    echo "[1/3] pio run -e $env"
    if ! pio run -e "$env"; then
        echo "ERROR: Build failed for env $env"
        exit 1
    fi

    echo ""
    echo "[2/3] Verifying artifacts..."
    missing=0
    for f in "$BOOTLOADER" "$PARTITIONS" "$FIRMWARE"; do
        if [ ! -f "$f" ]; then
            echo "      MISSING: $(basename "$f")"
            missing=1
        else
            echo "      OK:      $(basename "$f") ($(ls -lh "$f" | awk '{print $5}'))"
        fi
    done
    if [ $missing -ne 0 ]; then
        echo "ERROR: artifacts missing"; exit 1
    fi

    name_prefix="$(name_for_env "$env")"
    if [ -n "$VERSION" ]; then
        OUTPUT_NAME="${name_prefix}-${VERSION}.bin"
    else
        OUTPUT_NAME="${name_prefix}.bin"
    fi
    OUTPUT_FILE="$OUTPUT_DIR/$OUTPUT_NAME"

    echo ""
    echo "[3/3] Merging to $OUTPUT_NAME"
    $ESPTOOL_CMD --chip esp32p4 merge-bin \
        --output "$OUTPUT_FILE" \
        --flash-mode qio \
        --flash-freq 80m \
        --flash-size 16MB \
        "$BOOTLOADER_OFFSET" "$BOOTLOADER" \
        "$PARTITION_OFFSET"  "$PARTITIONS" \
        "$APP_OFFSET"        "$FIRMWARE" 2>&1 | grep -v "^Warning:" || true

    # Validate bootloader magic byte at +0x2000 in the merged image
    HEADER=$(xxd -s 0x2000 -l 4 "$OUTPUT_FILE" 2>/dev/null | awk '{print $2$3}')
    if [ "$HEADER" = "e903004f" ]; then
        echo "      Bootloader header: VALID (0xE9 magic byte)"
    else
        echo "      WARNING: bootloader header mismatch ($HEADER; expected e903004f)"
    fi
    SIZE=$(ls -lh "$OUTPUT_FILE" | awk '{print $5}')
    echo "      File size: $SIZE"
    echo ""
    PRODUCED+=("$OUTPUT_FILE")
done

echo "========================================"
echo "  Release build complete"
echo "========================================"
for f in "${PRODUCED[@]}"; do
    echo "  $f"
done
echo ""
echo "Flash with, e.g.:"
echo "  esptool --chip esp32p4 --port /dev/cu.usbmodem* \\"
echo "          --baud 921600 write-flash 0x0 ${PRODUCED[0]}"
echo ""
