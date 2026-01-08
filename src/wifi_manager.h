/**
 * @file wifi_manager.h
 * WiFi Manager for Tab5 (ESP32-P4 + ESP32-C6)
 * Handles WiFi connection via the C6 co-processor
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include "config.h"

// ============================================================================
// WiFi Connection States
// ============================================================================

enum WiFiState {
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED_STA,
    WIFI_STATE_AP_MODE
};

// ============================================================================
// WiFi Manager Class
// ============================================================================

class WiFiManager {
public:
    WiFiManager();
    
    // Initialization and connection
    void begin();
    bool connectToKnownNetworks();
    void startAccessPoint();
    void disconnect();
    
    // State queries
    WiFiState getState();
    bool isConnected();
    bool isAccessPoint();
    String getIPAddress();
    String getSSID();
    int getRSSI();
    String getAPName();
    
    // Status string for display
    String getStatusString();
    
    // Update (call in loop)
    void update();
    
private:
    WiFiState state;
    String apName;
    String currentSSID;
    unsigned long lastConnectAttempt;
    int currentNetworkIndex;
    bool pinsConfigured;
    
    void configurePins();
    void generateAPName();
    bool tryConnectNetwork(const char* ssid, const char* password);
};

// Global instance
extern WiFiManager wifiMgr;

#endif // WIFI_MANAGER_H
