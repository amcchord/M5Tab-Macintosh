/**
 * @file vesc_protocol.h
 * VESC UART Protocol Handler
 * Packet building, parsing, and telemetry data structures
 */

#ifndef VESC_PROTOCOL_H
#define VESC_PROTOCOL_H

#include <Arduino.h>
#include <vector>
#include "config.h"

// Define this to indicate VescTelemetry is defined here
#define VESC_TELEMETRY_DEFINED

// ============================================================================
// VESC Telemetry Data Structure
// ============================================================================

struct VescTelemetry {
    float tempFet;              // FET temperature (°C)
    float tempMotor;            // Motor temperature (°C)
    float currentMotor;         // Motor current (A)
    float currentIn;            // Battery current (A)
    float currentId;            // FOC Id current (A)
    float currentIq;            // FOC Iq current (A)
    float dutyNow;              // Duty cycle (0-1)
    int32_t rpm;                // Electrical RPM
    float voltage;              // Input voltage (V)
    float ampHours;             // Amp hours consumed (Ah)
    float ampHoursCharged;      // Amp hours charged (Ah)
    float wattHours;            // Watt hours consumed (Wh)
    float wattHoursCharged;     // Watt hours charged (Wh)
    int32_t tachometer;         // Tachometer value
    int32_t tachometerAbs;      // Absolute tachometer value
    uint8_t faultCode;          // Fault code
    
    // Derived values
    int cellCount;              // Estimated cell count
    float cellVoltage;          // Voltage per cell
    int batteryPercent;         // Battery percentage
    float peakCurrent;          // Peak current over window
    
    // PPM/ADC Input values
    float ppmValue;             // Decoded PPM input (-1.0 to 1.0)
    float adcValue;             // Decoded ADC input (0 to 1.0)
    float adcValue2;            // Secondary ADC (brake)
    bool ppmValid;              // PPM signal detected/valid
    bool adcValid;              // ADC signal detected/valid
    unsigned long ppmLastUpdate;    // Last PPM update time
    unsigned long adcLastUpdate;    // Last ADC update time
    
    // Timestamps
    unsigned long lastUpdate;   // Last update time (millis)
    bool valid;                 // Data is valid
    
    VescTelemetry() {
        reset();
    }
    
    void reset() {
        tempFet = 0;
        tempMotor = 0;
        currentMotor = 0;
        currentIn = 0;
        currentId = 0;
        currentIq = 0;
        dutyNow = 0;
        rpm = 0;
        voltage = 0;
        ampHours = 0;
        ampHoursCharged = 0;
        wattHours = 0;
        wattHoursCharged = 0;
        tachometer = 0;
        tachometerAbs = 0;
        faultCode = 0;
        cellCount = 0;
        cellVoltage = 0;
        batteryPercent = 0;
        peakCurrent = 0;
        ppmValue = 0;
        adcValue = 0;
        adcValue2 = 0;
        ppmValid = false;
        adcValid = false;
        ppmLastUpdate = 0;
        adcLastUpdate = 0;
        lastUpdate = 0;
        valid = false;
    }
};

// ============================================================================
// VESC Fault Codes
// ============================================================================

enum VescFaultCode {
    FAULT_CODE_NONE = 0,
    FAULT_CODE_OVER_VOLTAGE = 1,
    FAULT_CODE_UNDER_VOLTAGE = 2,
    FAULT_CODE_DRV = 3,
    FAULT_CODE_ABS_OVER_CURRENT = 4,
    FAULT_CODE_OVER_TEMP_FET = 5,
    FAULT_CODE_OVER_TEMP_MOTOR = 6,
    FAULT_CODE_GATE_DRIVER_OVER_VOLTAGE = 7,
    FAULT_CODE_GATE_DRIVER_UNDER_VOLTAGE = 8,
    FAULT_CODE_MCU_UNDER_VOLTAGE = 9,
    FAULT_CODE_BOOTING_FROM_WATCHDOG_RESET = 10,
    FAULT_CODE_ENCODER_SPI = 11,
    FAULT_CODE_ENCODER_SINCOS_BELOW_MIN_AMPLITUDE = 12,
    FAULT_CODE_ENCODER_SINCOS_ABOVE_MAX_AMPLITUDE = 13,
    FAULT_CODE_FLASH_CORRUPTION = 14,
    FAULT_CODE_HIGH_OFFSET_CURRENT_SENSOR_1 = 15,
    FAULT_CODE_HIGH_OFFSET_CURRENT_SENSOR_2 = 16,
    FAULT_CODE_HIGH_OFFSET_CURRENT_SENSOR_3 = 17,
    FAULT_CODE_UNBALANCED_CURRENTS = 18,
    FAULT_CODE_BRK = 19,
    FAULT_CODE_RESOLVER_LOT = 20,
    FAULT_CODE_RESOLVER_DOS = 21,
    FAULT_CODE_RESOLVER_LOS = 22,
    FAULT_CODE_FLASH_CORRUPTION_APP_CFG = 23,
    FAULT_CODE_FLASH_CORRUPTION_MC_CFG = 24,
    FAULT_CODE_ENCODER_NO_MAGNET = 25
};

// ============================================================================
// VESC Protocol Class
// ============================================================================

class VescProtocol {
public:
    VescProtocol();
    
    // Packet building
    void buildPacket(uint8_t* buffer, size_t* length, uint8_t command);
    void buildPacket(uint8_t* buffer, size_t* length, uint8_t command, const uint8_t* payload, size_t payloadLen);
    
    // Packet parsing
    bool addReceivedByte(uint8_t byte);
    bool hasCompletePacket();
    bool parsePacket(VescTelemetry& telemetry);
    void clearBuffer();
    
    // Specific packet builders
    void buildGetValuesPacket(uint8_t* buffer, size_t* length);
    void buildAlivePacket(uint8_t* buffer, size_t* length);
    void buildGetMcconfPacket(uint8_t* buffer, size_t* length);
    void buildGetAppconfPacket(uint8_t* buffer, size_t* length);
    void buildGetDecodedPpmPacket(uint8_t* buffer, size_t* length);
    void buildGetDecodedAdcPacket(uint8_t* buffer, size_t* length);
    
    // CRC calculation
    static uint16_t crc16(const uint8_t* data, uint32_t len);
    
    // Fault code to string
    static const char* faultCodeToString(uint8_t faultCode);
    
    // Battery estimation
    static int estimateCellCount(float voltage);
    static int estimateBatteryPercent(float cellVoltage);
    static int calculateBatteryPercent(float voltage, int cellCount);
    
private:
    std::vector<uint8_t> rxBuffer;
    bool waitingForPacket;
    
    // Helper functions
    int16_t readInt16(const uint8_t* data, size_t offset);
    int32_t readInt32(const uint8_t* data, size_t offset);
    uint16_t readUint16(const uint8_t* data, size_t offset);
    uint32_t readUint32(const uint8_t* data, size_t offset);
};

#endif // VESC_PROTOCOL_H
