#!/bin/bash
# Generate CPU opcode tables for BasiliskII UAE 68k emulator

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

OUTPUT_DIR="$SCRIPT_DIR/../../src/basilisk/uae_cpu/generated"
mkdir -p "$OUTPUT_DIR"

echo "=== Generating CPU Tables for BasiliskII ==="
echo "Output directory: $OUTPUT_DIR"

# Step 1: Compile build68k (only needs the header, not readcpu.cpp)
echo ""
echo "Step 1: Compiling build68k..."
cc -I. -o build68k build68k.c
echo "  Done."

# Step 2: Generate cpudefs.cpp using build68k
echo ""
echo "Step 2: Generating cpudefs.cpp..."
./build68k < table68k > "$OUTPUT_DIR/cpudefs.cpp"
echo "  Done: $OUTPUT_DIR/cpudefs.cpp"

# Step 3: Compile gencpu (needs readcpu.cpp AND cpudefs.cpp)
echo ""
echo "Step 3: Compiling gencpu..."
cc -I. -I"$OUTPUT_DIR" -o gencpu gencpu.c readcpu.cpp "$OUTPUT_DIR/cpudefs.cpp" -lstdc++
echo "  Done."

# Step 4: Generate CPU emulation files
echo ""
echo "Step 4: Generating CPU emulation files..."
cd "$OUTPUT_DIR"
"$SCRIPT_DIR/gencpu"
echo "  Done."

# Step 5: Add ESP32 PSRAM attributes to large tables
echo ""
echo "Step 5: Adding PSRAM attributes for ESP32..."
# Add PSRAM attribute to cpustbl.cpp tables
sed -i '' 's/^struct cputbl CPUFUNC/EXT_RAM_ATTR struct cputbl CPUFUNC/g' cpustbl.cpp
# Add the ESP32 attribute definition at the top of cpustbl.cpp
sed -i '' '1i\
#ifdef ARDUINO\
#define EXT_RAM_ATTR __attribute__((section(".ext_ram.data")))\
#else\
#define EXT_RAM_ATTR\
#endif\
' cpustbl.cpp
echo "  Done."

# List generated files
echo ""
echo "=== Generated files ==="
ls -la "$OUTPUT_DIR"

echo ""
echo "=== CPU table generation complete! ==="
