/**
 * @file ble_vesc.cpp
 * BLE VESC Manager Implementation
 * 
 * Currently operates in MOCK mode to test the UI.
 * Ready for real BLE integration when hardware support becomes available.
 */

#include "ble_vesc.h"

// Global instance
BLEVescManager bleVesc;

// ============================================================================
// Constructor / Destructor
// ============================================================================

BLEVescManager::BLEVescManager()
    : state(BLE_STATE_IDLE)
    , mode(BLE_MODE_MOCK)
    , callbacks(nullptr)
    , lastConnectedIndex(-1)
    , scanComplete(false)
    , autoReconnectEnabled(true)
    , lastReconnectAttempt(0)
    , reconnectAttempts(0)
    , lastPeakSampleTime(0)
    , peakCurrent(0)
    , lastMockUpdate(0)
    , mockVoltage(48.0f)
    , mockCurrent(0.0f)
    , mockRpm(0)
{
    currentSamples.reserve(PEAK_CURRENT_WINDOW_SECONDS * (1000 / PEAK_CURRENT_SAMPLE_RATE_MS));
}

BLEVescManager::~BLEVescManager() {
}

// ============================================================================
// Initialization
// ============================================================================

void BLEVescManager::begin() {
    Serial.println("[BLE] Initializing BLE Manager...");
    Serial.println("[BLE] NOTE: ESP32-P4 does not have native Bluetooth.");
    Serial.println("[BLE] Running in MOCK mode for UI testing.");
    
    mode = BLE_MODE_MOCK;
    state = BLE_STATE_IDLE;
    
    Serial.println("[BLE] Initialization complete (MOCK mode)");
}

// ============================================================================
// Mode Control
// ============================================================================

void BLEVescManager::setMode(BLEMode newMode) {
    mode = newMode;
    if (mode == BLE_MODE_MOCK) {
        Serial.println("[BLE] Switched to MOCK mode");
    } else if (mode == BLE_MODE_UART) {
        Serial.println("[BLE] UART mode not yet implemented");
        mode = BLE_MODE_MOCK;
    } else if (mode == BLE_MODE_NATIVE) {
        Serial.println("[BLE] Native BLE not available on ESP32-P4");
        mode = BLE_MODE_MOCK;
    }
}

BLEMode BLEVescManager::getMode() {
    return mode;
}

bool BLEVescManager::isHardwareAvailable() {
    // ESP32-P4 doesn't have native BLE
    return false;
}

// ============================================================================
// Scanning
// ============================================================================

void BLEVescManager::startScan() {
    Serial.println("[BLE] Starting scan...");
    
    discoveredDevices.clear();
    scanComplete = false;
    state = BLE_STATE_SCANNING;
    
    if (mode == BLE_MODE_MOCK) {
        // Generate mock devices after a short delay
        delay(1500);  // Simulate scan time
        generateMockDevices();
    }
    
    scanComplete = true;
    state = BLE_STATE_SCAN_COMPLETE;
    
    Serial.printf("[BLE] Scan complete. Found %d device(s).\n", discoveredDevices.size());
    
    if (callbacks) {
        callbacks->onScanComplete(discoveredDevices);
    }
}

void BLEVescManager::generateMockDevices() {
    // Generate some mock VESC devices
    BLEDeviceInfo dev1;
    dev1.name = "VESC_Mock_01";
    dev1.address = "00:11:22:33:44:55";
    dev1.rssi = -65;
    dev1.isRandom = true;
    discoveredDevices.push_back(dev1);
    
    BLEDeviceInfo dev2;
    dev2.name = "VESC BLE UART";
    dev2.address = "AA:BB:CC:DD:EE:FF";
    dev2.rssi = -72;
    dev2.isRandom = true;
    discoveredDevices.push_back(dev2);
    
    Serial.println("[BLE] Generated mock devices for testing");
}

void BLEVescManager::stopScan() {
    if (state == BLE_STATE_SCANNING) {
        state = BLE_STATE_SCAN_COMPLETE;
    }
}

bool BLEVescManager::isScanning() {
    return state == BLE_STATE_SCANNING;
}

const std::vector<BLEDeviceInfo>& BLEVescManager::getDiscoveredDevices() {
    return discoveredDevices;
}

// ============================================================================
// Connection
// ============================================================================

bool BLEVescManager::connect(int deviceIndex) {
    if (deviceIndex < 0 || deviceIndex >= (int)discoveredDevices.size()) {
        Serial.println("[BLE] Invalid device index");
        return false;
    }
    
    const BLEDeviceInfo& device = discoveredDevices[deviceIndex];
    lastConnectedIndex = deviceIndex;
    
    return connect(device.address, device.isRandom);
}

bool BLEVescManager::connect(const String& address, bool isRandom) {
    Serial.printf("[BLE] Connecting to: %s\n", address.c_str());
    
    state = BLE_STATE_CONNECTING;
    
    if (mode == BLE_MODE_MOCK) {
        // Simulate connection delay
        delay(1000);
        
        // Find device info
        bool found = false;
        for (auto& d : discoveredDevices) {
            if (d.address == address) {
                connectedDevice = d;
                found = true;
                break;
            }
        }
        if (!found) {
            connectedDevice.address = address;
            connectedDevice.name = "VESC Mock";
            connectedDevice.isRandom = isRandom;
            connectedDevice.rssi = -60;
        }
        
        state = BLE_STATE_CONNECTED;
        reconnectAttempts = 0;
        telemetry.reset();
        
        // Reset mock state
        mockVoltage = 48.0f;
        mockCurrent = 0.0f;
        mockRpm = 0;
        peakCurrent = 0;
        currentSamples.clear();
        lastMockUpdate = millis();
        
        Serial.printf("[BLE] Connected to %s (MOCK)\n", connectedDevice.name.c_str());
        
        if (callbacks) {
            callbacks->onConnected(connectedDevice);
        }
        
        return true;
    }
    
    // Real BLE connection would go here
    Serial.println("[BLE] Real BLE not available");
    state = BLE_STATE_NOT_SUPPORTED;
    return false;
}

void BLEVescManager::disconnect() {
    Serial.println("[BLE] Disconnecting...");
    state = BLE_STATE_DISCONNECTED;
    autoReconnectEnabled = false;
    
    if (callbacks) {
        callbacks->onDisconnected();
    }
}

bool BLEVescManager::isConnected() {
    return state == BLE_STATE_CONNECTED;
}

// ============================================================================
// Auto-reconnection
// ============================================================================

void BLEVescManager::enableAutoReconnect(bool enable) {
    autoReconnectEnabled = enable;
}

bool BLEVescManager::attemptReconnect() {
    if (!autoReconnectEnabled) {
        return false;
    }
    
    if (reconnectAttempts >= VESC_MAX_RECONNECT_ATTEMPTS) {
        Serial.println("[BLE] Max reconnect attempts reached");
        state = BLE_STATE_IDLE;
        return false;
    }
    
    unsigned long now = millis();
    if (now - lastReconnectAttempt < VESC_RECONNECT_INTERVAL_MS) {
        return false;
    }
    
    lastReconnectAttempt = now;
    reconnectAttempts++;
    
    Serial.printf("[BLE] Reconnect attempt %d/%d\n", reconnectAttempts, VESC_MAX_RECONNECT_ATTEMPTS);
    
    state = BLE_STATE_RECONNECTING;
    
    if (!connectedDevice.address.isEmpty()) {
        if (connect(connectedDevice.address, connectedDevice.isRandom)) {
            return true;
        }
    }
    
    state = BLE_STATE_DISCONNECTED;
    return false;
}

// ============================================================================
// Communication (stub - for real BLE)
// ============================================================================

void BLEVescManager::sendGetValues() {
    // In mock mode, we generate values internally
}

void BLEVescManager::sendAlive() {
    // No-op in mock mode
}

void BLEVescManager::sendGetDecodedPpm() {
    // No-op in mock mode
}

void BLEVescManager::sendGetDecodedAdc() {
    // No-op in mock mode
}

void BLEVescManager::sendCommand(uint8_t command) {
    // No-op in mock mode
}

void BLEVescManager::sendPacket(const uint8_t* data, size_t length) {
    // No-op in mock mode
}

// ============================================================================
// Telemetry
// ============================================================================

const VescTelemetry& BLEVescManager::getTelemetry() {
    return telemetry;
}

bool BLEVescManager::hasFreshData() {
    if (!telemetry.valid) {
        return false;
    }
    return (millis() - telemetry.lastUpdate) < VESC_DATA_STALE_TIMEOUT_MS;
}

// ============================================================================
// State
// ============================================================================

BLEState BLEVescManager::getState() {
    return state;
}

const BLEDeviceInfo& BLEVescManager::getConnectedDevice() {
    return connectedDevice;
}

String BLEVescManager::getStateString() {
    switch (state) {
        case BLE_STATE_IDLE:
            return "Idle";
        case BLE_STATE_SCANNING:
            return "Scanning...";
        case BLE_STATE_SCAN_COMPLETE:
            return "Scan Complete";
        case BLE_STATE_CONNECTING:
            return "Connecting...";
        case BLE_STATE_CONNECTED:
            return "Connected";
        case BLE_STATE_DISCONNECTED:
            return "Disconnected";
        case BLE_STATE_RECONNECTING:
            return "Reconnecting...";
        case BLE_STATE_NOT_SUPPORTED:
            return "Not Supported";
        default:
            return "Unknown";
    }
}

void BLEVescManager::setCallbacks(BLEVescCallbacks* cb) {
    callbacks = cb;
}

// ============================================================================
// Main Loop Update
// ============================================================================

void BLEVescManager::update() {
    // Handle mock data generation
    if (mode == BLE_MODE_MOCK && state == BLE_STATE_CONNECTED) {
        updateMockTelemetry();
    }
    
    // Handle reconnection
    if (state == BLE_STATE_DISCONNECTED && autoReconnectEnabled) {
        attemptReconnect();
    }
}

// ============================================================================
// Mock Telemetry Generation
// ============================================================================

void BLEVescManager::updateMockTelemetry() {
    unsigned long now = millis();
    
    // Update at ~20Hz
    if (now - lastMockUpdate < 50) {
        return;
    }
    lastMockUpdate = now;
    
    // Simulate voltage discharge (very slow)
    mockVoltage -= 0.0005f;
    if (mockVoltage < 36.0f) {
        mockVoltage = 50.4f;
    }
    
    // Simulate varying current with noise
    float timeSeconds = now / 1000.0f;
    mockCurrent = 10.0f + sin(timeSeconds * 0.5f) * 8.0f + (random(-100, 100) / 100.0f);
    
    // Simulate RPM based on current
    mockRpm = (int)(abs(mockCurrent) * 250 + sin(timeSeconds * 0.3f) * 500 + random(-100, 100));
    
    // Update telemetry struct
    telemetry.voltage = mockVoltage + (random(-10, 10) / 100.0f);
    telemetry.currentIn = mockCurrent;
    telemetry.currentMotor = mockCurrent * 1.2f;
    telemetry.rpm = mockRpm;
    
    // Calculate battery from voltage
    telemetry.cellCount = protocol.estimateCellCount(telemetry.voltage);
    if (telemetry.cellCount > 0) {
        telemetry.cellVoltage = telemetry.voltage / telemetry.cellCount;
        telemetry.batteryPercent = protocol.estimateBatteryPercent(telemetry.cellVoltage);
    } else {
        telemetry.cellVoltage = 0;
        telemetry.batteryPercent = 0;
    }
    
    // Energy consumption (cumulative)
    float hourFraction = 50.0f / 1000.0f / 3600.0f;  // 50ms in hours
    telemetry.ampHours += abs(mockCurrent) * hourFraction;
    telemetry.wattHours += abs(mockCurrent * mockVoltage) * hourFraction;
    
    // Temperatures (affected by current)
    telemetry.tempFet = 45.0f + abs(mockCurrent) * 0.4f + sin(timeSeconds * 0.1f) * 3.0f;
    telemetry.tempMotor = 55.0f + abs(mockCurrent) * 0.5f + sin(timeSeconds * 0.08f) * 4.0f;
    
    // Duty cycle
    telemetry.dutyNow = abs(mockCurrent) / 100.0f;
    if (telemetry.dutyNow > 0.95f) {
        telemetry.dutyNow = 0.95f;
    }
    
    // Input values (throttle simulation)
    telemetry.ppmValid = true;
    telemetry.ppmValue = sin(timeSeconds * 0.4f) * 0.5f;
    telemetry.adcValid = false;
    telemetry.adcValue = 0;
    
    // No faults in mock mode (unless we want to test)
    telemetry.faultCode = 0;
    
    telemetry.valid = true;
    telemetry.lastUpdate = now;
    
    // Update peak current tracking
    updatePeakCurrent(abs(telemetry.currentIn));
    telemetry.peakCurrent = peakCurrent;
    
    // Notify callbacks
    if (callbacks) {
        callbacks->onTelemetryReceived(telemetry);
    }
}

// ============================================================================
// Peak Current Tracking
// ============================================================================

void BLEVescManager::updatePeakCurrent(float current) {
    unsigned long now = millis();
    
    // Sample at defined rate
    if (now - lastPeakSampleTime >= PEAK_CURRENT_SAMPLE_RATE_MS) {
        lastPeakSampleTime = now;
        currentSamples.push_back(current);
        
        // Remove old samples
        int maxSamples = PEAK_CURRENT_WINDOW_SECONDS * (1000 / PEAK_CURRENT_SAMPLE_RATE_MS);
        while ((int)currentSamples.size() > maxSamples) {
            currentSamples.erase(currentSamples.begin());
        }
        
        // Find peak
        peakCurrent = 0;
        for (float sample : currentSamples) {
            if (sample > peakCurrent) {
                peakCurrent = sample;
            }
        }
    }
}

float BLEVescManager::getPeakCurrent() {
    return peakCurrent;
}

void BLEVescManager::resetPeakCurrent() {
    currentSamples.clear();
    peakCurrent = 0;
}
