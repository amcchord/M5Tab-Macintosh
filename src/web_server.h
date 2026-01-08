/**
 * @file web_server.h
 * Web Server Manager - LittleFS based
 * HTTP server with WebSocket support for real-time telemetry
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"
#include "vesc_protocol.h"

// Forward declaration
class BLEVescManager;

// ============================================================================
// Web Server Manager Class
// ============================================================================

class WebServerManager {
public:
    WebServerManager();
    ~WebServerManager();
    
    // Initialization
    void begin(BLEVescManager* vescManager);
    void stop();
    
    // Telemetry broadcasting
    void broadcastTelemetry(const VescTelemetry& telemetry);
    
    // Update (call in loop)
    void update();
    
    // Connection info
    int getClientCount();
    bool hasClients();
    
private:
    AsyncWebServer* server;
    AsyncWebSocket* ws;
    BLEVescManager* vescMgr;
    
    unsigned long lastBroadcast;
    bool isRunning;
    bool fsInitialized;
    
    // LittleFS initialization
    bool initFileSystem();
    
    // Route setup
    void setupRoutes();
    void setupStaticFiles();
    void setupApiRoutes();
    
    // API handlers
    void handleGetTelemetry(AsyncWebServerRequest* request);
    void handleGetStatus(AsyncWebServerRequest* request);
    void handleVescReboot(AsyncWebServerRequest* request);
    
    // WebSocket handlers
    void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                   AwsEventType type, void* arg, uint8_t* data, size_t len);
    void handleWsMessage(AsyncWebSocketClient* client, uint8_t* data, size_t len);
    
    // JSON generation
    String generateTelemetryJson(const VescTelemetry& telemetry);
    String generateStatusJson();
    
    // MIME type helper
    String getContentType(const String& filename);
};

// Global instance
extern WebServerManager webServer;

#endif // WEB_SERVER_H
