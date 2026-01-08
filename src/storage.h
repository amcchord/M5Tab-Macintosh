/**
 * @file storage.h
 * NVS Persistent Storage Manager
 */

#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"

// ============================================================================
// Stored VESC Info
// ============================================================================

struct StoredVescInfo {
    String name;
    String address;
    bool isRandom;
    bool valid;
    
    StoredVescInfo() : isRandom(true), valid(false) {}
};

// ============================================================================
// Storage Manager Class
// ============================================================================

class StorageManager {
public:
    StorageManager();
    
    // Initialization
    void begin();
    
    // Last connected VESC
    void saveLastVesc(const String& name, const String& address, bool isRandom);
    StoredVescInfo getLastVesc();
    void clearLastVesc();
    
    // Generic key-value storage
    void putString(const char* key, const String& value);
    String getString(const char* key, const String& defaultValue = "");
    void putInt(const char* key, int32_t value);
    int32_t getInt(const char* key, int32_t defaultValue = 0);
    void putFloat(const char* key, float value);
    float getFloat(const char* key, float defaultValue = 0.0f);
    void putBool(const char* key, bool value);
    bool getBool(const char* key, bool defaultValue = false);
    
    // Clear all stored data
    void clearAll();
    
private:
    Preferences prefs;
    bool initialized;
};

// Global instance
extern StorageManager storage;

#endif // STORAGE_H
