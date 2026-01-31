#!/bin/bash
#
# Build Release Binary for M5Tab-Macintosh
#
# This script builds the firmware and creates a properly merged binary
# that includes bootloader, partition table, and application - ready
# for single-command flashing with esptool.
#
# Usage:
#   ./scripts/build_release.sh [version]
#
# Examples:
#   ./scripts/build_release.sh           # Creates M5Tab-Macintosh.bin
#   ./scripts/build_release.sh v2.9      # Creates M5Tab-Macintosh-v2.9.bin
#

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/.pio/build/esp32p4_pioarduino"
OUTPUT_DIR="$PROJECT_DIR/release"

# ESP32-P4 flash offsets
BOOTLOADER_OFFSET="0x2000"
PARTITION_OFFSET="0x8000"
APP_OFFSET="0x10000"

# Parse version argument
VERSION="${1:-}"
if [ -n "$VERSION" ]; then
    OUTPUT_NAME="M5Tab-Macintosh-${VERSION}.bin"
else
    OUTPUT_NAME="M5Tab-Macintosh.bin"
fi

echo "========================================"
echo "  M5Tab-Macintosh Release Builder"
echo "========================================"
echo ""

# Change to project directory
cd "$PROJECT_DIR"

# Step 1: Build the firmware
echo "[1/4] Building firmware..."
if ! pio run; then
    echo "ERROR: Build failed!"
    exit 1
fi
echo "      Build successful!"
echo ""

# Step 2: Verify required files exist
echo "[2/4] Verifying build artifacts..."
BOOTLOADER="$BUILD_DIR/bootloader.bin"
PARTITIONS="$BUILD_DIR/partitions.bin"
FIRMWARE="$BUILD_DIR/firmware.bin"

missing_files=0
for f in "$BOOTLOADER" "$PARTITIONS" "$FIRMWARE"; do
    if [ ! -f "$f" ]; then
        echo "      ERROR: Missing $(basename "$f")"
        missing_files=1
    else
        echo "      Found: $(basename "$f") ($(ls -lh "$f" | awk '{print $5}'))"
    fi
done

if [ $missing_files -eq 1 ]; then
    echo "ERROR: Required build artifacts missing!"
    exit 1
fi
echo ""

# Step 3: Create merged binary
echo "[3/4] Creating merged binary..."

# Create output directory
mkdir -p "$OUTPUT_DIR"
OUTPUT_FILE="$OUTPUT_DIR/$OUTPUT_NAME"

# Use esptool to merge binaries
# Note: Using 'esptool' (without .py) to avoid deprecation warning
if command -v esptool &> /dev/null; then
    ESPTOOL_CMD="esptool"
elif command -v esptool.py &> /dev/null; then
    ESPTOOL_CMD="esptool.py"
else
    echo "ERROR: esptool not found. Install with: pip install esptool"
    exit 1
fi

$ESPTOOL_CMD --chip esp32p4 merge-bin \
    --output "$OUTPUT_FILE" \
    --flash-mode qio \
    --flash-freq 80m \
    --flash-size 16MB \
    "$BOOTLOADER_OFFSET" "$BOOTLOADER" \
    "$PARTITION_OFFSET" "$PARTITIONS" \
    "$APP_OFFSET" "$FIRMWARE" 2>&1 | grep -v "^Warning:"

echo "      Created: $OUTPUT_FILE"
echo ""

# Step 4: Verify the merged binary
echo "[4/4] Verifying merged binary..."
HEADER=$(xxd -s 0x2000 -l 4 "$OUTPUT_FILE" | awk '{print $2$3}')
if [ "$HEADER" = "e903004f" ]; then
    echo "      Bootloader header: VALID (0xE9 magic byte)"
else
    echo "      WARNING: Bootloader header may be invalid: $HEADER"
    echo "      Expected: e903004f"
fi

SIZE=$(ls -lh "$OUTPUT_FILE" | awk '{print $5}')
echo "      File size: $SIZE"
echo ""

echo "========================================"
echo "  Release binary created successfully!"
echo "========================================"
echo ""
echo "Output: $OUTPUT_FILE"
echo ""
echo "To flash:"
echo "  esptool --chip esp32p4 --port /dev/cu.usbmodem* \\"
echo "      --baud 921600 write-flash 0x0 $OUTPUT_FILE"
echo ""
