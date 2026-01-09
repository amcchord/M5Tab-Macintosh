/*
 *  input_esp32.cpp - Input handling for ESP32 with M5Unified
 *
 *  BasiliskII ESP32 Port
 *
 *  Handles:
 *  - Touch panel input (as mouse via M5Unified)
 *  - USB HID keyboard input (future: via ESP-IDF USB Host)
 *  - USB HID mouse input (future: via ESP-IDF USB Host)
 */

#include "sysdeps.h"
#include "input.h"
#include "adb.h"
#include "video.h"

#include <M5Unified.h>

#define DEBUG 0
#include "debug.h"

// ============================================================================
// USB HID Scancode to Mac ADB Keycode Translation Table
// ============================================================================
//
// USB HID scancodes (Usage Page 0x07) map to Mac ADB keycodes.
// This table is based on the SDL2/cocoa keycode mapping from BasiliskII.
// Index = USB HID scancode, Value = Mac ADB keycode (0xFF = invalid/unmapped)
//
// Reference: USB HID Usage Tables, Keyboard/Keypad Page (0x07)
// https://usb.org/sites/default/files/hut1_4.pdf
//

static const uint8_t usb_to_mac_keycode[256] = {
    // 0x00-0x03: Reserved/Error codes
    0xFF, 0xFF, 0xFF, 0xFF,
    
    // 0x04-0x1D: Letters A-Z
    0x00,  // 0x04: A
    0x0B,  // 0x05: B
    0x08,  // 0x06: C
    0x02,  // 0x07: D
    0x0E,  // 0x08: E
    0x03,  // 0x09: F
    0x05,  // 0x0A: G
    0x04,  // 0x0B: H
    0x22,  // 0x0C: I
    0x26,  // 0x0D: J
    0x28,  // 0x0E: K
    0x25,  // 0x0F: L
    0x2E,  // 0x10: M
    0x2D,  // 0x11: N
    0x1F,  // 0x12: O
    0x23,  // 0x13: P
    0x0C,  // 0x14: Q
    0x0F,  // 0x15: R
    0x01,  // 0x16: S
    0x11,  // 0x17: T
    0x20,  // 0x18: U
    0x09,  // 0x19: V
    0x0D,  // 0x1A: W
    0x07,  // 0x1B: X
    0x10,  // 0x1C: Y
    0x06,  // 0x1D: Z
    
    // 0x1E-0x27: Numbers 1-9, 0
    0x12,  // 0x1E: 1
    0x13,  // 0x1F: 2
    0x14,  // 0x20: 3
    0x15,  // 0x21: 4
    0x17,  // 0x22: 5
    0x16,  // 0x23: 6
    0x1A,  // 0x24: 7
    0x1C,  // 0x25: 8
    0x19,  // 0x26: 9
    0x1D,  // 0x27: 0
    
    // 0x28-0x2C: Special keys
    0x24,  // 0x28: Return/Enter
    0x35,  // 0x29: Escape
    0x33,  // 0x2A: Backspace/Delete
    0x30,  // 0x2B: Tab
    0x31,  // 0x2C: Space
    
    // 0x2D-0x38: Punctuation and symbols
    0x1B,  // 0x2D: - (minus)
    0x18,  // 0x2E: = (equals)
    0x21,  // 0x2F: [ (left bracket)
    0x1E,  // 0x30: ] (right bracket)
    0x2A,  // 0x31: \ (backslash)
    0x32,  // 0x32: # (non-US hash) - maps to International
    0x29,  // 0x33: ; (semicolon)
    0x27,  // 0x34: ' (apostrophe)
    0x0A,  // 0x35: ` (grave accent)
    0x2B,  // 0x36: , (comma)
    0x2F,  // 0x37: . (period)
    0x2C,  // 0x38: / (slash)
    
    // 0x39: Caps Lock
    0x39,  // 0x39: Caps Lock
    
    // 0x3A-0x45: Function keys F1-F12
    0x7A,  // 0x3A: F1
    0x78,  // 0x3B: F2
    0x63,  // 0x3C: F3
    0x76,  // 0x3D: F4
    0x60,  // 0x3E: F5
    0x61,  // 0x3F: F6
    0x62,  // 0x40: F7
    0x64,  // 0x41: F8
    0x65,  // 0x42: F9
    0x6D,  // 0x43: F10
    0x67,  // 0x44: F11
    0x6F,  // 0x45: F12
    
    // 0x46-0x48: Print Screen, Scroll Lock, Pause
    0x69,  // 0x46: Print Screen (F13)
    0x6B,  // 0x47: Scroll Lock (F14)
    0x71,  // 0x48: Pause (F15)
    
    // 0x49-0x4E: Navigation cluster
    0x72,  // 0x49: Insert (Help)
    0x73,  // 0x4A: Home
    0x74,  // 0x4B: Page Up
    0x75,  // 0x4C: Delete (Forward Delete)
    0x77,  // 0x4D: End
    0x79,  // 0x4E: Page Down
    
    // 0x4F-0x52: Arrow keys
    0x3C,  // 0x4F: Right Arrow
    0x3B,  // 0x50: Left Arrow
    0x3D,  // 0x51: Down Arrow
    0x3E,  // 0x52: Up Arrow
    
    // 0x53: Num Lock
    0x47,  // 0x53: Num Lock/Clear
    
    // 0x54-0x63: Keypad
    0x4B,  // 0x54: KP /
    0x43,  // 0x55: KP *
    0x4E,  // 0x56: KP -
    0x45,  // 0x57: KP +
    0x4C,  // 0x58: KP Enter
    0x53,  // 0x59: KP 1
    0x54,  // 0x5A: KP 2
    0x55,  // 0x5B: KP 3
    0x56,  // 0x5C: KP 4
    0x57,  // 0x5D: KP 5
    0x58,  // 0x5E: KP 6
    0x59,  // 0x5F: KP 7
    0x5B,  // 0x60: KP 8
    0x5C,  // 0x61: KP 9
    0x52,  // 0x62: KP 0
    0x41,  // 0x63: KP .
    
    // 0x64: Non-US backslash
    0x32,  // 0x64: International
    
    // 0x65: Application/Menu key
    0x32,  // 0x65: Application (-> International)
    
    // 0x66: Power key
    0x7F,  // 0x66: Power
    
    // 0x67: KP =
    0x51,  // 0x67: KP =
    
    // 0x68-0x73: F13-F24 (extended function keys)
    0x69,  // 0x68: F13
    0x6B,  // 0x69: F14
    0x71,  // 0x6A: F15
    0xFF,  // 0x6B: F16
    0xFF,  // 0x6C: F17
    0xFF,  // 0x6D: F18
    0xFF,  // 0x6E: F19
    0xFF,  // 0x6F: F20
    0xFF,  // 0x70: F21
    0xFF,  // 0x71: F22
    0xFF,  // 0x72: F23
    0xFF,  // 0x73: F24
    
    // 0x74-0xDF: Various (mostly unmapped)
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 0x74-0x7B
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 0x7C-0x83
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 0x84-0x8B
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 0x8C-0x93
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 0x94-0x9B
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 0x9C-0xA3
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 0xA4-0xAB
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 0xAC-0xB3
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 0xB4-0xBB
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 0xBC-0xC3
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 0xC4-0xCB
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 0xCC-0xD3
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 0xD4-0xDB
    0xFF, 0xFF, 0xFF, 0xFF,                          // 0xDC-0xDF
    
    // 0xE0-0xE7: Modifier keys (left/right variants)
    0x36,  // 0xE0: Left Control
    0x38,  // 0xE1: Left Shift
    0x3A,  // 0xE2: Left Alt (-> Option)
    0x37,  // 0xE3: Left GUI/Command
    0x36,  // 0xE4: Right Control
    0x38,  // 0xE5: Right Shift
    0x3A,  // 0xE6: Right Alt (-> Option)
    0x37,  // 0xE7: Right GUI/Command
    
    // 0xE8-0xFF: Reserved
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 0xE8-0xEF
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // 0xF0-0xF7
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF   // 0xF8-0xFF
};

// ============================================================================
// Input State
// ============================================================================

// Mac screen dimensions for coordinate scaling
static int mac_screen_width = 640;
static int mac_screen_height = 360;

// Display dimensions (from M5.Display)
static int display_width = 1280;
static int display_height = 720;

// Input enable flags
static bool touch_enabled = true;
static bool keyboard_enabled = true;

// Touch state
static bool touch_was_pressed = false;
static int last_touch_x = 0;
static int last_touch_y = 0;
static uint32_t touch_press_time = 0;

// Click timing constants (in milliseconds)
static const uint32_t CLICK_HOLD_THRESHOLD = 200;  // Time to register as click vs drag

// Keyboard state tracking (for detecting key up events)
// USB HID reports up to 6 simultaneous keys (boot protocol)
static uint8_t prev_keys[6] = {0, 0, 0, 0, 0, 0};
static uint8_t prev_modifiers = 0;

// USB device connection state
static bool keyboard_connected = false;
static bool mouse_connected = false;

// ============================================================================
// Touch Input Handling
// ============================================================================

/*
 *  Convert display coordinates to Mac screen coordinates
 *  Display is 1280x720, Mac screen is 640x360 (2x scale factor)
 */
static void convertTouchToMac(int touch_x, int touch_y, int *mac_x, int *mac_y)
{
    // Scale from display coordinates to Mac coordinates
    *mac_x = (touch_x * mac_screen_width) / display_width;
    *mac_y = (touch_y * mac_screen_height) / display_height;
    
    // Clamp to valid range
    if (*mac_x < 0) *mac_x = 0;
    if (*mac_x >= mac_screen_width) *mac_x = mac_screen_width - 1;
    if (*mac_y < 0) *mac_y = 0;
    if (*mac_y >= mac_screen_height) *mac_y = mac_screen_height - 1;
}

/*
 *  Process touch panel input
 *  Called from InputPoll() to handle touch events
 */
static void processTouchInput(void)
{
    if (!touch_enabled) return;
    
    // Get touch state from M5Unified
    auto touch_detail = M5.Touch.getDetail();
    
    bool is_pressed = touch_detail.isPressed();
    int touch_x = touch_detail.x;
    int touch_y = touch_detail.y;
    
    // Convert to Mac coordinates
    int mac_x, mac_y;
    convertTouchToMac(touch_x, touch_y, &mac_x, &mac_y);
    
    if (is_pressed) {
        if (!touch_was_pressed) {
            // Touch just started
            touch_press_time = millis();
            touch_was_pressed = true;
            
            // Move cursor to touch position
            ADBMouseMoved(mac_x, mac_y);
            
            // Immediately press mouse button for responsive feel
            ADBMouseDown(0);
            
            D(bug("[INPUT] Touch down at (%d, %d) -> Mac (%d, %d)\n", 
                  touch_x, touch_y, mac_x, mac_y));
        } else {
            // Touch is being held/dragged
            // Only update if position changed significantly (reduce noise)
            int dx = mac_x - last_touch_x;
            int dy = mac_y - last_touch_y;
            if (dx != 0 || dy != 0) {
                ADBMouseMoved(mac_x, mac_y);
            }
        }
        
        last_touch_x = mac_x;
        last_touch_y = mac_y;
    } else {
        if (touch_was_pressed) {
            // Touch just released
            ADBMouseUp(0);
            touch_was_pressed = false;
            
            D(bug("[INPUT] Touch up\n"));
        }
    }
}

// ============================================================================
// USB Keyboard Input Handling
// ============================================================================

/*
 *  Process USB HID keyboard modifiers
 *  Modifier byte format: [RGui][RAlt][RShift][RCtrl][LGui][LAlt][LShift][LCtrl]
 */
static void processKeyboardModifiers(uint8_t modifiers)
{
    uint8_t changed = modifiers ^ prev_modifiers;
    
    if (changed == 0) return;
    
    // Check each modifier bit
    // Bit 0: Left Control
    if (changed & 0x01) {
        if (modifiers & 0x01) {
            ADBKeyDown(0x36);  // Control
        } else {
            ADBKeyUp(0x36);
        }
    }
    
    // Bit 1: Left Shift
    if (changed & 0x02) {
        if (modifiers & 0x02) {
            ADBKeyDown(0x38);  // Shift
        } else {
            ADBKeyUp(0x38);
        }
    }
    
    // Bit 2: Left Alt (Option)
    if (changed & 0x04) {
        if (modifiers & 0x04) {
            ADBKeyDown(0x3A);  // Option
        } else {
            ADBKeyUp(0x3A);
        }
    }
    
    // Bit 3: Left GUI (Command)
    if (changed & 0x08) {
        if (modifiers & 0x08) {
            ADBKeyDown(0x37);  // Command
        } else {
            ADBKeyUp(0x37);
        }
    }
    
    // Bit 4: Right Control
    if (changed & 0x10) {
        if (modifiers & 0x10) {
            ADBKeyDown(0x36);  // Control
        } else {
            ADBKeyUp(0x36);
        }
    }
    
    // Bit 5: Right Shift
    if (changed & 0x20) {
        if (modifiers & 0x20) {
            ADBKeyDown(0x38);  // Shift
        } else {
            ADBKeyUp(0x38);
        }
    }
    
    // Bit 6: Right Alt (Option)
    if (changed & 0x40) {
        if (modifiers & 0x40) {
            ADBKeyDown(0x3A);  // Option
        } else {
            ADBKeyUp(0x3A);
        }
    }
    
    // Bit 7: Right GUI (Command)
    if (changed & 0x80) {
        if (modifiers & 0x80) {
            ADBKeyDown(0x37);  // Command
        } else {
            ADBKeyUp(0x37);
        }
    }
    
    prev_modifiers = modifiers;
}

/*
 *  Check if a key is in the key array
 */
static bool keyInArray(uint8_t key, const uint8_t *keys, int count)
{
    for (int i = 0; i < count; i++) {
        if (keys[i] == key) return true;
    }
    return false;
}

/*
 *  Process USB HID keyboard key array
 *  Called when new keyboard report is received
 *  keys[] contains up to 6 currently pressed keys (0 = no key)
 */
static void processKeyboardKeys(const uint8_t *keys)
{
    // Find keys that were released (in prev but not in current)
    for (int i = 0; i < 6; i++) {
        uint8_t key = prev_keys[i];
        if (key != 0 && !keyInArray(key, keys, 6)) {
            // Key was released
            uint8_t mac_code = usb_to_mac_keycode[key];
            if (mac_code != 0xFF) {
                ADBKeyUp(mac_code);
                D(bug("[INPUT] Key up: USB 0x%02X -> Mac 0x%02X\n", key, mac_code));
            }
        }
    }
    
    // Find keys that were pressed (in current but not in prev)
    for (int i = 0; i < 6; i++) {
        uint8_t key = keys[i];
        if (key != 0 && !keyInArray(key, prev_keys, 6)) {
            // Key was pressed
            uint8_t mac_code = usb_to_mac_keycode[key];
            if (mac_code != 0xFF) {
                ADBKeyDown(mac_code);
                D(bug("[INPUT] Key down: USB 0x%02X -> Mac 0x%02X\n", key, mac_code));
            }
        }
    }
    
    // Update previous state
    memcpy(prev_keys, keys, 6);
}

/*
 *  Process a complete USB HID keyboard report (boot protocol)
 *  Format: [modifier][reserved][key1][key2][key3][key4][key5][key6]
 */
void InputProcessKeyboardReport(const uint8_t *report, int length)
{
    if (!keyboard_enabled || length < 8) return;
    
    uint8_t modifiers = report[0];
    // report[1] is reserved
    const uint8_t *keys = &report[2];
    
    processKeyboardModifiers(modifiers);
    processKeyboardKeys(keys);
}

// ============================================================================
// USB Mouse Input Handling (Placeholder for future implementation)
// ============================================================================

/*
 *  Process a USB HID mouse report
 *  Format varies by device, typically: [buttons][x_delta][y_delta][wheel]
 */
void InputProcessMouseReport(const uint8_t *report, int length)
{
    if (length < 3) return;
    
    uint8_t buttons = report[0];
    int8_t dx = (int8_t)report[1];
    int8_t dy = (int8_t)report[2];
    
    // Handle button changes
    static uint8_t prev_buttons = 0;
    
    // Left button
    if ((buttons & 0x01) != (prev_buttons & 0x01)) {
        if (buttons & 0x01) {
            ADBMouseDown(0);
        } else {
            ADBMouseUp(0);
        }
    }
    
    // Right button
    if ((buttons & 0x02) != (prev_buttons & 0x02)) {
        if (buttons & 0x02) {
            ADBMouseDown(1);
        } else {
            ADBMouseUp(1);
        }
    }
    
    // Middle button
    if ((buttons & 0x04) != (prev_buttons & 0x04)) {
        if (buttons & 0x04) {
            ADBMouseDown(2);
        } else {
            ADBMouseUp(2);
        }
    }
    
    prev_buttons = buttons;
    
    // Handle mouse movement (relative mode)
    if (dx != 0 || dy != 0) {
        // Ensure we're in relative mouse mode for USB mouse
        ADBSetRelMouseMode(true);
        ADBMouseMoved(dx, dy);
    }
}

// ============================================================================
// Public API
// ============================================================================

bool InputInit(void)
{
    Serial.println("[INPUT] Initializing input subsystem...");
    
    // Get display dimensions from M5
    display_width = M5.Display.width();
    display_height = M5.Display.height();
    
    Serial.printf("[INPUT] Display size: %dx%d\n", display_width, display_height);
    Serial.printf("[INPUT] Mac screen size: %dx%d\n", mac_screen_width, mac_screen_height);
    
    // Initialize touch state
    touch_was_pressed = false;
    last_touch_x = 0;
    last_touch_y = 0;
    
    // Reset keyboard state
    memset(prev_keys, 0, sizeof(prev_keys));
    prev_modifiers = 0;
    
    // Set mouse to absolute mode for touch input
    ADBSetRelMouseMode(false);
    
    // TODO: Initialize USB Host for HID devices
    // This requires ESP-IDF USB Host stack integration
    // For now, touch input works immediately via M5Unified
    
    Serial.println("[INPUT] Touch input enabled");
    Serial.println("[INPUT] USB keyboard support: pending USB Host integration");
    
    return true;
}

void InputExit(void)
{
    Serial.println("[INPUT] Shutting down input subsystem");
    
    // Release any held buttons
    if (touch_was_pressed) {
        ADBMouseUp(0);
        touch_was_pressed = false;
    }
    
    // TODO: Cleanup USB Host resources
}

void InputPoll(void)
{
    // Process touch input
    processTouchInput();
    
    // USB keyboard/mouse processing happens via callbacks from USB Host
    // (InputProcessKeyboardReport / InputProcessMouseReport)
}

void InputSetScreenSize(int width, int height)
{
    mac_screen_width = width;
    mac_screen_height = height;
    Serial.printf("[INPUT] Mac screen size set to: %dx%d\n", width, height);
}

void InputSetTouchEnabled(bool enabled)
{
    touch_enabled = enabled;
    if (!enabled && touch_was_pressed) {
        ADBMouseUp(0);
        touch_was_pressed = false;
    }
}

void InputSetKeyboardEnabled(bool enabled)
{
    keyboard_enabled = enabled;
}

bool InputIsKeyboardConnected(void)
{
    return keyboard_connected;
}

bool InputIsMouseConnected(void)
{
    return mouse_connected;
}
