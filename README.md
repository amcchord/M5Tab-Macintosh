# BigDashVesc

A VESC dashboard application for the M5Stack Tab5, featuring a 720p touchscreen display with real-time telemetry visualization and a web-based remote dashboard.

## Features

- **720p Dashboard Display** - Full-color UI optimized for the Tab5's 1280x720 IPS display
- **BLE VESC Connection** - Connects to VESC controllers via Bluetooth Low Energy
- **WiFi Web Dashboard** - Remote monitoring through a web interface with WebSocket real-time updates
- **Auto-Reconnect** - Automatically reconnects to previously paired VESC devices
- **Persistent Storage** - Saves settings and last connected device to NVS flash

## Hardware

### M5Stack Tab5

The Tab5 features a dual-chip architecture:

| Chip | Role | Key Features |
|------|------|--------------|
| **ESP32-P4** | Main Application Processor | 400MHz dual-core RISC-V, 32MB PSRAM |
| **ESP32-C6** | Wireless Co-processor | WiFi 6, Bluetooth LE 5.0 |

See [boardConfig.md](boardConfig.md) for detailed hardware specifications and pin mappings.

## Building

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or IDE extension)
- USB-C cable for programming

### Build & Upload

```bash
# Build the project
pio run

# Upload to device
pio run --target upload

# Monitor serial output
pio device monitor
```

### Filesystem Upload

The web dashboard files are stored on LittleFS. Upload them with:

```bash
pio run --target uploadfs
```

## Project Structure

```
├── src/
│   ├── main.cpp           # Application entry point and state machine
│   ├── config.h           # Configuration constants
│   ├── ble_vesc.*         # BLE VESC communication
│   ├── vesc_protocol.*    # VESC packet encoding/decoding
│   ├── wifi_manager.*     # WiFi connection management
│   ├── web_server.*       # HTTP server and WebSocket handling
│   ├── storage.*          # NVS persistent storage
│   └── ui/
│       ├── ui_init.*      # Display initialization
│       └── ui_dashboard.* # Dashboard rendering
├── data/                  # Web dashboard files (LittleFS)
│   ├── index.html
│   ├── css/style.css
│   └── js/app.js
├── scripts/               # Build scripts
├── platformio.ini         # PlatformIO configuration
└── partitions.csv         # Custom partition table
```

## Web Dashboard

Once connected to WiFi, access the web dashboard at the device's IP address. The dashboard displays:

- Real-time speed and power metrics
- Battery voltage and current
- Motor temperature
- Trip statistics

## Configuration

Edit `src/config.h` to customize:

- WiFi credentials
- Display refresh rates
- BLE scan parameters
- VESC communication intervals

## License

MIT License
