/**
 * @file storage.cpp
 * NVS Persistent Storage Implementation
 */

#include "storage.h"

// Global instance
StorageManager storage;

// ============================================================================
// Constructor
// ============================================================================

StorageManager::StorageManager() : initialized(false) {
}

// ============================================================================
// Initialization
// ============================================================================

void StorageManager::begin() {
    Serial.println("[Storage] Initializing NVS...");
    
    if (prefs.begin(NVS_NAMESPACE, false)) {
        initialized = true;
        Serial.println("[Storage] NVS initialized successfully");
    } else {
        Serial.println("[Storage] Failed to initialize NVS");
    }
}

// ============================================================================
// Last Connected VESC
// ============================================================================

void StorageManager::saveLastVesc(const String& name, const String& address, bool isRandom) {
    if (!initialized) return;
    
    prefs.putString(NVS_KEY_LAST_VESC, name);
    prefs.putString(NVS_KEY_LAST_VESC_ADDR, address);
    prefs.putBool("last_random", isRandom);
    
    Serial.printf("[Storage] Saved last VESC: %s (%s)\n", name.c_str(), address.c_str());
}

StoredVescInfo StorageManager::getLastVesc() {
    StoredVescInfo info;
    
    if (!initialized) return info;
    
    info.name = prefs.getString(NVS_KEY_LAST_VESC, "");
    info.address = prefs.getString(NVS_KEY_LAST_VESC_ADDR, "");
    info.isRandom = prefs.getBool("last_random", true);
    info.valid = !info.address.isEmpty();
    
    if (info.valid) {
        Serial.printf("[Storage] Retrieved last VESC: %s (%s)\n", 
                      info.name.c_str(), info.address.c_str());
    }
    
    return info;
}

void StorageManager::clearLastVesc() {
    if (!initialized) return;
    
    prefs.remove(NVS_KEY_LAST_VESC);
    prefs.remove(NVS_KEY_LAST_VESC_ADDR);
    prefs.remove("last_random");
    
    Serial.println("[Storage] Cleared last VESC");
}

// ============================================================================
// Generic Key-Value Storage
// ============================================================================

void StorageManager::putString(const char* key, const String& value) {
    if (!initialized) return;
    prefs.putString(key, value);
}

String StorageManager::getString(const char* key, const String& defaultValue) {
    if (!initialized) return defaultValue;
    return prefs.getString(key, defaultValue);
}

void StorageManager::putInt(const char* key, int32_t value) {
    if (!initialized) return;
    prefs.putInt(key, value);
}

int32_t StorageManager::getInt(const char* key, int32_t defaultValue) {
    if (!initialized) return defaultValue;
    return prefs.getInt(key, defaultValue);
}

void StorageManager::putFloat(const char* key, float value) {
    if (!initialized) return;
    prefs.putFloat(key, value);
}

float StorageManager::getFloat(const char* key, float defaultValue) {
    if (!initialized) return defaultValue;
    return prefs.getFloat(key, defaultValue);
}

void StorageManager::putBool(const char* key, bool value) {
    if (!initialized) return;
    prefs.putBool(key, value);
}

bool StorageManager::getBool(const char* key, bool defaultValue) {
    if (!initialized) return defaultValue;
    return prefs.getBool(key, defaultValue);
}

// ============================================================================
// Clear All
// ============================================================================

void StorageManager::clearAll() {
    if (!initialized) return;
    
    prefs.clear();
    Serial.println("[Storage] All data cleared");
}
