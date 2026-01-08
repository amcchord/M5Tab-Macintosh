/**
 * @file vesc_protocol.cpp
 * VESC UART Protocol Implementation
 */

#include "vesc_protocol.h"

// ============================================================================
// Constructor
// ============================================================================

VescProtocol::VescProtocol() : waitingForPacket(false) {
    rxBuffer.reserve(256);
}

// ============================================================================
// CRC16 Calculation (CRC-16-CCITT)
// ============================================================================

uint16_t VescProtocol::crc16(const uint8_t* data, uint32_t len) {
    uint16_t crc = 0;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i] << 8;
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

// ============================================================================
// Packet Building
// ============================================================================

void VescProtocol::buildPacket(uint8_t* buffer, size_t* length, uint8_t command) {
    buildPacket(buffer, length, command, nullptr, 0);
}

void VescProtocol::buildPacket(uint8_t* buffer, size_t* length, uint8_t command, 
                                const uint8_t* payload, size_t payloadLen) {
    size_t idx = 0;
    size_t totalPayloadLen = 1 + payloadLen;  // command byte + payload
    
    if (totalPayloadLen <= 255) {
        // Short packet
        buffer[idx++] = VESC_PACKET_START_SHORT;
        buffer[idx++] = (uint8_t)totalPayloadLen;
    } else {
        // Long packet
        buffer[idx++] = VESC_PACKET_START_LONG;
        buffer[idx++] = (totalPayloadLen >> 8) & 0xFF;
        buffer[idx++] = totalPayloadLen & 0xFF;
    }
    
    // Command byte
    buffer[idx++] = command;
    
    // Payload
    if (payload != nullptr && payloadLen > 0) {
        memcpy(&buffer[idx], payload, payloadLen);
        idx += payloadLen;
    }
    
    // Calculate CRC over command + payload
    size_t crcStartIdx = (totalPayloadLen <= 255) ? 2 : 3;
    uint16_t checksum = crc16(&buffer[crcStartIdx], totalPayloadLen);
    buffer[idx++] = (checksum >> 8) & 0xFF;
    buffer[idx++] = checksum & 0xFF;
    
    // Stop byte
    buffer[idx++] = VESC_PACKET_STOP;
    
    *length = idx;
}

void VescProtocol::buildGetValuesPacket(uint8_t* buffer, size_t* length) {
    buildPacket(buffer, length, COMM_GET_VALUES);
}

void VescProtocol::buildAlivePacket(uint8_t* buffer, size_t* length) {
    buildPacket(buffer, length, COMM_ALIVE);
}

void VescProtocol::buildGetMcconfPacket(uint8_t* buffer, size_t* length) {
    buildPacket(buffer, length, COMM_GET_MCCONF);
}

void VescProtocol::buildGetAppconfPacket(uint8_t* buffer, size_t* length) {
    buildPacket(buffer, length, COMM_GET_APPCONF);
}

void VescProtocol::buildGetDecodedPpmPacket(uint8_t* buffer, size_t* length) {
    buildPacket(buffer, length, COMM_GET_DECODED_PPM);
}

void VescProtocol::buildGetDecodedAdcPacket(uint8_t* buffer, size_t* length) {
    buildPacket(buffer, length, COMM_GET_DECODED_ADC);
}

// ============================================================================
// Packet Receiving
// ============================================================================

bool VescProtocol::addReceivedByte(uint8_t byte) {
    rxBuffer.push_back(byte);
    return hasCompletePacket();
}

void VescProtocol::clearBuffer() {
    rxBuffer.clear();
    waitingForPacket = false;
}

bool VescProtocol::hasCompletePacket() {
    // Need at least 6 bytes for minimum packet
    if (rxBuffer.size() < 6) {
        return false;
    }
    
    // Look for start byte
    while (!rxBuffer.empty() && rxBuffer[0] != VESC_PACKET_START_SHORT && 
           rxBuffer[0] != VESC_PACKET_START_LONG) {
        rxBuffer.erase(rxBuffer.begin());
    }
    
    if (rxBuffer.empty()) {
        return false;
    }
    
    // Determine packet length
    size_t payloadLen = 0;
    size_t headerLen = 0;
    
    if (rxBuffer[0] == VESC_PACKET_START_SHORT) {
        if (rxBuffer.size() < 2) return false;
        payloadLen = rxBuffer[1];
        headerLen = 2;
    } else {
        if (rxBuffer.size() < 3) return false;
        payloadLen = (rxBuffer[1] << 8) | rxBuffer[2];
        headerLen = 3;
    }
    
    // Total packet size: header + payload + CRC(2) + stop(1)
    size_t totalLen = headerLen + payloadLen + 3;
    
    if (rxBuffer.size() < totalLen) {
        return false;
    }
    
    // Check stop byte
    if (rxBuffer[totalLen - 1] != VESC_PACKET_STOP) {
        // Invalid packet, remove first byte and try again
        rxBuffer.erase(rxBuffer.begin());
        return hasCompletePacket();
    }
    
    return true;
}

// ============================================================================
// Packet Parsing
// ============================================================================

int16_t VescProtocol::readInt16(const uint8_t* data, size_t offset) {
    return (int16_t)((data[offset] << 8) | data[offset + 1]);
}

int32_t VescProtocol::readInt32(const uint8_t* data, size_t offset) {
    return (int32_t)((data[offset] << 24) | (data[offset + 1] << 16) | 
                     (data[offset + 2] << 8) | data[offset + 3]);
}

uint16_t VescProtocol::readUint16(const uint8_t* data, size_t offset) {
    return (uint16_t)((data[offset] << 8) | data[offset + 1]);
}

uint32_t VescProtocol::readUint32(const uint8_t* data, size_t offset) {
    return (uint32_t)((data[offset] << 24) | (data[offset + 1] << 16) | 
                      (data[offset + 2] << 8) | data[offset + 3]);
}

bool VescProtocol::parsePacket(VescTelemetry& telemetry) {
    if (!hasCompletePacket()) {
        return false;
    }
    
    // Get packet info
    size_t payloadLen = 0;
    size_t headerLen = 0;
    
    if (rxBuffer[0] == VESC_PACKET_START_SHORT) {
        payloadLen = rxBuffer[1];
        headerLen = 2;
    } else {
        payloadLen = (rxBuffer[1] << 8) | rxBuffer[2];
        headerLen = 3;
    }
    
    // Verify CRC
    size_t totalLen = headerLen + payloadLen + 3;
    uint16_t receivedCrc = (rxBuffer[totalLen - 3] << 8) | rxBuffer[totalLen - 2];
    uint16_t calculatedCrc = crc16(&rxBuffer[headerLen], payloadLen);
    
    if (receivedCrc != calculatedCrc) {
        Serial.printf("[VESC] CRC mismatch: received 0x%04X, calculated 0x%04X\n", 
                      receivedCrc, calculatedCrc);
        rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + totalLen);
        return false;
    }
    
    // Get command byte
    uint8_t command = rxBuffer[headerLen];
    
    // Parse based on command
    if (command == COMM_GET_VALUES) {
        // COMM_GET_VALUES response
        const uint8_t* data = &rxBuffer[headerLen];
        
        // Check minimum payload size (command + 52 bytes of data)
        if (payloadLen >= 53) {
            // Payload data starts at index 1 (after command byte)
            const uint8_t* telem = &data[1];
            
            telemetry.tempFet = readInt16(telem, 0) / 10.0f;
            telemetry.tempMotor = readInt16(telem, 2) / 10.0f;
            telemetry.currentMotor = readInt32(telem, 4) / 100.0f;
            telemetry.currentIn = readInt32(telem, 8) / 100.0f;
            telemetry.currentId = readInt32(telem, 12) / 100.0f;
            telemetry.currentIq = readInt32(telem, 16) / 100.0f;
            telemetry.dutyNow = readInt16(telem, 20) / 1000.0f;
            telemetry.rpm = readInt32(telem, 22);
            telemetry.voltage = readInt16(telem, 26) / 10.0f;
            telemetry.ampHours = readInt32(telem, 28) / 10000.0f;
            telemetry.ampHoursCharged = readInt32(telem, 32) / 10000.0f;
            telemetry.wattHours = readInt32(telem, 36) / 10000.0f;
            telemetry.wattHoursCharged = readInt32(telem, 40) / 10000.0f;
            telemetry.tachometer = readInt32(telem, 44);
            telemetry.tachometerAbs = readInt32(telem, 48);
            telemetry.faultCode = telem[52];
            
            // Calculate derived values
            telemetry.cellCount = estimateCellCount(telemetry.voltage);
            if (telemetry.cellCount > 0) {
                telemetry.cellVoltage = telemetry.voltage / telemetry.cellCount;
                telemetry.batteryPercent = calculateBatteryPercent(telemetry.voltage, telemetry.cellCount);
            }
            
            telemetry.lastUpdate = millis();
            telemetry.valid = true;
            
            Serial.printf("[VESC] Parsed: V=%.1f, I=%.1f, RPM=%d, FET=%.1f°C\n",
                         telemetry.voltage, telemetry.currentIn, telemetry.rpm, telemetry.tempFet);
        }
    } else if (command == COMM_GET_DECODED_PPM) {
        // COMM_GET_DECODED_PPM response
        const uint8_t* data = &rxBuffer[headerLen];
        
        if (payloadLen >= 5) {
            const uint8_t* ppmData = &data[1];
            int32_t ppmRaw = readInt32(ppmData, 0);
            telemetry.ppmValue = ppmRaw / 1000000.0f;
            telemetry.ppmValid = true;
            telemetry.ppmLastUpdate = millis();
            
            Serial.printf("[VESC] PPM: %.3f\n", telemetry.ppmValue);
        }
    } else if (command == COMM_GET_DECODED_ADC) {
        // COMM_GET_DECODED_ADC response
        const uint8_t* data = &rxBuffer[headerLen];
        
        if (payloadLen >= 9) {
            const uint8_t* adcData = &data[1];
            int32_t adcRaw = readInt32(adcData, 0);
            telemetry.adcValue = adcRaw / 1000000.0f;
            
            // If we have secondary ADC data
            if (payloadLen >= 17) {
                int32_t adc2Raw = readInt32(adcData, 8);
                telemetry.adcValue2 = adc2Raw / 1000000.0f;
            }
            
            telemetry.adcValid = true;
            telemetry.adcLastUpdate = millis();
            
            Serial.printf("[VESC] ADC: %.3f, ADC2: %.3f\n", telemetry.adcValue, telemetry.adcValue2);
        }
    }
    
    // Remove processed packet from buffer
    rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + totalLen);
    
    return telemetry.valid;
}

// ============================================================================
// Battery Estimation
// ============================================================================

int VescProtocol::estimateCellCount(float voltage) {
    if (voltage < 5.0f) {
        return 0;  // Invalid or no battery
    }
    
    // Common configurations: 6S, 8S, 10S, 12S, 13S, 14S
    // Estimate based on nominal voltage range
    int estimated = (int)round(voltage / CELL_VOLTAGE_ESTIMATE);
    
    // Clamp to reasonable values
    if (estimated < 3) estimated = 3;
    if (estimated > 20) estimated = 20;
    
    return estimated;
}

int VescProtocol::estimateBatteryPercent(float cellVoltage) {
    // Calculate percentage based on cell voltage
    // Empty: 3.0V, Full: 4.2V
    float percent = (cellVoltage - CELL_VOLTAGE_EMPTY) / (CELL_VOLTAGE_FULL - CELL_VOLTAGE_EMPTY) * 100.0f;
    
    // Clamp to 0-100
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    
    return (int)percent;
}

int VescProtocol::calculateBatteryPercent(float voltage, int cellCount) {
    if (cellCount <= 0) {
        return 0;
    }
    
    float cellVoltage = voltage / cellCount;
    return estimateBatteryPercent(cellVoltage);
}

// ============================================================================
// Fault Code Strings
// ============================================================================

const char* VescProtocol::faultCodeToString(uint8_t faultCode) {
    switch (faultCode) {
        case FAULT_CODE_NONE: return "None";
        case FAULT_CODE_OVER_VOLTAGE: return "Over Voltage";
        case FAULT_CODE_UNDER_VOLTAGE: return "Under Voltage";
        case FAULT_CODE_DRV: return "DRV Error";
        case FAULT_CODE_ABS_OVER_CURRENT: return "Over Current";
        case FAULT_CODE_OVER_TEMP_FET: return "FET Over Temp";
        case FAULT_CODE_OVER_TEMP_MOTOR: return "Motor Over Temp";
        case FAULT_CODE_GATE_DRIVER_OVER_VOLTAGE: return "Gate Driver Over Voltage";
        case FAULT_CODE_GATE_DRIVER_UNDER_VOLTAGE: return "Gate Driver Under Voltage";
        case FAULT_CODE_MCU_UNDER_VOLTAGE: return "MCU Under Voltage";
        case FAULT_CODE_BOOTING_FROM_WATCHDOG_RESET: return "Watchdog Reset";
        case FAULT_CODE_ENCODER_SPI: return "Encoder SPI";
        case FAULT_CODE_ENCODER_SINCOS_BELOW_MIN_AMPLITUDE: return "Encoder Low Amplitude";
        case FAULT_CODE_ENCODER_SINCOS_ABOVE_MAX_AMPLITUDE: return "Encoder High Amplitude";
        case FAULT_CODE_FLASH_CORRUPTION: return "Flash Corruption";
        case FAULT_CODE_HIGH_OFFSET_CURRENT_SENSOR_1: return "Current Sensor 1 Offset";
        case FAULT_CODE_HIGH_OFFSET_CURRENT_SENSOR_2: return "Current Sensor 2 Offset";
        case FAULT_CODE_HIGH_OFFSET_CURRENT_SENSOR_3: return "Current Sensor 3 Offset";
        case FAULT_CODE_UNBALANCED_CURRENTS: return "Unbalanced Currents";
        case FAULT_CODE_BRK: return "BRK Fault";
        case FAULT_CODE_RESOLVER_LOT: return "Resolver LOT";
        case FAULT_CODE_RESOLVER_DOS: return "Resolver DOS";
        case FAULT_CODE_RESOLVER_LOS: return "Resolver LOS";
        case FAULT_CODE_FLASH_CORRUPTION_APP_CFG: return "App Config Corruption";
        case FAULT_CODE_FLASH_CORRUPTION_MC_CFG: return "Motor Config Corruption";
        case FAULT_CODE_ENCODER_NO_MAGNET: return "Encoder No Magnet";
        default: return "Unknown Fault";
    }
}
