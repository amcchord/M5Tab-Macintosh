/**
 * @file config.h
 * BigDashVesc Configuration
 * M5Stack Tab5 VESC Dashboard
 */

#ifndef CONFIG_H
#define CONFIG_H

// ============================================================================
// Tab5 Hardware Configuration
// ============================================================================

// WiFi SDIO pins for ESP32-C6 communication
#define SDIO2_CLK GPIO_NUM_12
#define SDIO2_CMD GPIO_NUM_13
#define SDIO2_D0  GPIO_NUM_11
#define SDIO2_D1  GPIO_NUM_10
#define SDIO2_D2  GPIO_NUM_9
#define SDIO2_D3  GPIO_NUM_8
#define SDIO2_RST GPIO_NUM_15

// Display dimensions
#define SCREEN_WIDTH  1280
#define SCREEN_HEIGHT 720

// ============================================================================
// WiFi Configuration
// ============================================================================

struct WiFiNetwork {
    const char* ssid;
    const char* password;
};

// Known networks (defined in wifi_manager.cpp)
extern const WiFiNetwork KNOWN_NETWORKS[];
extern const int KNOWN_NETWORKS_COUNT;

// WiFi connection settings
#define WIFI_CONNECT_TIMEOUT_MS     10000
#define WIFI_RETRY_DELAY_MS         2000

// Access Point settings (fallback when no known network found)
#define AP_PASSWORD                 "vescdash123"
#define AP_CHANNEL                  1
#define AP_MAX_CONNECTIONS          4

// ============================================================================
// BLE Configuration
// ============================================================================

#define BLE_SCAN_TIME_SECONDS       5
#define BLE_SCAN_INTERVAL           100
#define BLE_SCAN_WINDOW             99

// Nordic UART Service UUIDs
#define NUS_SERVICE_UUID        "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_CHAR_UUID        "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_CHAR_UUID        "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// ============================================================================
// VESC Communication Configuration
// ============================================================================

#define VESC_DATA_REFRESH_MS        100
#define VESC_DATA_STALE_TIMEOUT_MS  3000
#define VESC_RECONNECT_INTERVAL_MS  5000
#define VESC_CONNECTION_GRACE_MS    10000
#define VESC_MAX_RECONNECT_ATTEMPTS 3

// VESC UART Protocol Constants
#define VESC_PACKET_START_SHORT     0x02
#define VESC_PACKET_START_LONG      0x03
#define VESC_PACKET_STOP            0x03

// VESC Command IDs
#define COMM_FW_VERSION             0
#define COMM_GET_VALUES             4
#define COMM_SET_DUTY               5
#define COMM_SET_CURRENT            6
#define COMM_SET_CURRENT_BRAKE      7
#define COMM_SET_RPM                8
#define COMM_SET_MCCONF             13
#define COMM_GET_MCCONF             14
#define COMM_GET_MCCONF_DEFAULT     15
#define COMM_SET_APPCONF            16
#define COMM_GET_APPCONF            17
#define COMM_GET_APPCONF_DEFAULT    18
#define COMM_REBOOT                 29
#define COMM_ALIVE                  30
#define COMM_GET_DECODED_PPM        55
#define COMM_GET_DECODED_ADC        56

// ============================================================================
// Display Configuration
// ============================================================================

#define VOLTAGE_UPDATE_THRESHOLD    0.05f
#define CURRENT_UPDATE_THRESHOLD    0.1f
#define TEMP_UPDATE_THRESHOLD       0.5f
#define BATTERY_UPDATE_THRESHOLD    1

// Temperature warning thresholds (in Celsius)
#define FET_TEMP_WARNING_C          80.0f
#define FET_TEMP_CRITICAL_C         90.0f
#define MOTOR_TEMP_WARNING_C        100.0f

// Battery estimation (Li-ion)
#define CELL_VOLTAGE_FULL           4.2f
#define CELL_VOLTAGE_NOMINAL        3.7f
#define CELL_VOLTAGE_EMPTY          3.0f
#define CELL_VOLTAGE_ESTIMATE       4.0f

// ============================================================================
// Web Server Configuration
// ============================================================================

#define WEB_SERVER_PORT             80
#define WEBSOCKET_PATH              "/ws"
#define WEBSOCKET_PING_INTERVAL_MS  5000
#define TELEMETRY_BROADCAST_MS      100

// ============================================================================
// Peak Current Tracking
// ============================================================================

#define PEAK_CURRENT_WINDOW_SECONDS 60
#define PEAK_CURRENT_SAMPLE_RATE_MS 100

// ============================================================================
// NVS Storage Keys
// ============================================================================

#define NVS_NAMESPACE           "vescdash"
#define NVS_KEY_LAST_VESC       "last_vesc"
#define NVS_KEY_LAST_VESC_ADDR  "last_addr"
#define NVS_KEY_WIFI_MODE       "wifi_mode"

// ============================================================================
// UI Refresh Rates
// ============================================================================

#define UI_REFRESH_RATE_MS          16    // ~60 FPS
#define DASHBOARD_UPDATE_MS         50    // 20 FPS for data
#define STATUS_BAR_UPDATE_MS        1000  // 1 second

#endif // CONFIG_H
