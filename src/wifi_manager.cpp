/**
 * @file wifi_manager.cpp
 * WiFi Manager Implementation for Tab5
 */

#include "wifi_manager.h"

// Global instance
WiFiManager wifiMgr;

// Define the known networks
const WiFiNetwork KNOWN_NETWORKS[] = {
    {"SvensHaus", "montreal19"},
    {"McLab", "gogogadget"}
};
const int KNOWN_NETWORKS_COUNT = sizeof(KNOWN_NETWORKS) / sizeof(KNOWN_NETWORKS[0]);

// ============================================================================
// Constructor
// ============================================================================

WiFiManager::WiFiManager() 
    : state(WIFI_STATE_DISCONNECTED)
    , lastConnectAttempt(0)
    , currentNetworkIndex(0)
    , pinsConfigured(false)
{
}

// ============================================================================
// Pin Configuration for Tab5
// ============================================================================

void WiFiManager::configurePins() {
    if (pinsConfigured) {
        return;
    }
    
    Serial.println("[WiFi] Configuring SDIO pins for ESP32-C6 communication...");
    
    // CRITICAL: Tab5 uses ESP32-P4 which has no native WiFi
    // WiFi is provided by ESP32-C6 co-processor via SDIO bus
    // Must call WiFi.setPins() before any WiFi operations
    WiFi.setPins(SDIO2_CLK, SDIO2_CMD, SDIO2_D0, SDIO2_D1, SDIO2_D2, SDIO2_D3, SDIO2_RST);
    
    pinsConfigured = true;
    Serial.println("[WiFi] SDIO pins configured");
}

// ============================================================================
// Initialization
// ============================================================================

void WiFiManager::begin() {
    Serial.println("[WiFi] Initializing...");
    
    // Configure SDIO pins first
    configurePins();
    
    // Generate AP name based on MAC
    generateAPName();
    
    // Start WiFi
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    
    Serial.printf("[WiFi] MAC Address: %s\n", WiFi.macAddress().c_str());
    Serial.printf("[WiFi] AP Name will be: %s\n", apName.c_str());
}

// ============================================================================
// AP Name Generation
// ============================================================================

void WiFiManager::generateAPName() {
    // Get MAC address
    uint8_t mac[6];
    WiFi.macAddress(mac);
    
    // Generate hex string from last 2 bytes
    char hexStr[5];
    snprintf(hexStr, sizeof(hexStr), "%02X%02X", mac[4], mac[5]);
    
    apName = "VESCDASH-" + String(hexStr);
}

// ============================================================================
// Connect to Known Networks
// ============================================================================

bool WiFiManager::connectToKnownNetworks() {
    Serial.println("[WiFi] Attempting to connect to known networks...");
    
    state = WIFI_STATE_CONNECTING;
    
    for (int i = 0; i < KNOWN_NETWORKS_COUNT; i++) {
        const WiFiNetwork& network = KNOWN_NETWORKS[i];
        Serial.printf("[WiFi] Trying: %s\n", network.ssid);
        
        if (tryConnectNetwork(network.ssid, network.password)) {
            Serial.printf("[WiFi] Connected to: %s\n", network.ssid);
            Serial.printf("[WiFi] IP Address: %s\n", WiFi.localIP().toString().c_str());
            currentSSID = network.ssid;
            state = WIFI_STATE_CONNECTED_STA;
            return true;
        }
    }
    
    Serial.println("[WiFi] Failed to connect to any known network");
    state = WIFI_STATE_DISCONNECTED;
    return false;
}

bool WiFiManager::tryConnectNetwork(const char* ssid, const char* password) {
    WiFi.disconnect();
    delay(100);
    
    WiFi.begin(ssid, password);
    
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startTime > WIFI_CONNECT_TIMEOUT_MS) {
            Serial.printf("[WiFi] Timeout connecting to %s\n", ssid);
            return false;
        }
        delay(100);
        Serial.print(".");
    }
    Serial.println();
    
    return true;
}

// ============================================================================
// Access Point Mode
// ============================================================================

void WiFiManager::startAccessPoint() {
    Serial.printf("[WiFi] Starting Access Point: %s\n", apName.c_str());
    
    WiFi.disconnect();
    delay(100);
    
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName.c_str(), AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONNECTIONS);
    
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[WiFi] AP IP Address: %s\n", ip.toString().c_str());
    
    state = WIFI_STATE_AP_MODE;
    currentSSID = apName;
}

// ============================================================================
// Disconnect
// ============================================================================

void WiFiManager::disconnect() {
    if (state == WIFI_STATE_AP_MODE) {
        WiFi.softAPdisconnect(true);
    } else {
        WiFi.disconnect();
    }
    state = WIFI_STATE_DISCONNECTED;
    currentSSID = "";
}

// ============================================================================
// State Queries
// ============================================================================

WiFiState WiFiManager::getState() {
    return state;
}

bool WiFiManager::isConnected() {
    return state == WIFI_STATE_CONNECTED_STA || state == WIFI_STATE_AP_MODE;
}

bool WiFiManager::isAccessPoint() {
    return state == WIFI_STATE_AP_MODE;
}

String WiFiManager::getIPAddress() {
    if (state == WIFI_STATE_AP_MODE) {
        return WiFi.softAPIP().toString();
    } else if (state == WIFI_STATE_CONNECTED_STA) {
        return WiFi.localIP().toString();
    }
    return "0.0.0.0";
}

String WiFiManager::getSSID() {
    return currentSSID;
}

int WiFiManager::getRSSI() {
    if (state == WIFI_STATE_CONNECTED_STA) {
        return WiFi.RSSI();
    }
    return 0;
}

String WiFiManager::getAPName() {
    return apName;
}

// ============================================================================
// Status String for Display
// ============================================================================

String WiFiManager::getStatusString() {
    switch (state) {
        case WIFI_STATE_DISCONNECTED:
            return "Disconnected";
        case WIFI_STATE_CONNECTING:
            return "Connecting...";
        case WIFI_STATE_CONNECTED_STA:
            return getIPAddress();
        case WIFI_STATE_AP_MODE:
            return "AP: " + getIPAddress();
        default:
            return "Unknown";
    }
}

// ============================================================================
// Update (Call in Loop)
// ============================================================================

void WiFiManager::update() {
    // Check if STA connection was lost
    if (state == WIFI_STATE_CONNECTED_STA) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Connection lost, attempting reconnect...");
            state = WIFI_STATE_CONNECTING;
            
            // Try to reconnect
            if (!connectToKnownNetworks()) {
                // Fall back to AP mode
                Serial.println("[WiFi] Reconnect failed, starting AP mode");
                startAccessPoint();
            }
        }
    }
}
