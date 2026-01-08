/**
 * @file ble_vesc.h
 * BLE VESC Manager for BigDashVesc
 * 
 * NOTE: The M5Stack Tab5 uses ESP32-P4 which does not have native Bluetooth.
 * The ESP32-C6 co-processor handles WiFi/BLE but BLE is not currently 
 * accessible through the standard Arduino framework via ESP-Hosted protocol.
 * 
 * This implementation provides:
 * 1. Mock data mode for testing the UI
 * 2. An abstraction layer ready for when BLE support becomes available
 * 
 * Future options for real BLE:
 * - Custom firmware on the ESP32-C6 with UART bridge to P4
 * - External BLE module (like HM-10) via UART
 * - ESP-Hosted BLE support when available
 */

#ifndef BLE_VESC_H
#define BLE_VESC_H

#include <Arduino.h>
#include <vector>
#include "config.h"
#include "vesc_protocol.h"

// ============================================================================
// BLE Device Info Structure
// ============================================================================

struct BLEDeviceInfo {
    String name;
    String address;
    int rssi;
    bool isRandom;
    
    BLEDeviceInfo() : rssi(0), isRandom(false) {}
};

// ============================================================================
// BLE Connection States
// ============================================================================

enum BLEState {
    BLE_STATE_IDLE,
    BLE_STATE_SCANNING,
    BLE_STATE_SCAN_COMPLETE,
    BLE_STATE_CONNECTING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCONNECTED,
    BLE_STATE_RECONNECTING,
    BLE_STATE_NOT_SUPPORTED  // BLE hardware not available
};

// ============================================================================
// BLE Mode
// ============================================================================

enum BLEMode {
    BLE_MODE_MOCK,      // Simulated data for testing
    BLE_MODE_UART,      // External BLE module via UART (future)
    BLE_MODE_NATIVE     // Native BLE when available (future)
};

// ============================================================================
// Callback Interface
// ============================================================================

class BLEVescCallbacks {
public:
    virtual ~BLEVescCallbacks() {}
    virtual void onScanComplete(const std::vector<BLEDeviceInfo>& devices) = 0;
    virtual void onConnected(const BLEDeviceInfo& device) = 0;
    virtual void onDisconnected() = 0;
    virtual void onTelemetryReceived(const VescTelemetry& telemetry) = 0;
};

// ============================================================================
// BLE VESC Manager Class
// ============================================================================

class BLEVescManager {
public:
    BLEVescManager();
    ~BLEVescManager();
    
    // Initialization
    void begin();
    
    // Mode control
    void setMode(BLEMode mode);
    BLEMode getMode();
    bool isHardwareAvailable();
    
    // Scanning
    void startScan();
    void stopScan();
    bool isScanning();
    const std::vector<BLEDeviceInfo>& getDiscoveredDevices();
    
    // Connection
    bool connect(int deviceIndex);
    bool connect(const String& address, bool isRandom = true);
    void disconnect();
    bool isConnected();
    
    // Auto-reconnection
    void enableAutoReconnect(bool enable);
    bool attemptReconnect();
    
    // Communication (only used in real BLE mode)
    void sendGetValues();
    void sendAlive();
    void sendGetDecodedPpm();
    void sendGetDecodedAdc();
    void sendCommand(uint8_t command);
    void sendPacket(const uint8_t* data, size_t length);
    
    // Telemetry
    const VescTelemetry& getTelemetry();
    bool hasFreshData();
    
    // State
    BLEState getState();
    const BLEDeviceInfo& getConnectedDevice();
    String getStateString();
    
    // Callbacks
    void setCallbacks(BLEVescCallbacks* callbacks);
    
    // Main loop processing
    void update();
    
    // Peak current tracking
    void updatePeakCurrent(float current);
    float getPeakCurrent();
    void resetPeakCurrent();
    
private:
    BLEState state;
    BLEMode mode;
    std::vector<BLEDeviceInfo> discoveredDevices;
    BLEVescCallbacks* callbacks;
    
    // Connection state
    BLEDeviceInfo connectedDevice;
    int lastConnectedIndex;
    bool scanComplete;
    
    // Protocol handler
    VescProtocol protocol;
    VescTelemetry telemetry;
    
    // Reconnection
    bool autoReconnectEnabled;
    unsigned long lastReconnectAttempt;
    int reconnectAttempts;
    
    // Peak current tracking
    std::vector<float> currentSamples;
    unsigned long lastPeakSampleTime;
    float peakCurrent;
    
    // Mock mode state
    unsigned long lastMockUpdate;
    float mockVoltage;
    float mockCurrent;
    int32_t mockRpm;
    
    // Internal methods
    void updateMockTelemetry();
    void generateMockDevices();
};

// Global instance
extern BLEVescManager bleVesc;

#endif // BLE_VESC_H
