/**
 * @file ui_dashboard.h
 * Dashboard UI Component using M5GFX
 * Displays VESC telemetry on 720p display
 */

#ifndef UI_DASHBOARD_H
#define UI_DASHBOARD_H

#include <Arduino.h>
#include "ui_init.h"

// Forward declaration of telemetry structure
struct VescTelemetry;

// ============================================================================
// Dashboard Component
// ============================================================================

class UIDashboard {
public:
    UIDashboard();
    
    /**
     * @brief Initialize dashboard
     */
    void begin(void);
    
    /**
     * @brief Render full dashboard
     */
    void render(void);
    
    /**
     * @brief Update dashboard with telemetry data
     * @param telemetry VESC telemetry data
     */
    void update(const VescTelemetry& telemetry);
    
    /**
     * @brief Update dashboard with mock data for testing
     */
    void updateMock(void);
    
    /**
     * @brief Update WiFi status display
     * @param status Status string (IP address or connection state)
     */
    void setWifiStatus(const String& status);
    
    /**
     * @brief Update VESC connection status
     * @param connected True if connected to VESC
     * @param deviceName Name of connected device (if any)
     */
    void setVescStatus(bool connected, const String& deviceName = "");
    
    /**
     * @brief Show warning message
     * @param message Warning text to display
     */
    void showWarning(const String& message);
    
    /**
     * @brief Hide warning message
     */
    void hideWarning(void);

private:
    // Status strings
    String wifiStatus;
    String vescStatus;
    bool vescConnected;
    
    // Warning
    String warningMessage;
    bool warningVisible;
    
    // Mock data state
    float mockVoltage;
    float mockCurrent;
    int mockRpm;
    unsigned long lastMockUpdate;
    
    // Cached telemetry for rendering
    float voltage;
    float currentIn;
    float peakCurrent;
    int32_t rpm;
    int batteryPercent;
    int cellCount;
    float cellVoltage;
    float tempFet;
    float tempMotor;
    float duty;
    float ampHours;
    float wattHours;
    float ppmValue;
    bool ppmValid;
    uint8_t faultCode;
    
    // Rendering functions
    void drawStatusBar(M5Canvas* canvas);
    void drawPrimaryMetrics(M5Canvas* canvas);
    void drawSecondaryMetrics(M5Canvas* canvas);
    void drawTemperatures(M5Canvas* canvas);
    void drawInputBar(M5Canvas* canvas);
    void drawWarningBanner(M5Canvas* canvas);
    
    // Helper functions
    void drawCard(M5Canvas* canvas, int x, int y, int w, int h);
    void drawArc(M5Canvas* canvas, int cx, int cy, int r, int thickness, 
                 float startAngle, float endAngle, uint16_t color);
    void drawProgressArc(M5Canvas* canvas, int cx, int cy, int r, int thickness,
                         float value, float maxVal, uint16_t color);
    void drawCenteredText(M5Canvas* canvas, int x, int y, const char* text, 
                          const lgfx::v1::IFont* font, uint16_t color);
    
    float celsiusToFahrenheit(float celsius);
    String formatLargeNumber(int32_t num);
    String formatNumber(float num, int decimals);
};

// Global dashboard instance
extern UIDashboard dashboard;

#endif // UI_DASHBOARD_H
