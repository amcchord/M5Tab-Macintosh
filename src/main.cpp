/**
 * @file main.cpp
 * BigDashVesc - VESC Dashboard for M5Stack Tab5
 * Main Application Entry Point
 * 
 * Integrates:
 * - M5GFX 720p Dashboard UI
 * - BLE VESC connection (via ESP32-C6 co-processor)
 * - WiFi with Web Dashboard
 * - WebSocket real-time telemetry
 * - NVS persistent storage
 */

#include <Arduino.h>
#include <M5Unified.h>
#include <M5GFX.h>
#include "config.h"
#include "storage.h"
#include "wifi_manager.h"
#include "ble_vesc.h"
#include "web_server.h"
#include "ui/ui_init.h"
#include "ui/ui_dashboard.h"

// ============================================================================
// Application State
// ============================================================================

enum AppState {
    STATE_BOOTING,
    STATE_WIFI_CONNECTING,
    STATE_BLE_SCANNING,
    STATE_DEVICE_SELECT,
    STATE_CONNECTING,
    STATE_DASHBOARD,
    STATE_RECONNECTING,
    STATE_MENU
};

AppState appState = STATE_BOOTING;
int selectedDeviceIndex = 0;

// Timing
unsigned long lastDataRequest = 0;
unsigned long lastInputRequest = 0;
unsigned long lastUiUpdate = 0;
unsigned long lastStatusUpdate = 0;
int inputRequestCycle = 0;

// ============================================================================
// BLE Callbacks
// ============================================================================

class AppBLECallbacks : public BLEVescCallbacks {
public:
    void onScanComplete(const std::vector<BLEDeviceInfo>& devices) override {
        Serial.printf("[App] Scan complete, found %d devices\n", devices.size());
        
        if (devices.empty()) {
            appState = STATE_DEVICE_SELECT;
            dashboard.setVescStatus(false, "No devices found");
        } else {
            appState = STATE_DEVICE_SELECT;
            selectedDeviceIndex = 0;
            dashboard.setVescStatus(false, "Select device...");
        }
    }
    
    void onConnected(const BLEDeviceInfo& device) override {
        Serial.printf("[App] Connected to %s\n", device.name.c_str());
        
        // Save as last connected device
        storage.saveLastVesc(device.name, device.address, device.isRandom);
        
        appState = STATE_DASHBOARD;
        dashboard.setVescStatus(true, device.name);
    }
    
    void onDisconnected() override {
        Serial.println("[App] Disconnected from VESC");
        
        if (appState == STATE_DASHBOARD) {
            appState = STATE_RECONNECTING;
            bleVesc.enableAutoReconnect(true);
            dashboard.setVescStatus(false, "Reconnecting...");
        }
    }
    
    void onTelemetryReceived(const VescTelemetry& telemetry) override {
        // Update UI with real telemetry
        dashboard.update(telemetry);
        
        // Broadcast to web clients
        webServer.broadcastTelemetry(telemetry);
    }
};

AppBLECallbacks bleCallbacks;

// Forward declarations
void handleDeviceSelectState();
void handleDashboardState();
void handleReconnectingState();

// ============================================================================
// Setup
// ============================================================================

void setup()
{
    // Initialize M5Stack Tab5
    auto cfg = M5.config();
    M5.begin(cfg);
    
    Serial.begin(115200);
    delay(500);
    
    Serial.println("\n\n========================================");
    Serial.println("   BigDashVesc v1.0");
    Serial.println("   M5Stack Tab5 VESC Dashboard");
    Serial.println("========================================\n");
    
    // Configure display orientation (landscape)
    M5.Display.setRotation(3);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(2);
    
    // Show boot message
    M5.Display.setCursor(50, 50);
    M5.Display.println("Initializing BigDashVesc...");
    
    Serial.printf("[App] Display: %dx%d\n", M5.Display.width(), M5.Display.height());
    Serial.printf("[App] Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("[App] Free PSRAM: %d bytes\n", ESP.getFreePsram());
    
    // Initialize storage
    M5.Display.setCursor(50, 80);
    M5.Display.println("Loading settings...");
    storage.begin();
    
    // Initialize display/UI
    Serial.println("[App] Initializing display...");
    M5.Display.setCursor(50, 110);
    M5.Display.println("Initializing display...");
    ui_init();
    dashboard.begin();
    
    // Initial render
    dashboard.setWifiStatus("Initializing...");
    dashboard.setVescStatus(false, "");
    dashboard.render();
    
    // Initialize WiFi with Tab5 SDIO pins
    Serial.println("[App] Initializing WiFi...");
    dashboard.setWifiStatus("Connecting WiFi...");
    dashboard.render();
    
    wifiMgr.begin();
    
    bool wifiConnected = wifiMgr.connectToKnownNetworks();
    if (!wifiConnected) {
        Serial.println("[App] Starting AP mode...");
        wifiMgr.startAccessPoint();
    }
    
    dashboard.setWifiStatus(wifiMgr.getStatusString());
    Serial.printf("[App] WiFi: %s\n", wifiMgr.getStatusString().c_str());
    
    // Initialize BLE
    Serial.println("[App] Initializing BLE...");
    dashboard.setVescStatus(false, "Initializing BLE...");
    dashboard.render();
    
    bleVesc.begin();
    bleVesc.setCallbacks(&bleCallbacks);
    
    // Start web server
    Serial.println("[App] Starting web server...");
    webServer.begin(&bleVesc);
    
    // Check for previously connected VESC
    StoredVescInfo lastVesc = storage.getLastVesc();
    
    if (lastVesc.valid) {
        Serial.printf("[App] Found saved VESC: %s (%s)\n", 
                      lastVesc.name.c_str(), lastVesc.address.c_str());
        
        dashboard.setVescStatus(false, "Connecting to " + lastVesc.name + "...");
        dashboard.render();
        
        // Try to connect to the saved VESC
        appState = STATE_CONNECTING;
        
        if (bleVesc.connect(lastVesc.address, lastVesc.isRandom)) {
            appState = STATE_DASHBOARD;
            dashboard.setVescStatus(true, lastVesc.name);
        } else {
            Serial.println("[App] Could not connect to saved VESC, starting scan");
            appState = STATE_BLE_SCANNING;
            dashboard.setVescStatus(false, "Scanning...");
            bleVesc.startScan();
        }
    } else {
        // No saved VESC, start scanning
        Serial.println("[App] No saved VESC, starting scan");
        appState = STATE_BLE_SCANNING;
        dashboard.setVescStatus(false, "Scanning...");
        bleVesc.startScan();
    }
    
    dashboard.render();
    Serial.println("[App] Setup complete\n");
}

// ============================================================================
// Main Loop
// ============================================================================

void loop()
{
    unsigned long now = millis();
    
    // Update M5Stack (handles touch, buttons)
    M5.update();
    
    // Update subsystems
    wifiMgr.update();
    bleVesc.update();
    webServer.update();
    
    // Update WiFi status periodically
    if (now - lastStatusUpdate >= STATUS_BAR_UPDATE_MS) {
        lastStatusUpdate = now;
        dashboard.setWifiStatus(wifiMgr.getStatusString());
    }
    
    // State machine
    switch (appState) {
        case STATE_BOOTING:
            // Should not stay here
            break;
            
        case STATE_WIFI_CONNECTING:
            // WiFi connection handled in setup
            break;
            
        case STATE_BLE_SCANNING:
            // Scanning handled by BLE manager
            // Callback will change state when complete
            dashboard.updateMock();  // Show animation while scanning
            break;
            
        case STATE_DEVICE_SELECT:
            handleDeviceSelectState();
            break;
            
        case STATE_CONNECTING:
            // Connection handled by BLE manager
            dashboard.updateMock();  // Show animation while connecting
            break;
            
        case STATE_DASHBOARD:
            handleDashboardState();
            break;
            
        case STATE_RECONNECTING:
            handleReconnectingState();
            break;
            
        case STATE_MENU:
            // Future: handle menu state
            break;
    }
    
    // Small delay to prevent CPU hogging
    delay(5);
}

// ============================================================================
// State Handlers
// ============================================================================

void handleDeviceSelectState()
{
    const std::vector<BLEDeviceInfo>& devices = bleVesc.getDiscoveredDevices();
    
    // For now, auto-connect to first device if available
    // TODO: Add device selection UI screen
    if (!devices.empty()) {
        Serial.printf("[App] Auto-connecting to first device: %s\n", devices[0].name.c_str());
        dashboard.setVescStatus(false, "Connecting...");
        dashboard.render();
        
        appState = STATE_CONNECTING;
        
        if (bleVesc.connect(0)) {
            // Connection successful - callback will handle state change
        } else {
            Serial.println("[App] Connection failed");
            dashboard.setVescStatus(false, "Connection failed");
            dashboard.showWarning("Failed to connect to VESC");
            dashboard.render();
            delay(2000);
            dashboard.hideWarning();
            
            // Rescan
            appState = STATE_BLE_SCANNING;
            bleVesc.startScan();
        }
    } else {
        // No devices found, show message and wait for rescan
        dashboard.setVescStatus(false, "No VESC found");
        dashboard.updateMock();  // Keep showing something
        
        // Auto-rescan after 5 seconds
        static unsigned long lastScanTime = 0;
        if (millis() - lastScanTime > 5000) {
            lastScanTime = millis();
            appState = STATE_BLE_SCANNING;
            dashboard.setVescStatus(false, "Scanning...");
            bleVesc.startScan();
        }
    }
}

void handleDashboardState()
{
    unsigned long now = millis();
    
    // Request telemetry data periodically
    if (now - lastDataRequest >= VESC_DATA_REFRESH_MS) {
        lastDataRequest = now;
        bleVesc.sendGetValues();
    }
    
    // Request PPM/ADC input values (slower rate, alternating)
    if (now - lastInputRequest >= 200) {
        lastInputRequest = now;
        if (inputRequestCycle == 0) {
            bleVesc.sendGetDecodedPpm();
        } else {
            bleVesc.sendGetDecodedAdc();
        }
        inputRequestCycle = (inputRequestCycle + 1) % 2;
    }
    
    // Check for connection loss
    if (!bleVesc.isConnected()) {
        Serial.println("[App] Connection lost, entering reconnect state");
        appState = STATE_RECONNECTING;
        bleVesc.enableAutoReconnect(true);
        dashboard.setVescStatus(false, "Reconnecting...");
    }
    
    // Touch handling for future menu access
    auto touch = M5.Touch.getDetail();
    if (touch.wasPressed()) {
        // Could open menu or trigger actions
        Serial.printf("[App] Touch at (%d, %d)\n", touch.x, touch.y);
    }
}

void handleReconnectingState()
{
    // Display reconnecting status
    dashboard.setVescStatus(false, "Reconnecting...");
    dashboard.updateMock();  // Keep showing something
    
    // Check if reconnected
    if (bleVesc.isConnected()) {
        Serial.println("[App] Reconnected successfully");
        appState = STATE_DASHBOARD;
        
        const BLEDeviceInfo& device = bleVesc.getConnectedDevice();
        dashboard.setVescStatus(true, device.name);
    }
    
    // Check if gave up
    BLEState bleState = bleVesc.getState();
    if (bleState == BLE_STATE_IDLE) {
        Serial.println("[App] Reconnection failed, starting scan");
        appState = STATE_BLE_SCANNING;
        dashboard.setVescStatus(false, "Scanning...");
        bleVesc.startScan();
    }
}
