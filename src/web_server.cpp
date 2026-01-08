/**
 * @file web_server.cpp
 * Web Server Implementation - LittleFS based
 */

#include "web_server.h"
#include "ble_vesc.h"

// Global instance
WebServerManager webServer;

// ============================================================================
// Constructor / Destructor
// ============================================================================

WebServerManager::WebServerManager()
    : server(nullptr)
    , ws(nullptr)
    , vescMgr(nullptr)
    , lastBroadcast(0)
    , isRunning(false)
    , fsInitialized(false)
{
}

WebServerManager::~WebServerManager() {
    stop();
}

// ============================================================================
// File System Initialization
// ============================================================================

bool WebServerManager::initFileSystem() {
    if (fsInitialized) {
        return true;
    }
    
    Serial.println("[Web] Initializing LittleFS...");
    
    if (!LittleFS.begin(true)) {
        Serial.println("[Web] LittleFS mount failed!");
        return false;
    }
    
    // List files for debugging
    Serial.println("[Web] LittleFS contents:");
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file) {
        Serial.printf("  %s (%d bytes)\n", file.name(), file.size());
        file = root.openNextFile();
    }
    
    fsInitialized = true;
    Serial.println("[Web] LittleFS initialized");
    return true;
}

// ============================================================================
// Initialization
// ============================================================================

void WebServerManager::begin(BLEVescManager* vescManager) {
    if (isRunning) {
        return;
    }
    
    vescMgr = vescManager;
    
    Serial.println("[Web] Starting web server...");
    
    // Initialize file system
    if (!initFileSystem()) {
        Serial.println("[Web] WARNING: LittleFS not available, static files won't be served");
    }
    
    server = new AsyncWebServer(WEB_SERVER_PORT);
    ws = new AsyncWebSocket(WEBSOCKET_PATH);
    
    // Setup WebSocket handler
    ws->onEvent([this](AsyncWebSocket* server, AsyncWebSocketClient* client,
                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
        this->onWsEvent(server, client, type, arg, data, len);
    });
    
    server->addHandler(ws);
    
    // Setup routes
    setupRoutes();
    
    server->begin();
    isRunning = true;
    
    Serial.printf("[Web] Server started on port %d\n", WEB_SERVER_PORT);
}

void WebServerManager::stop() {
    if (!isRunning) {
        return;
    }
    
    if (ws) {
        ws->closeAll();
        delete ws;
        ws = nullptr;
    }
    
    if (server) {
        server->end();
        delete server;
        server = nullptr;
    }
    
    isRunning = false;
    Serial.println("[Web] Server stopped");
}

// ============================================================================
// MIME Type Helper
// ============================================================================

String WebServerManager::getContentType(const String& filename) {
    if (filename.endsWith(".html")) return "text/html";
    if (filename.endsWith(".css")) return "text/css";
    if (filename.endsWith(".js")) return "application/javascript";
    if (filename.endsWith(".json")) return "application/json";
    if (filename.endsWith(".png")) return "image/png";
    if (filename.endsWith(".jpg")) return "image/jpeg";
    if (filename.endsWith(".ico")) return "image/x-icon";
    if (filename.endsWith(".svg")) return "image/svg+xml";
    if (filename.endsWith(".woff")) return "font/woff";
    if (filename.endsWith(".woff2")) return "font/woff2";
    return "text/plain";
}

// ============================================================================
// Route Setup
// ============================================================================

void WebServerManager::setupRoutes() {
    // Setup API routes first (more specific)
    setupApiRoutes();
    
    // Setup static file serving from LittleFS
    setupStaticFiles();
    
    // 404 handler
    server->onNotFound([](AsyncWebServerRequest* request) {
        request->send(404, "text/plain", "Not Found");
    });
}

void WebServerManager::setupStaticFiles() {
    // Serve index.html for root
    server->on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (fsInitialized && LittleFS.exists("/index.html")) {
            request->send(LittleFS, "/index.html", "text/html");
        } else {
            // Fallback: simple inline page
            String html = "<!DOCTYPE html><html><head>";
            html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
            html += "<title>BigDashVesc</title>";
            html += "<style>body{font-family:sans-serif;background:#0d1117;color:#fff;display:flex;";
            html += "justify-content:center;align-items:center;height:100vh;margin:0;}";
            html += ".box{text-align:center;padding:40px;background:#161b22;border-radius:12px;}";
            html += "h1{color:#58a6ff;margin-bottom:20px;}</style></head><body>";
            html += "<div class='box'><h1>BigDashVesc</h1>";
            html += "<p>Web UI files not found in LittleFS.</p>";
            html += "<p>Upload files with: <code>pio run --target uploadfs</code></p>";
            html += "</div></body></html>";
            request->send(200, "text/html", html);
        }
    });
    
    // Serve all other files from LittleFS
    server->serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
}

void WebServerManager::setupApiRoutes() {
    // Telemetry API (polling fallback)
    server->on("/api/telemetry", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleGetTelemetry(request);
    });
    
    // Status API
    server->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleGetStatus(request);
    });
    
    // VESC reboot
    server->on("/api/vesc/reboot", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleVescReboot(request);
    });
    
    // Health check
    server->on("/health", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/plain", "OK");
    });
    
    // System info
    server->on("/api/system", HTTP_GET, [](AsyncWebServerRequest* request) {
        JsonDocument doc;
        doc["freeHeap"] = ESP.getFreeHeap();
        doc["freePsram"] = ESP.getFreePsram();
        doc["uptime"] = millis() / 1000;
        doc["chipModel"] = ESP.getChipModel();
        doc["cpuFreq"] = ESP.getCpuFreqMHz();
        
        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });
}

// ============================================================================
// API Handlers
// ============================================================================

void WebServerManager::handleGetTelemetry(AsyncWebServerRequest* request) {
    if (!vescMgr) {
        request->send(500, "application/json", "{\"error\":\"No VESC manager\"}");
        return;
    }
    
    const VescTelemetry& telem = vescMgr->getTelemetry();
    String json = generateTelemetryJson(telem);
    request->send(200, "application/json", json);
}

void WebServerManager::handleGetStatus(AsyncWebServerRequest* request) {
    String json = generateStatusJson();
    request->send(200, "application/json", json);
}

void WebServerManager::handleVescReboot(AsyncWebServerRequest* request) {
    if (!vescMgr || !vescMgr->isConnected()) {
        request->send(503, "application/json", "{\"success\":false,\"error\":\"VESC not connected\"}");
        return;
    }
    
    vescMgr->sendCommand(COMM_REBOOT);
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Reboot command sent\"}");
}

// ============================================================================
// WebSocket Handlers
// ============================================================================

void WebServerManager::onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                                  AwsEventType type, void* arg, uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.printf("[Web] WS client #%u connected\n", client->id());
            break;
            
        case WS_EVT_DISCONNECT:
            Serial.printf("[Web] WS client #%u disconnected\n", client->id());
            break;
            
        case WS_EVT_DATA:
            handleWsMessage(client, data, len);
            break;
            
        case WS_EVT_PONG:
            break;
            
        case WS_EVT_ERROR:
            Serial.printf("[Web] WS error on client #%u\n", client->id());
            break;
    }
}

void WebServerManager::handleWsMessage(AsyncWebSocketClient* client, uint8_t* data, size_t len) {
    // Parse incoming WebSocket message
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, data, len);
    
    if (error) {
        return;
    }
    
    const char* type = doc["type"];
    if (!type) return;
    
    if (strcmp(type, "ping") == 0) {
        client->text("{\"type\":\"pong\"}");
    } else if (strcmp(type, "getStatus") == 0) {
        String status = "{\"type\":\"status\",\"data\":" + generateStatusJson() + "}";
        client->text(status);
    } else if (strcmp(type, "getTelemetry") == 0) {
        if (vescMgr) {
            String telem = "{\"type\":\"telemetry\",\"data\":" + generateTelemetryJson(vescMgr->getTelemetry()) + "}";
            client->text(telem);
        }
    }
}

// ============================================================================
// Telemetry Broadcasting
// ============================================================================

void WebServerManager::broadcastTelemetry(const VescTelemetry& telemetry) {
    if (!ws || ws->count() == 0) {
        return;
    }
    
    String json = "{\"type\":\"telemetry\",\"data\":" + generateTelemetryJson(telemetry) + "}";
    ws->textAll(json);
}

String WebServerManager::generateTelemetryJson(const VescTelemetry& telemetry) {
    JsonDocument doc;
    
    doc["voltage"] = telemetry.voltage;
    doc["currentIn"] = telemetry.currentIn;
    doc["currentMotor"] = telemetry.currentMotor;
    doc["peakCurrent"] = telemetry.peakCurrent;
    doc["rpm"] = telemetry.rpm;
    doc["ampHours"] = telemetry.ampHours;
    doc["ampHoursCharged"] = telemetry.ampHoursCharged;
    doc["wattHours"] = telemetry.wattHours;
    doc["wattHoursCharged"] = telemetry.wattHoursCharged;
    doc["tempFet"] = telemetry.tempFet;
    doc["tempMotor"] = telemetry.tempMotor;
    doc["duty"] = telemetry.dutyNow;
    doc["cellCount"] = telemetry.cellCount;
    doc["cellVoltage"] = telemetry.cellVoltage;
    doc["batteryPercent"] = telemetry.batteryPercent;
    doc["faultCode"] = telemetry.faultCode;
    doc["faultString"] = VescProtocol::faultCodeToString(telemetry.faultCode);
    doc["tachometer"] = telemetry.tachometer;
    doc["tachometerAbs"] = telemetry.tachometerAbs;
    doc["valid"] = telemetry.valid;
    doc["timestamp"] = millis();
    
    // PPM/ADC input values
    doc["ppmValue"] = telemetry.ppmValue;
    doc["adcValue"] = telemetry.adcValue;
    doc["adcValue2"] = telemetry.adcValue2;
    doc["ppmValid"] = telemetry.ppmValid;
    doc["adcValid"] = telemetry.adcValid;
    
    // Calculate power
    doc["power"] = telemetry.voltage * telemetry.currentIn;
    
    String output;
    serializeJson(doc, output);
    return output;
}

String WebServerManager::generateStatusJson() {
    JsonDocument doc;
    
    doc["connected"] = (vescMgr && vescMgr->isConnected());
    
    if (vescMgr) {
        const BLEDeviceInfo& device = vescMgr->getConnectedDevice();
        doc["deviceName"] = device.name;
        doc["deviceAddress"] = device.address;
        doc["rssi"] = device.rssi;
        
        BLEState state = vescMgr->getState();
        switch (state) {
            case BLE_STATE_IDLE: doc["state"] = "idle"; break;
            case BLE_STATE_SCANNING: doc["state"] = "scanning"; break;
            case BLE_STATE_SCAN_COMPLETE: doc["state"] = "scan_complete"; break;
            case BLE_STATE_CONNECTING: doc["state"] = "connecting"; break;
            case BLE_STATE_CONNECTED: doc["state"] = "connected"; break;
            case BLE_STATE_DISCONNECTED: doc["state"] = "disconnected"; break;
            case BLE_STATE_RECONNECTING: doc["state"] = "reconnecting"; break;
            default: doc["state"] = "unknown"; break;
        }
    }
    
    doc["wsClients"] = getClientCount();
    doc["uptime"] = millis() / 1000;
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["freePsram"] = ESP.getFreePsram();
    
    String output;
    serializeJson(doc, output);
    return output;
}

// ============================================================================
// Update Loop
// ============================================================================

void WebServerManager::update() {
    if (!isRunning || !ws) {
        return;
    }
    
    // Clean up disconnected clients
    ws->cleanupClients();
    
    // Broadcast telemetry at defined interval
    unsigned long now = millis();
    if (now - lastBroadcast >= TELEMETRY_BROADCAST_MS) {
        lastBroadcast = now;
        
        if (vescMgr && vescMgr->hasFreshData()) {
            broadcastTelemetry(vescMgr->getTelemetry());
        }
    }
}

// ============================================================================
// Connection Info
// ============================================================================

int WebServerManager::getClientCount() {
    if (!ws) return 0;
    return ws->count();
}

bool WebServerManager::hasClients() {
    return getClientCount() > 0;
}
