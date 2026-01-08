# M5Stack Tab5 Board Configuration

## Overview

The M5Stack Tab5 (SKU: C145/K145) is a highly expandable, portable smart-IoT terminal development device featuring a **dual-chip architecture**.

## Dual-Chip Architecture

The Tab5 uses two separate ESP32 chips that communicate internally:

| Chip | Role | Key Features |
|------|------|--------------|
| **ESP32-P4** | Main Application Processor | 400MHz dual-core RISC-V, 32MB PSRAM, no native WiFi/Bluetooth |
| **ESP32-C6** | Wireless Co-processor | WiFi 6 (2.4GHz), Bluetooth LE 5.0, Thread/Zigbee capable |

The ESP32-P4 handles all application logic, display rendering, and peripheral control. WiFi and Bluetooth operations are delegated to the ESP32-C6, which communicates with the P4 via an internal bus. **M5Unified abstracts this complexity**, allowing standard `WiFi.h` and `Bluetooth.h` usage in your code.

## Display

- **Size**: 5 inches
- **Resolution**: 1280 x 720 (720p)
- **Type**: IPS TFT
- **Interface**: MIPI-DSI
- **Touch**: Capacitive touch (ST7123 controller)
- **Library**: M5GFX / M5Unified

## Audio System

| Component | Chip | Function |
|-----------|------|----------|
| Audio Codec | ES8388 | Hi-Fi recording/playback |
| AEC Frontend | ES7210 | Acoustic echo cancellation, dual microphones |
| Amplifier | NS4150B | 1W speaker output |
| Headphone | 3.5mm jack | Stereo output |

## Sensors

| Sensor | Chip | Function |
|--------|------|----------|
| IMU | BMI270 | 6-axis accelerometer + gyroscope |
| RTC | RX8130CE | Real-time clock with timed interrupt wake-up |
| Power Monitor | INA226 | Battery voltage/current monitoring |

## Connectivity & Expansion

### USB Ports
- **USB Type-A**: Host mode (for keyboard, mouse, USB devices)
- **USB Type-C**: USB 2.0 OTG (programming, data transfer)

### Industrial Interfaces
- **RS-485**: SIT3088 transceiver with switchable 120Ω terminator

### Expansion
- **HY2.0-4P**: I2C expansion port (Grove compatible)
- **M5-Bus**: Rear expansion connector for M5Stack modules
- **Stamp Expansion Pad**: For M5Stamp modules
- **microSD**: Card slot for storage

### Antenna
- Built-in 3D antenna
- MMCX external antenna port
- Software-switchable via `RF_PTH_L_INT_H_EXT` (LOW = internal, HIGH = external)

## Pin Mappings

### I2C Bus (Shared)
| Signal | GPIO |
|--------|------|
| SDA | GPIO7 |
| SCL | GPIO8 |

**I2C Addresses:**
- BMI270: 0x68
- RX8130CE (RTC): 0x32
- INA226: 0x40
- ES8388: 0x10
- ES7210: 0x41
- Touch (ST7123): 0x55
- PI4IOE5V6408 (GPIO Expander): 0x43

### ESP32-C6 Communication
| Signal | GPIO |
|--------|------|
| P4 TX → C6 RX | GPIO25 |
| P4 RX ← C6 TX | GPIO26 |
| WLAN_PWR_EN | Via GPIO Expander |

### RS-485
| Signal | GPIO |
|--------|------|
| TX | GPIO24 |
| RX | GPIO23 |
| DE/RE | GPIO22 |

### HY2.0-4P (Grove Port)
| Signal | GPIO |
|--------|------|
| SDA | GPIO7 |
| SCL | GPIO8 |
| VCC | 5V (controlled by EXT5V_EN) |
| GND | GND |

### microSD Card
| Signal | GPIO |
|--------|------|
| CLK | GPIO39 |
| CMD | GPIO44 |
| DATA0 | GPIO40 |
| DATA1 | GPIO41 |
| DATA2 | GPIO42 |
| DATA3 | GPIO43 |

### Camera (SC2356 2MP)
| Signal | GPIO |
|--------|------|
| PCLK | GPIO47 |
| VSYNC | GPIO46 |
| HSYNC | GPIO45 |
| D0-D7 | GPIO13-GPIO20 |
| XCLK | GPIO48 |
| PWDN | Via GPIO Expander |
| RESET | Via GPIO Expander |

## Power System

- **Battery**: Removable NP-F550 Li-ion (2000mAh in Kit version)
- **Charging**: IP2326 charge management IC
- **Power Rail**: MP4560 buck-boost converter
- **Monitoring**: INA226 for voltage/current sensing

### Power Control
- **Power Button**: Short press = on/off, Long press = force off
- **Download Mode**: Hold BOOT button while pressing RESET

## PlatformIO Configuration

```ini
[env:esp32p4_pioarduino]
platform = https://github.com/pioarduino/platform-espressif32.git#54.03.21
upload_speed = 1500000
monitor_speed = 115200
build_type = debug
framework = arduino
board = esp32-p4-evboard
board_build.mcu = esp32p4
board_build.flash_mode = qio
build_flags =
    -DBOARD_HAS_PSRAM
    -DCORE_DEBUG_LEVEL=5
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DARDUINO_USB_MODE=1
lib_deps =
    https://github.com/M5Stack/M5Unified.git
    https://github.com/M5Stack/M5GFX.git
```

## Development Notes

### WiFi Usage
The ESP32-P4 has no native WiFi - it communicates with the ESP32-C6 via SDIO. **You MUST call `WiFi.setPins()` before any WiFi operations!**

```cpp
#include <M5Unified.h>
#include <WiFi.h>

// Tab5 WiFi SDIO pins for ESP32-C6 communication
#define SDIO2_CLK GPIO_NUM_12
#define SDIO2_CMD GPIO_NUM_13
#define SDIO2_D0  GPIO_NUM_11
#define SDIO2_D1  GPIO_NUM_10
#define SDIO2_D2  GPIO_NUM_9
#define SDIO2_D3  GPIO_NUM_8
#define SDIO2_RST GPIO_NUM_15

void setup() {
    M5.begin();
    
    // CRITICAL: Set WiFi pins BEFORE any WiFi operations
    WiFi.setPins(SDIO2_CLK, SDIO2_CMD, SDIO2_D0, SDIO2_D1, SDIO2_D2, SDIO2_D3, SDIO2_RST);
    
    WiFi.begin("SSID", "password");
}
```

Reference: [Tab5 WiFi Documentation](https://docs.m5stack.com/en/arduino/m5tab5/wifi)

### Display Usage
M5Unified provides a unified display API:

```cpp
#include <M5Unified.h>

M5.begin();
M5.Display.setTextSize(2);
M5.Display.println("Hello World");
```

### Important Build Flags
- `BOARD_HAS_PSRAM`: Enables 32MB PSRAM
- `ARDUINO_USB_CDC_ON_BOOT=1`: USB serial via Type-C
- `ARDUINO_USB_MODE=1`: USB device mode

## Resources

- [Official Documentation](https://docs.m5stack.com/en/core/Tab5)
- [Tab5 Arduino Quick Start](https://docs.m5stack.com/en/arduino/m5tab5/program)
- [M5Unified Library](https://github.com/M5Stack/M5Unified)
- [M5GFX Library](https://github.com/M5Stack/M5GFX)
- [Tab5 Schematics PDF](https://docs.m5stack.com/en/core/Tab5#schematic)
