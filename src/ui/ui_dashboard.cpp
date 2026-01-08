/**
 * @file ui_dashboard.cpp
 * Dashboard UI Implementation using M5GFX
 */

#include "ui_dashboard.h"
#include "../config.h"

// Include telemetry structure
#ifndef VESC_TELEMETRY_DEFINED
struct VescTelemetry {
    float voltage;
    float currentIn;
    float currentMotor;
    float peakCurrent;
    int32_t rpm;
    float ampHours;
    float wattHours;
    float tempFet;
    float tempMotor;
    float dutyNow;
    int cellCount;
    float cellVoltage;
    int batteryPercent;
    uint8_t faultCode;
    float ppmValue;
    float adcValue;
    bool ppmValid;
    bool adcValid;
    bool valid;
};
#endif

// Global instance
UIDashboard dashboard;

// ============================================================================
// Constructor
// ============================================================================

UIDashboard::UIDashboard()
    : vescConnected(false)
    , warningVisible(false)
    , mockVoltage(48.0f)
    , mockCurrent(0.0f)
    , mockRpm(0)
    , lastMockUpdate(0)
    , voltage(0)
    , currentIn(0)
    , peakCurrent(0)
    , rpm(0)
    , batteryPercent(0)
    , cellCount(0)
    , cellVoltage(0)
    , tempFet(0)
    , tempMotor(0)
    , duty(0)
    , ampHours(0)
    , wattHours(0)
    , ppmValue(0)
    , ppmValid(false)
    , faultCode(0)
{
    wifiStatus = "Initializing...";
    vescStatus = "Disconnected";
}

// ============================================================================
// Initialization
// ============================================================================

void UIDashboard::begin(void)
{
    Serial.println("[UI] Dashboard initialized");
}

// ============================================================================
// Rendering
// ============================================================================

void UIDashboard::render(void)
{
    M5Canvas* canvas = ui_get_canvas();
    if (!canvas) return;
    
    // Clear background
    canvas->fillScreen(COLOR_BG_DARK);
    
    // Draw sections
    drawStatusBar(canvas);
    drawPrimaryMetrics(canvas);
    drawSecondaryMetrics(canvas);
    drawTemperatures(canvas);
    drawInputBar(canvas);
    
    if (warningVisible) {
        drawWarningBanner(canvas);
    }
    
    // Push to display
    ui_push();
}

// ============================================================================
// Status Bar
// ============================================================================

void UIDashboard::drawStatusBar(M5Canvas* canvas)
{
    // Status bar background
    canvas->fillRect(0, 0, DISPLAY_WIDTH, 40, COLOR_BG_HEADER);
    
    // Use smaller font that fits the bar
    canvas->setTextSize(1);
    canvas->setFont(&fonts::FreeSans12pt7b);
    
    // WiFi status (left)
    canvas->setTextColor(COLOR_ACCENT_CYAN);
    canvas->drawString(("WiFi: " + wifiStatus).c_str(), 20, 10);
    
    // VESC status (center)
    uint16_t vescColor = vescConnected ? COLOR_ACCENT_GREEN : COLOR_TEXT_DIM;
    canvas->setTextColor(vescColor);
    String vescStr = "VESC: " + vescStatus;
    int textWidth = canvas->textWidth(vescStr.c_str());
    canvas->drawString(vescStr.c_str(), (DISPLAY_WIDTH - textWidth) / 2, 10);
    
    // Title (right)
    canvas->setTextColor(COLOR_TEXT_DIM);
    canvas->drawString("BigDashVesc v1.0", DISPLAY_WIDTH - 220, 10);
}

// ============================================================================
// Primary Metrics
// ============================================================================

void UIDashboard::drawPrimaryMetrics(M5Canvas* canvas)
{
    int startY = 55;
    int cardHeight = 280;
    int cardWidth = 300;
    int spacing = 20;
    
    // ---- VOLTAGE ----
    int x1 = spacing;
    drawCard(canvas, x1, startY, cardWidth, cardHeight);
    
    // Arc background
    drawProgressArc(canvas, x1 + cardWidth/2, startY + 140, 100, 15, 1.0f, 1.0f, COLOR_BG_DARK);
    
    // Voltage arc
    float voltagePercent = (voltage - 30.0f) / 30.0f;  // 30V-60V range
    if (voltagePercent < 0) voltagePercent = 0;
    if (voltagePercent > 1) voltagePercent = 1;
    drawProgressArc(canvas, x1 + cardWidth/2, startY + 140, 100, 15, voltagePercent, 1.0f, COLOR_ACCENT_GREEN);
    
    // Voltage value
    canvas->setTextColor(COLOR_ACCENT_GREEN);
    canvas->setFont(&fonts::FreeSansBold24pt7b);
    String voltStr = formatNumber(voltage, 1);
    int vw = canvas->textWidth(voltStr.c_str());
    canvas->drawString(voltStr.c_str(), x1 + (cardWidth - vw) / 2, startY + 150);
    
    // Unit
    canvas->setTextColor(COLOR_TEXT_DIM);
    canvas->setFont(&fonts::FreeSans12pt7b);
    canvas->drawString("VOLTS", x1 + cardWidth/2 - 30, startY + 200);
    
    // ---- BATTERY ----
    int x2 = x1 + cardWidth + spacing;
    drawCard(canvas, x2, startY, cardWidth, cardHeight);
    
    // Battery percentage
    canvas->setTextColor(COLOR_TEXT_PRIMARY);
    canvas->setFont(&fonts::FreeSansBold24pt7b);
    String battStr = String(batteryPercent) + "%";
    int bw = canvas->textWidth(battStr.c_str());
    canvas->drawString(battStr.c_str(), x2 + (cardWidth - bw) / 2, startY + 80);
    
    // Battery bar
    int barX = x2 + 40;
    int barY = startY + 130;
    int barW = cardWidth - 80;
    int barH = 30;
    canvas->fillRoundRect(barX, barY, barW, barH, 8, COLOR_BG_DARK);
    
    uint16_t battColor = COLOR_ACCENT_GREEN;
    if (batteryPercent < 20) {
        battColor = COLOR_CRIT_RED;
    } else if (batteryPercent < 40) {
        battColor = COLOR_WARN_YELLOW;
    }
    int fillW = (barW * batteryPercent) / 100;
    if (fillW > 0) {
        canvas->fillRoundRect(barX, barY, fillW, barH, 8, battColor);
    }
    
    // Cell info
    canvas->setTextColor(COLOR_TEXT_DIM);
    canvas->setFont(&fonts::FreeSans12pt7b);
    String cellStr = String(cellCount) + "S @ " + formatNumber(cellVoltage, 2) + "V/cell";
    canvas->drawString(cellStr.c_str(), x2 + 40, startY + 200);
    
    canvas->drawString("BATTERY", x2 + cardWidth/2 - 45, startY + 250);
    
    // ---- CURRENT ----
    int x3 = x2 + cardWidth + spacing;
    drawCard(canvas, x3, startY, cardWidth, cardHeight);
    
    // Current arc background
    drawProgressArc(canvas, x3 + cardWidth/2, startY + 140, 100, 15, 1.0f, 1.0f, COLOR_BG_DARK);
    
    // Current arc
    float currentPercent = abs(currentIn) / 100.0f;  // 0-100A range
    if (currentPercent > 1) currentPercent = 1;
    drawProgressArc(canvas, x3 + cardWidth/2, startY + 140, 100, 15, currentPercent, 1.0f, COLOR_ACCENT_CYAN);
    
    // Current value
    canvas->setTextColor(COLOR_ACCENT_CYAN);
    canvas->setFont(&fonts::FreeSansBold24pt7b);
    String curStr = formatNumber(currentIn, 1);
    int cw = canvas->textWidth(curStr.c_str());
    canvas->drawString(curStr.c_str(), x3 + (cardWidth - cw) / 2, startY + 150);
    
    // Unit and peak
    canvas->setTextColor(COLOR_TEXT_DIM);
    canvas->setFont(&fonts::FreeSans12pt7b);
    canvas->drawString("AMPS", x3 + cardWidth/2 - 30, startY + 200);
    
    canvas->setTextColor(COLOR_WARN_YELLOW);
    canvas->setFont(&fonts::FreeSans9pt7b);
    String peakStr = "Peak: " + formatNumber(peakCurrent, 1) + "A";
    canvas->drawString(peakStr.c_str(), x3 + cardWidth/2 - 50, startY + 250);
    
    // ---- POWER ----
    int x4 = x3 + cardWidth + spacing;
    drawCard(canvas, x4, startY, cardWidth, cardHeight);
    
    float power = voltage * currentIn;
    canvas->setTextColor(COLOR_WARN_YELLOW);
    canvas->setFont(&fonts::FreeSansBold24pt7b);
    String powerStr = formatNumber(power, 0);
    int pw = canvas->textWidth(powerStr.c_str());
    canvas->drawString(powerStr.c_str(), x4 + (cardWidth - pw) / 2, startY + 100);
    
    canvas->setTextColor(COLOR_TEXT_DIM);
    canvas->setFont(&fonts::FreeSans12pt7b);
    canvas->drawString("WATTS", x4 + cardWidth/2 - 35, startY + 150);
    
    // ERPM below power
    canvas->setTextColor(COLOR_TEXT_DIM);
    canvas->setFont(&fonts::FreeSans9pt7b);
    canvas->drawString("ERPM", x4 + cardWidth/2 - 25, startY + 200);
    
    canvas->setTextColor(COLOR_TEXT_PRIMARY);
    canvas->setFont(&fonts::FreeSansBold18pt7b);
    String rpmStr = formatLargeNumber(rpm);
    int rw = canvas->textWidth(rpmStr.c_str());
    canvas->drawString(rpmStr.c_str(), x4 + (cardWidth - rw) / 2, startY + 240);
}

// ============================================================================
// Secondary Metrics
// ============================================================================

void UIDashboard::drawSecondaryMetrics(M5Canvas* canvas)
{
    int startY = 370;
    int cardHeight = 100;
    int cardWidth = 180;
    int spacing = 20;
    int x = spacing;
    
    // Duty
    drawCard(canvas, x, startY, cardWidth, cardHeight);
    canvas->setTextColor(COLOR_TEXT_DIM);
    canvas->setFont(&fonts::FreeSans9pt7b);
    canvas->drawString("DUTY", x + 20, startY + 20);
    canvas->setTextColor(COLOR_TEXT_PRIMARY);
    canvas->setFont(&fonts::FreeSansBold18pt7b);
    String dutyStr = formatNumber(duty * 100, 0) + "%";
    canvas->drawString(dutyStr.c_str(), x + 20, startY + 60);
    
    x += cardWidth + spacing;
    
    // mAh Used
    drawCard(canvas, x, startY, cardWidth, cardHeight);
    canvas->setTextColor(COLOR_TEXT_DIM);
    canvas->setFont(&fonts::FreeSans9pt7b);
    canvas->drawString("mAh USED", x + 20, startY + 20);
    canvas->setTextColor(COLOR_TEXT_PRIMARY);
    canvas->setFont(&fonts::FreeSansBold18pt7b);
    String mahStr = formatLargeNumber((int32_t)(ampHours * 1000));
    canvas->drawString(mahStr.c_str(), x + 20, startY + 60);
    
    x += cardWidth + spacing;
    
    // Wh Used
    drawCard(canvas, x, startY, cardWidth, cardHeight);
    canvas->setTextColor(COLOR_TEXT_DIM);
    canvas->setFont(&fonts::FreeSans9pt7b);
    canvas->drawString("Wh USED", x + 20, startY + 20);
    canvas->setTextColor(COLOR_TEXT_PRIMARY);
    canvas->setFont(&fonts::FreeSansBold18pt7b);
    String whStr = formatNumber(wattHours, 1);
    canvas->drawString(whStr.c_str(), x + 20, startY + 60);
}

// ============================================================================
// Temperatures
// ============================================================================

void UIDashboard::drawTemperatures(M5Canvas* canvas)
{
    int startY = 370;
    int cardHeight = 100;
    int cardWidth = 200;
    int spacing = 20;
    int x = DISPLAY_WIDTH - (cardWidth * 2) - (spacing * 2);
    
    // FET Temp
    drawCard(canvas, x, startY, cardWidth, cardHeight);
    canvas->setTextColor(COLOR_TEXT_DIM);
    canvas->setFont(&fonts::FreeSans9pt7b);
    canvas->drawString("FET TEMP", x + 20, startY + 20);
    
    uint16_t fetColor = COLOR_ACCENT_GREEN;
    if (tempFet >= FET_TEMP_CRITICAL_C) {
        fetColor = COLOR_CRIT_RED;
    } else if (tempFet >= FET_TEMP_WARNING_C) {
        fetColor = COLOR_WARN_YELLOW;
    }
    canvas->setTextColor(fetColor);
    canvas->setFont(&fonts::FreeSansBold18pt7b);
    String fetStr = String((int)celsiusToFahrenheit(tempFet)) + "F";
    canvas->drawString(fetStr.c_str(), x + 20, startY + 60);
    
    x += cardWidth + spacing;
    
    // Motor Temp
    drawCard(canvas, x, startY, cardWidth, cardHeight);
    canvas->setTextColor(COLOR_TEXT_DIM);
    canvas->setFont(&fonts::FreeSans9pt7b);
    canvas->drawString("MOTOR TEMP", x + 20, startY + 20);
    
    uint16_t motorColor = COLOR_TEXT_PRIMARY;
    if (tempMotor >= MOTOR_TEMP_WARNING_C) {
        motorColor = COLOR_WARN_YELLOW;
    }
    canvas->setTextColor(motorColor);
    canvas->setFont(&fonts::FreeSansBold18pt7b);
    String motorStr = String((int)celsiusToFahrenheit(tempMotor)) + "F";
    canvas->drawString(motorStr.c_str(), x + 20, startY + 60);
}

// ============================================================================
// Input Bar (PPM/ADC)
// ============================================================================

void UIDashboard::drawInputBar(M5Canvas* canvas)
{
    int x = 20;
    int y = 490;
    int w = 400;
    int h = 80;
    
    drawCard(canvas, x, y, w, h);
    
    // Label
    canvas->setTextColor(COLOR_TEXT_DIM);
    canvas->setFont(&fonts::FreeSans9pt7b);
    String inputLabel = ppmValid ? "PPM INPUT" : "NO INPUT";
    canvas->drawString(inputLabel.c_str(), x + 15, y + 25);
    
    // Value
    if (ppmValid) {
        canvas->setTextColor(COLOR_ACCENT_GREEN);
        canvas->setFont(&fonts::FreeSansBold12pt7b);
        String valStr = formatNumber(ppmValue * 100, 0) + "%";
        canvas->drawString(valStr.c_str(), x + w - 80, y + 25);
    }
    
    // Bidirectional bar
    int barX = x + 20;
    int barY = y + 50;
    int barW = w - 40;
    int barH = 15;
    
    canvas->fillRoundRect(barX, barY, barW, barH, 4, COLOR_BG_DARK);
    
    // Center line
    canvas->drawFastVLine(barX + barW/2, barY, barH, COLOR_TEXT_DIM);
    
    // Fill
    if (ppmValid) {
        int fillW = (int)(abs(ppmValue) * barW / 2);
        int fillX = barX + barW / 2;
        
        if (ppmValue >= 0) {
            canvas->fillRect(fillX, barY, fillW, barH, COLOR_ACCENT_GREEN);
        } else {
            fillX -= fillW;
            canvas->fillRect(fillX, barY, fillW, barH, COLOR_WARN_YELLOW);
        }
    }
}

// ============================================================================
// Warning Banner
// ============================================================================

void UIDashboard::drawWarningBanner(M5Canvas* canvas)
{
    int y = 600;
    int h = 60;
    int margin = 20;
    
    canvas->fillRoundRect(margin, y, DISPLAY_WIDTH - margin * 2, h, 10, COLOR_CRIT_RED);
    
    // Center text vertically and horizontally
    canvas->setTextColor(COLOR_TEXT_PRIMARY);
    canvas->setFont(&fonts::FreeSansBold12pt7b);
    int tw = canvas->textWidth(warningMessage.c_str());
    // Position text centered in the banner (y + h/2 + font offset)
    canvas->drawString(warningMessage.c_str(), (DISPLAY_WIDTH - tw) / 2, y + 38);
}

// ============================================================================
// Helper Functions
// ============================================================================

void UIDashboard::drawCard(M5Canvas* canvas, int x, int y, int w, int h)
{
    canvas->fillRoundRect(x, y, w, h, 12, COLOR_BG_CARD);
}

void UIDashboard::drawProgressArc(M5Canvas* canvas, int cx, int cy, int r, int thickness,
                                   float value, float maxVal, uint16_t color)
{
    float startAngle = 135;
    float totalAngle = 270;
    float endAngle = startAngle + (totalAngle * value / maxVal);
    
    // Draw arc using filled triangles
    float angleStep = 3.0f;
    for (float a = startAngle; a < endAngle; a += angleStep) {
        float rad1 = a * PI / 180.0f;
        float rad2 = (a + angleStep) * PI / 180.0f;
        
        int x1o = cx + (r) * cos(rad1);
        int y1o = cy + (r) * sin(rad1);
        int x1i = cx + (r - thickness) * cos(rad1);
        int y1i = cy + (r - thickness) * sin(rad1);
        
        int x2o = cx + (r) * cos(rad2);
        int y2o = cy + (r) * sin(rad2);
        int x2i = cx + (r - thickness) * cos(rad2);
        int y2i = cy + (r - thickness) * sin(rad2);
        
        canvas->fillTriangle(x1o, y1o, x1i, y1i, x2o, y2o, color);
        canvas->fillTriangle(x2o, y2o, x1i, y1i, x2i, y2i, color);
    }
}

float UIDashboard::celsiusToFahrenheit(float celsius)
{
    return (celsius * 9.0f / 5.0f) + 32.0f;
}

String UIDashboard::formatLargeNumber(int32_t num)
{
    if (abs(num) >= 100000) {
        return String(num / 1000) + "k";
    }
    if (abs(num) >= 10000) {
        return String(num / 1000.0f, 1) + "k";
    }
    
    // Add thousands separator
    String result = "";
    String numStr = String(abs(num));
    int len = numStr.length();
    for (int i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) {
            result += ",";
        }
        result += numStr[i];
    }
    if (num < 0) {
        result = "-" + result;
    }
    return result;
}

String UIDashboard::formatNumber(float num, int decimals)
{
    return String(num, decimals);
}

// ============================================================================
// Update Functions
// ============================================================================

void UIDashboard::update(const VescTelemetry& telemetry)
{
    voltage = telemetry.voltage;
    currentIn = telemetry.currentIn;
    peakCurrent = telemetry.peakCurrent;
    rpm = telemetry.rpm;
    batteryPercent = telemetry.batteryPercent;
    cellCount = telemetry.cellCount;
    cellVoltage = telemetry.cellVoltage;
    tempFet = telemetry.tempFet;
    tempMotor = telemetry.tempMotor;
    duty = telemetry.dutyNow;
    ampHours = telemetry.ampHours;
    wattHours = telemetry.wattHours;
    ppmValue = telemetry.ppmValue;
    ppmValid = telemetry.ppmValid;
    faultCode = telemetry.faultCode;
    
    // Warnings
    if (faultCode != 0) {
        showWarning("VESC FAULT DETECTED!");
    } else if (tempFet >= FET_TEMP_CRITICAL_C) {
        showWarning("WARNING: FET Temperature Critical!");
    } else if (batteryPercent <= 10) {
        showWarning("WARNING: Battery Low!");
    } else {
        hideWarning();
    }
    
    render();
}

void UIDashboard::updateMock(void)
{
    unsigned long now = millis();
    if (now - lastMockUpdate < 50) {  // 20 FPS for smoother updates
        return;
    }
    lastMockUpdate = now;
    
    // Simulate voltage discharge
    mockVoltage -= 0.001f;
    if (mockVoltage < 36.0f) mockVoltage = 50.4f;
    voltage = mockVoltage + (random(-10, 10) / 100.0f);
    
    // Simulate varying current
    mockCurrent = 10.0f + sin(now / 1000.0f) * 8.0f + (random(-100, 100) / 100.0f);
    currentIn = mockCurrent;
    
    static float maxPeak = 0;
    if (abs(mockCurrent) > maxPeak) maxPeak = abs(mockCurrent);
    peakCurrent = maxPeak;
    
    // Simulate RPM
    mockRpm = (int)(3000 + sin(now / 2000.0f) * 2000 + random(-100, 100));
    rpm = mockRpm;
    
    // Battery calculations
    cellCount = 12;
    cellVoltage = voltage / cellCount;
    batteryPercent = (int)((cellVoltage - CELL_VOLTAGE_EMPTY) / (CELL_VOLTAGE_FULL - CELL_VOLTAGE_EMPTY) * 100.0f);
    if (batteryPercent < 0) batteryPercent = 0;
    if (batteryPercent > 100) batteryPercent = 100;
    
    // Other values
    duty = abs(mockCurrent) / 100.0f;
    ampHours = 1.25f + (50.4f - mockVoltage) * 0.5f;
    wattHours = ampHours * 48.0f;
    tempFet = 45.0f + abs(mockCurrent) * 0.3f;
    tempMotor = 55.0f + abs(mockCurrent) * 0.4f;
    
    // PPM input simulation
    ppmValid = true;
    ppmValue = sin(now / 3000.0f) * 0.5f;
    
    faultCode = 0;
    
    render();
}

void UIDashboard::setWifiStatus(const String& status)
{
    wifiStatus = status;
}

void UIDashboard::setVescStatus(bool connected, const String& deviceName)
{
    vescConnected = connected;
    if (connected) {
        vescStatus = deviceName.length() > 0 ? deviceName : "Connected";
    } else {
        vescStatus = deviceName.length() > 0 ? deviceName : "Disconnected";
    }
}

void UIDashboard::showWarning(const String& message)
{
    warningMessage = message;
    warningVisible = true;
}

void UIDashboard::hideWarning(void)
{
    warningVisible = false;
}
