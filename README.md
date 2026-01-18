# BasiliskII ESP32 — Classic Macintosh Emulator for M5Stack Tab5

A full port of the **BasiliskII** Macintosh 68k emulator to the ESP32-P4 microcontroller, running on the M5Stack Tab5 hardware. This project brings classic Mac OS (System 7.x through Mac OS 8.1) to a portable embedded device with touchscreen input and USB peripheral support.


---

## Screenshots

<p align="center">
  <img src="screenshots/Toasters2.5.gif" alt="Flying Toasters v2.5"/>
</p>

*Flying Toasters running smoothly with write-time dirty tracking and tile-based rendering*

<p align="center">
  <img src="screenshots/MacOS8.1_Booted.jpeg" width="45%" alt="Mac OS 8.1 Desktop"/>
  <img src="screenshots/MacOS8.1_About.jpeg" width="45%" alt="Mac OS 8.1 About This Macintosh"/>
</p>

<p align="center">
  <img src="screenshots/MacOS8.1_Booting.jpeg" width="45%" alt="Mac OS 8.1 Booting"/>
  <img src="screenshots/MacOS753_About.jpeg" width="45%" alt="System 7.5.3 About This Macintosh"/>
</p>

<p align="center">
  <a href="screenshots.md">
    <img src="https://img.shields.io/badge/🎬_View_Animations-Boot_Sequence_&_Flying_Toasters-blue?style=for-the-badge" alt="View Animations"/>
  </a>
</p>

---

## Overview

This project runs a **Motorola 68040** emulator that can boot real Macintosh ROMs and run genuine classic Mac OS software. The emulation includes:

- **CPU**: Motorola 68040 emulation with FPU (68881)
- **RAM**: Configurable from 4MB to 16MB (allocated from ESP32-P4's 32MB PSRAM)
- **Display**: 640×360 virtual display (2× scaled to 1280×720 physical display), supporting 1/2/4/8-bit color depths
- **Storage**: Hard disk and CD-ROM images loaded from SD card
- **Input**: Capacitive touchscreen (as mouse) + USB keyboard/mouse support
- **Video**: Optimized pipeline with write-time dirty tracking and tile-based rendering for smooth performance

## Hardware

### [M5Stack Tab5](https://shop.m5stack.com/products/m5stack-tab5-iot-development-kit-esp32-p4)

The Tab5 features a unique **dual-chip architecture** that makes it ideal for this project:

| Chip | Role | Key Features |
|------|------|--------------|
| **ESP32-P4** | Main Application Processor | 400MHz dual-core RISC-V, 32MB PSRAM, MIPI-DSI display |
| **ESP32-C6** | Wireless Co-processor | WiFi 6, Bluetooth LE 5.0 (not used by emulator) |

### Key Specifications

| Component | Details |
|-----------|---------|
| **Display** | 5" IPS TFT, 1280×720 (720p), MIPI-DSI interface |
| **Touch** | Capacitive multi-touch (ST7123 controller) |
| **Memory** | 32MB PSRAM for emulated Mac RAM + frame buffers |
| **Storage** | microSD card slot for ROM, disk images, and settings |
| **USB** | Type-A host port for keyboard/mouse, Type-C for programming |
| **Battery** | NP-F550 Li-ion (2000mAh) for portable operation |

See [boardConfig.md](boardConfig.md) for detailed pin mappings and hardware documentation.

---

## Architecture

### Dual-Core Design

The emulator leverages the ESP32-P4's dual-core RISC-V architecture for optimal performance:

```
┌─────────────────────────────────────────────────────────────────┐
│                        ESP32-P4 (400MHz)                        │
├────────────────────────────┬────────────────────────────────────┤
│         CORE 0             │              CORE 1                │
│    (Video & I/O Core)      │       (CPU Emulation Core)         │
├────────────────────────────┼────────────────────────────────────┤
│  • Video rendering task    │  • 68040 CPU interpreter           │
│  • Dirty tile rendering    │  • Memory access emulation         │
│  • 2×2 pixel scaling       │  • Write-time dirty marking        │
│  • Touch input processing  │  • ROM patching                    │
│  • USB HID polling         │  • Disk I/O                        │
│  • Event-driven refresh    │  • 40,000 instruction quantum      │
└────────────────────────────┴────────────────────────────────────┘
```

### Memory Layout

```
┌──────────────────────────────────────────────────────────────┐
│                    32MB PSRAM Allocation                     │
├──────────────────────────────────────────────────────────────┤
│  Mac RAM (4-16MB)          │  Configurable via Boot GUI      │
├────────────────────────────┼─────────────────────────────────┤
│  Mac ROM (~1MB)            │  Q650.ROM or compatible         │
├────────────────────────────┼─────────────────────────────────┤
│  Mac Frame Buffer (230KB)  │  640×360 @ 8-bit indexed color  │
├────────────────────────────┼─────────────────────────────────┤
│  Snapshot Buffer (230KB)   │  Triple buffering for video     │
├────────────────────────────┼─────────────────────────────────┤
│  Compare Buffer (230KB)    │  Previous frame for fallback    │
├────────────────────────────┼─────────────────────────────────┤
│  Display Buffer (1.8MB)    │  1280×720 @ RGB565              │
├────────────────────────────┼─────────────────────────────────┤
│  Free PSRAM                │  Varies based on RAM selection  │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│                    Internal SRAM (Priority)                  │
├──────────────────────────────────────────────────────────────┤
│  Memory Bank Pointers      │  256KB - hot path optimization  │
├────────────────────────────┼─────────────────────────────────┤
│  Palette (512 bytes)       │  256 RGB565 entries             │
├────────────────────────────┼─────────────────────────────────┤
│  Dirty Tile Bitmap         │  144 bits (write-time tracking) │
└──────────────────────────────────────────────────────────────┘
```

### Video Pipeline

The video system uses a highly optimized pipeline with **write-time dirty tracking** to minimize CPU overhead:

```
┌─────────────────────────────────────────────────────────────────┐
│                    Video Pipeline Architecture                   │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────┐    marks dirty    ┌─────────────────────────┐ │
│  │  68040 CPU   │ ─────────────────▶│   Dirty Tile Bitmap     │ │
│  │  (Core 1)    │                   │   (16×9 = 144 tiles)    │ │
│  └──────────────┘                   └─────────────────────────┘ │
│         │                                      │                │
│         │ writes                               │ read & clear   │
│         ▼                                      ▼                │
│  ┌──────────────┐                   ┌─────────────────────────┐ │
│  │ Mac Frame    │                   │    Video Task (Core 0)  │ │
│  │   Buffer     │ ─────────────────▶│  • Tile snapshot        │ │
│  │ (640×360)    │   read tiles      │  • Palette lookup       │ │
│  └──────────────┘                   │  • 2×2 scaling          │ │
│                                     └─────────────────────────┘ │
│                                                │                │
│                                                │ push tiles     │
│                                                ▼                │
│                                     ┌─────────────────────────┐ │
│                                     │   MIPI-DSI Display      │ │
│                                     │      (1280×720)         │ │
│                                     └─────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

#### Key Features

1. **Write-Time Dirty Tracking**: When the 68040 CPU writes to the framebuffer, the memory system immediately marks the affected tile(s) as dirty. This eliminates expensive per-frame comparisons.

2. **Tile-Based Rendering**: The screen is divided into a 16×9 grid of 40×40 pixel tiles (144 total). Only dirty tiles are re-rendered each frame, typically reducing video CPU time by 60-90%.

3. **Triple Buffering**: Ensures race-free operation between CPU writes and video reads:
   - `mac_frame_buffer`: CPU writes here
   - `snapshot_buffer`: Atomic copy for rendering
   - `compare_buffer`: Previous frame for fallback comparison

4. **Multi-Depth Support**: Supports 1/2/4/8-bit indexed color modes with packed pixel decoding. Mac OS can switch between depths via the Monitors control panel.

5. **Event-Driven Refresh**: The video task sleeps until signaled by the CPU via FreeRTOS task notifications, reducing idle polling overhead.

---

## Emulation Details

### BasiliskII Components

This port includes the following BasiliskII subsystems, adapted for ESP32:

| Component | File(s) | Description |
|-----------|---------|-------------|
| **UAE CPU** | `uae_cpu/*.cpp` | Motorola 68040 interpreter |
| **Memory** | `uae_cpu/memory.cpp` | Memory banking with write-time dirty tracking |
| **ADB** | `adb.cpp` | Apple Desktop Bus for keyboard/mouse |
| **Video** | `video_esp32.cpp` | Tile-based display driver with 2× scaling |
| **Disk** | `disk.cpp`, `sys_esp32.cpp` | HDD image support via SD card |
| **CD-ROM** | `cdrom.cpp` | ISO image mounting |
| **XPRAM** | `xpram_esp32.cpp` | Non-volatile parameter RAM |
| **Timer** | `timer_esp32.cpp` | 60Hz/1Hz tick generation |
| **ROM Patches** | `rom_patches.cpp` | Compatibility patches for ROMs |
| **Input** | `input_esp32.cpp` | Touch + USB HID handling |

### Supported ROMs

The emulator works best with **Macintosh Quadra** series ROMs:

| ROM File | Machine | Recommended |
|----------|---------|-------------|
| `Q650.ROM` | Quadra 650 | ✅ Best compatibility |
| `Q700.ROM` | Quadra 700 | ✅ Good |
| `Q800.ROM` | Quadra 800 | ✅ Good |
| `68030-IIci.ROM` | Mac IIci | ⚠️ May work |

### Supported Operating Systems

| OS Version | Status | Notes |
|------------|--------|-------|
| System 7.1 | ✅ Works | Lightweight, fast boot |
| System 7.5.x | ✅ Works | Good compatibility |
| Mac OS 8.0 | ✅ Works | Full-featured |
| Mac OS 8.1 | ✅ Works | Latest supported |

---

## Getting Started

### Prerequisites

- **Hardware**: M5Stack Tab5
- **Software**: [PlatformIO](https://platformio.org/) (CLI or IDE extension)
- **SD Card**: FAT32 formatted microSD card (8GB+ recommended)

### SD Card Setup

#### Quick Start (Recommended)

Download a ready-to-use SD card image with Mac OS pre-installed:

**[📥 Download sdCard.zip](https://mcchord.net/static/sdCard.zip)**

1. Format your microSD card as **FAT32**
2. Extract the ZIP contents to the **root** of the SD card
3. Insert into Tab5 and boot

#### Manual Setup

Alternatively, create your own setup with these files in the SD card root:

```
/
├── Q650.ROM              # Macintosh Quadra ROM (required)
├── Macintosh.dsk         # Hard disk image (required)
├── System753.iso         # Mac OS installer CD (optional)
└── DiskTools1.img        # Boot floppy for installation (optional)
```

To create a blank disk image:

```bash
# Create a 500MB blank disk image
dd if=/dev/zero of=Macintosh.dsk bs=1M count=500
```

Then format it during Mac OS installation.

### Flashing the Firmware

#### Option 1: Pre-built Firmware (Easiest)

Download the latest release from GitHub:

**[📥 Download Latest Release](https://github.com/amcchord/M5Tab-Macintosh/releases/latest)**

Flash the single merged binary using `esptool.py`:

```bash
# Install esptool if you don't have it
pip install esptool

# Flash the merged binary (connect Tab5 via USB-C)
esptool.py --chip esp32p4 \
    --port /dev/ttyACM0 \
    --baud 921600 \
    write_flash \
    0x0 M5Tab-Macintosh-v2.0.1.bin
```

**Note**: Replace `/dev/ttyACM0` with your actual port:
- **macOS**: `/dev/cu.usbmodem*` or `/dev/tty.usbmodem*`  
  (Run `ls /dev/cu.*` to find available ports)
- **Windows**: `COM3` (or similar, check Device Manager)
- **Linux**: `/dev/ttyACM0` or `/dev/ttyUSB0`

**Troubleshooting Flash Issues**:
- If flashing fails, try a lower baud rate: `--baud 460800` or `--baud 115200`
- If the device isn't detected, hold the **BOOT** button while pressing **RESET** to enter bootloader mode
- On some systems you may need to run with `sudo` or add your user to the `dialout` group

#### Option 2: Build from Source

```bash
# Clone the repository
git clone https://github.com/amcchord/M5Tab-Macintosh.git
cd M5Tab-Macintosh

# Build the firmware
pio run

# Upload to device (connect via USB-C)
pio run --target upload

# Monitor serial output
pio device monitor
```

---

## Boot GUI

On startup, a **classic Mac-style boot configuration screen** appears:

```
┌─────────────────────────────────────────┐
│           BasiliskII                    │
│        Starting in 3...                 │
│                                         │
│    Disk: Macintosh.dsk                  │
│    RAM: 8 MB                            │
│                                         │
│  ┌─────────────────────────────────┐    │
│  │       Change Settings           │    │
│  └─────────────────────────────────┘    │
└─────────────────────────────────────────┘
```

### Features

- **3-second countdown** to auto-boot with saved settings
- **Tap to configure** disk images, CD-ROMs, and RAM size
- **Settings persistence** saved to `/basilisk_settings.txt` on SD card
- **Touch-friendly** large buttons designed for the 5" touchscreen

### Configuration Options

| Setting | Options | Default |
|---------|---------|---------|
| Hard Disk | Any `.dsk` or `.img` file on SD root | First found |
| CD-ROM | Any `.iso` file on SD root, or None | None |
| RAM Size | 4 MB, 8 MB, 12 MB, 16 MB | 8 MB |

---

## Input Support

### Touch Screen

The capacitive touchscreen acts as a single-button mouse:

- **Tap** = Click
- **Drag** = Click and drag
- Coordinates are scaled from 1280×720 display to 640×360 Mac screen

### USB Keyboard

Connect a USB keyboard to the **USB Type-A port**. Supported features:

- Full QWERTY layout with proper Mac key mapping
- Modifier keys: Command (⌘), Option (⌥), Control, Shift
- Function keys F1-F15
- Arrow keys and navigation cluster
- Numeric keypad
- **Caps Lock LED** sync with Mac OS

### USB Mouse

Connect a USB mouse for relative movement input:

- Left, right, and middle button support
- Relative movement mode (vs. absolute for touch)

---

## Project Structure

```
M5Tab-Macintosh/
├── src/
│   ├── main.cpp                    # Application entry point
│   └── basilisk/                   # BasiliskII emulator core
│       ├── main_esp32.cpp          # Emulator initialization & main loop
│       ├── video_esp32.cpp         # Tile-based display driver with dirty tracking
│       ├── input_esp32.cpp         # Touch + USB HID input handling
│       ├── boot_gui.cpp            # Pre-boot configuration GUI
│       ├── sys_esp32.cpp           # SD card disk I/O
│       ├── timer_esp32.cpp         # 60Hz/1Hz interrupt generation
│       ├── xpram_esp32.cpp         # NVRAM persistence to SD
│       ├── prefs_esp32.cpp         # Preferences loading
│       ├── uae_cpu/                # Motorola 68040 CPU emulator
│       │   ├── newcpu.cpp          # Main CPU interpreter loop
│       │   ├── memory.cpp          # Memory banking with write-time dirty tracking
│       │   ├── fpu/                # FPU emulation (IEEE)
│       │   └── generated/          # CPU instruction tables
│       └── include/                # Header files
├── platformio.ini                  # PlatformIO build configuration
├── partitions.csv                  # ESP32 flash partition table
├── boardConfig.md                  # Hardware documentation
├── screenshots/                    # Demo images and videos
└── scripts/                        # Build helper scripts
```

---

## Performance

### Benchmarks (Approximate)

| Metric | Value |
|--------|-------|
| CPU Quantum | 40,000 instructions per tick |
| Video Refresh | ~15 FPS target |
| Boot Time | ~15 seconds to Mac OS desktop |
| Typical Dirty Tiles | 5-15 tiles/frame (vs. 144 total) |
| Video CPU Savings | 60-90% reduction with dirty tracking |

### Optimization Techniques

1. **Write-Time Dirty Tracking**: Marks tiles dirty at CPU write time, avoiding expensive per-frame comparisons. Dirty bitmap uses atomic operations for thread safety.

2. **Dual-Core Separation**: CPU emulation (Core 1) and video rendering (Core 0) run independently with minimal synchronization.

3. **Memory Bank Placement**: The 256KB memory bank pointer array is allocated in internal SRAM when possible (faster than PSRAM for this hot path).

4. **4-Pixel Batch Processing**: Reads 4 pixels as a 32-bit word, converts through palette, and writes 8 scaled pixels in one pass.

5. **Tile Threshold Fallback**: If >80% of tiles are dirty, does a full frame update instead (reduces per-tile API overhead).

6. **Event-Driven Video Task**: Uses FreeRTOS task notifications instead of polling, waking only when work is needed.

7. **Aggressive Compiler Optimizations**: Build uses `-O3`, `-funroll-loops`, `-ffast-math`, and aggressive inlining for hot paths.

---

## Build Configuration

Key build flags in `platformio.ini`:

```ini
build_flags =
    -O3                          # Maximum optimization
    -funroll-loops               # Unroll loops for speed
    -ffast-math                  # Fast floating point
    -finline-functions           # Aggressive inlining
    -DEMULATED_68K=1             # Use 68k interpreter
    -DREAL_ADDRESSING=0          # Use memory banking
    -DSAVE_MEMORY_BANKS=1        # Dynamic bank allocation
    -DROM_IS_WRITE_PROTECTED=1   # Protect ROM from writes
    -DFPU_IEEE=1                 # IEEE FPU emulation
```

---

## Troubleshooting

### Common Issues

| Problem | Solution |
|---------|----------|
| "SD card initialization failed" | Ensure SD card is FAT32, properly seated |
| "Q650.ROM not found" | Place ROM file in SD card root directory |
| Black screen after boot | Check serial output for errors; verify ROM compatibility |
| Touch not responding | Wait for boot GUI to complete initialization |
| USB keyboard not working | Connect to Type-A port (not Type-C) |
| Slow/choppy display | Check serial for `[VIDEO PERF]` stats; typical is 60-90% partial updates |
| Screen flickering/tearing | May occur during heavy graphics; dirty tracking minimizes this |

### Serial Debug Output

Connect via USB-C and use:

```bash
pio device monitor
```

Look for initialization messages:

```
========================================
  BasiliskII ESP32 - Macintosh Emulator
  M5Stack Tab5 Edition
========================================

[MAIN] Display: 1280x720
[MAIN] Free heap: 473732 bytes
[MAIN] Free PSRAM: 31676812 bytes
[MAIN] Total PSRAM: 33554432 bytes
[MAIN] CPU Freq: 360 MHz
[VIDEO] Display size: 1280x720
[VIDEO] Mac frame buffer allocated: 0x48100000 (230400 bytes)
[VIDEO] Triple buffers allocated: snapshot, compare (230400 bytes each)
[VIDEO] Dirty tracking: 16x9 tiles (144 total), threshold 80%
[VIDEO] Video task created on Core 0 (write-time dirty tracking)
```

During operation, video performance is reported every 5 seconds:

```
[VIDEO PERF] frames=75 (full=2 partial=68 skip=5)
[VIDEO PERF] avg: snapshot=0us detect=45us render=8234us push=2156us
```

---

## Acknowledgments

- **BasiliskII** by Christian Bauer and contributors — the original open-source 68k Mac emulator
- **UAE** (Unix Amiga Emulator) — the CPU emulation core
- **[M5Stack](https://shop.m5stack.com/products/m5stack-tab5-iot-development-kit-esp32-p4)** — for the excellent Tab5 hardware and M5Unified/M5GFX libraries
- **EspUsbHost** — USB HID support for ESP32
- **Claude Opus 4.5** (Anthropic) — AI pair programmer that made this port possible

---

## License

This project is based on BasiliskII, which is licensed under the **GNU General Public License v2**.

---



*This project was built with the assistance of [Claude Opus 4.5](https://anthropic.com). I am in no way smart enough to have done this on my own.* 🤖🍎

*Run classic Mac OS in your pocket.*
