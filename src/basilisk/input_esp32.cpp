/*
 *  input_esp32.cpp - Input handling for ESP32 with M5Unified
 *
 *  BasiliskII ESP32 Port
 *
 *  Handles:
 *  - Touch panel input (as mouse via M5Unified)
 *  - USB HID keyboard input (via EspUsbHost library)
 *  - USB HID mouse input (via EspUsbHost library)
 *
 *  USB Host uses USB2 port on M5Stack Tab5
 */

#include "sysdeps.h"
#include "input.h"
#include "adb.h"
#include "video.h"

#include <M5Unified.h>
#include <EspUsbHost.h>

#define DEBUG 1
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

// USB device connection state
static bool keyboard_connected = false;
static bool mouse_connected = false;

// USB mouse button state
static uint8_t usb_mouse_buttons = 0;

// LED state tracking
static uint8_t last_led_state = 0;
static uint32_t last_led_check_time = 0;
static const uint32_t LED_CHECK_INTERVAL_MS = 100;  // Check every 100ms

// ============================================================================
// Forward declarations
// ============================================================================

class MacUsbHost;
static MacUsbHost *usbHost = NULL;

// ============================================================================
// MacUsbHost - Custom USB Host class for Mac emulation
// ============================================================================

class MacUsbHost : public EspUsbHost {
public:
    // Track modifier state with bitmask for proper left/right handling
    // Bit 0: Left Control pressed
    // Bit 1: Left Shift pressed
    // Bit 2: Left Alt pressed
    // Bit 3: Left GUI pressed
    // Bit 4: Right Control pressed
    // Bit 5: Right Shift pressed
    // Bit 6: Right Alt pressed
    // Bit 7: Right GUI pressed
    uint8_t modifier_state = 0;
    
    // Track if keyboard is connected for LED control
    bool has_keyboard = false;
    
    // Helper to check if a specific modifier is held (combining left+right)
    bool isControlHeld() { return (modifier_state & 0x11) != 0; }
    bool isShiftHeld() { return (modifier_state & 0x22) != 0; }
    bool isAltHeld() { return (modifier_state & 0x44) != 0; }
    bool isCommandHeld() { return (modifier_state & 0x88) != 0; }
    
    // Process a single modifier bit change
    void handleModifierBit(uint8_t bit, bool pressed, uint8_t mac_keycode) {
        uint8_t mask = (1 << bit);
        bool was_pressed = (modifier_state & mask) != 0;
        
        if (pressed && !was_pressed) {
            modifier_state |= mask;
            ADBKeyDown(mac_keycode);
            Serial.printf("[INPUT] Modifier DOWN: bit %d -> Mac 0x%02X\n", bit, mac_keycode);
        } else if (!pressed && was_pressed) {
            modifier_state &= ~mask;
            ADBKeyUp(mac_keycode);
            Serial.printf("[INPUT] Modifier UP: bit %d -> Mac 0x%02X\n", bit, mac_keycode);
        }
    }
    
    // Called when USB keyboard report is received
    void onKeyboard(hid_keyboard_report_t report, hid_keyboard_report_t last_report) override {
        if (!keyboard_enabled) return;
        
        has_keyboard = true;
        keyboard_connected = true;
        
        // Process modifier keys FIRST (important for key chords)
        // USB modifier byte: [RGui][RAlt][RShift][RCtrl][LGui][LAlt][LShift][LCtrl]
        handleModifierBit(0, (report.modifier & 0x01) != 0, 0x36);  // Left Control
        handleModifierBit(1, (report.modifier & 0x02) != 0, 0x38);  // Left Shift
        handleModifierBit(2, (report.modifier & 0x04) != 0, 0x3A);  // Left Alt/Option
        handleModifierBit(3, (report.modifier & 0x08) != 0, 0x37);  // Left GUI/Command
        handleModifierBit(4, (report.modifier & 0x10) != 0, 0x36);  // Right Control
        handleModifierBit(5, (report.modifier & 0x20) != 0, 0x38);  // Right Shift
        handleModifierBit(6, (report.modifier & 0x40) != 0, 0x3A);  // Right Alt/Option
        handleModifierBit(7, (report.modifier & 0x80) != 0, 0x37);  // Right GUI/Command
        
        // Process key releases BEFORE key presses (important for key transitions)
        for (int i = 0; i < 6; i++) {
            uint8_t old_key = last_report.keycode[i];
            if (old_key == 0) continue;
            
            // Check if this key is still pressed
            bool still_pressed = false;
            for (int j = 0; j < 6; j++) {
                if (report.keycode[j] == old_key) {
                    still_pressed = true;
                    break;
                }
            }
            
            if (!still_pressed) {
                uint8_t mac_code = usb_to_mac_keycode[old_key];
                if (mac_code != 0xFF) {
                    ADBKeyUp(mac_code);
                    Serial.printf("[INPUT] Key UP: USB 0x%02X -> Mac 0x%02X\n", old_key, mac_code);
                }
            }
        }
        
        // Process key presses
        for (int i = 0; i < 6; i++) {
            uint8_t new_key = report.keycode[i];
            if (new_key == 0) continue;
            
            // Check if this is a new key press
            bool was_pressed = false;
            for (int j = 0; j < 6; j++) {
                if (last_report.keycode[j] == new_key) {
                    was_pressed = true;
                    break;
                }
            }
            
            if (!was_pressed) {
                uint8_t mac_code = usb_to_mac_keycode[new_key];
                if (mac_code != 0xFF) {
                    ADBKeyDown(mac_code);
                    // Log with modifier state for debugging key chords
                    Serial.printf("[INPUT] Key DOWN: USB 0x%02X -> Mac 0x%02X (mods: %s%s%s%s)\n", 
                        new_key, mac_code,
                        isCommandHeld() ? "Cmd+" : "",
                        isControlHeld() ? "Ctrl+" : "",
                        isAltHeld() ? "Opt+" : "",
                        isShiftHeld() ? "Shift+" : "");
                }
            }
        }
    }
    
    // Override onReceive to properly parse mouse HID reports
    // The EspUsbHost library has wrong byte offsets for mouse parsing
    void onReceive(const usb_transfer_t *transfer) override {
        // Get endpoint data to check if this is a mouse
        endpoint_data_t *ep_data = &endpoint_data_list[(transfer->bEndpointAddress & 0x0F)];
        
        // Handle HID mice - check for HID class and mouse protocol
        // Don't require boot subclass since many mice use report protocol
        if (ep_data->bInterfaceClass != 0x03) {  // HID class
            return;
        }
        
        // Skip if this looks like a keyboard (protocol 1)
        if (ep_data->bInterfaceProtocol == 0x01) {
            return;
        }
        
        if (transfer->actual_num_bytes < 3) {
            return;
        }
        
        mouse_connected = true;
        
        // Debug: print ALL raw bytes to understand the report format
        Serial.printf("[INPUT] USB Mouse raw (%d bytes): [", transfer->actual_num_bytes);
        for (int i = 0; i < transfer->actual_num_bytes && i < 16; i++) {
            Serial.printf("%02x ", transfer->data_buffer[i]);
        }
        Serial.printf("] proto=%d\n", ep_data->bInterfaceProtocol);
        
        // Try to detect report format based on data
        uint8_t buttons = 0;
        int16_t dx = 0;
        int16_t dy = 0;
        
        // Logitech MX Master and similar mice use this format:
        // Byte 0: Report ID (0x02 for mouse movement)
        // Byte 1: Buttons
        // Byte 2: Padding/unknown (usually 0x00)
        // Bytes 3-4: X movement (16-bit signed, little endian)
        // Bytes 5-6: Y movement (16-bit signed, little endian)
        // Bytes 7-8: Wheel or other data
        
        if (transfer->actual_num_bytes >= 7 && transfer->data_buffer[0] == 0x02) {
            // Logitech extended format with report ID
            buttons = transfer->data_buffer[1];
            dx = (int16_t)(transfer->data_buffer[3] | (transfer->data_buffer[4] << 8));
            dy = (int16_t)(transfer->data_buffer[5] | (transfer->data_buffer[6] << 8));
            Serial.printf("[INPUT] Logitech format: buttons=%02x dx=%d dy=%d\n", buttons, dx, dy);
        } else if (transfer->actual_num_bytes >= 4 && transfer->data_buffer[0] <= 0x07) {
            // Standard boot protocol: buttons, X, Y, wheel
            buttons = transfer->data_buffer[0];
            dx = (int8_t)transfer->data_buffer[1];
            dy = (int8_t)transfer->data_buffer[2];
            Serial.printf("[INPUT] Boot format: buttons=%02x dx=%d dy=%d\n", buttons, dx, dy);
        } else if (transfer->actual_num_bytes >= 5) {
            // Try format with report ID: ReportID, buttons, X, Y
            buttons = transfer->data_buffer[1];
            dx = (int8_t)transfer->data_buffer[2];
            dy = (int8_t)transfer->data_buffer[3];
            Serial.printf("[INPUT] Report ID format: buttons=%02x dx=%d dy=%d\n", buttons, dx, dy);
        } else {
            // Fallback: assume boot protocol
            buttons = transfer->data_buffer[0];
            dx = (int8_t)transfer->data_buffer[1];
            dy = (int8_t)transfer->data_buffer[2];
            Serial.printf("[INPUT] Fallback format: buttons=%02x dx=%d dy=%d\n", buttons, dx, dy);
        }
        
        // Handle button changes
        uint8_t changed = buttons ^ usb_mouse_buttons;
        
        if (changed & 0x01) {
            if (buttons & 0x01) {
                ADBMouseDown(0);
                Serial.println("[INPUT] USB Mouse: Left button DOWN");
            } else {
                ADBMouseUp(0);
                Serial.println("[INPUT] USB Mouse: Left button UP");
            }
        }
        
        if (changed & 0x02) {
            if (buttons & 0x02) {
                ADBMouseDown(1);
                Serial.println("[INPUT] USB Mouse: Right button DOWN");
            } else {
                ADBMouseUp(1);
                Serial.println("[INPUT] USB Mouse: Right button UP");
            }
        }
        
        if (changed & 0x04) {
            if (buttons & 0x04) {
                ADBMouseDown(2);
                Serial.println("[INPUT] USB Mouse: Middle button DOWN");
            } else {
                ADBMouseUp(2);
                Serial.println("[INPUT] USB Mouse: Middle button UP");
            }
        }
        
        usb_mouse_buttons = buttons;
        
        // Handle movement
        if (dx != 0 || dy != 0) {
            ADBSetRelMouseMode(true);
            ADBMouseMoved(dx, dy);
        }
    }
    
    // Keep these as empty overrides to prevent the buggy EspUsbHost parsing
    void onMouseMove(hid_mouse_report_t report) override {
        // Handled in onReceive instead
    }
    
    void onMouseButtons(hid_mouse_report_t report, uint8_t last_buttons) override {
        // Handled in onReceive instead
    }
    
    // Called when USB device is disconnected
    void onGone(const usb_host_client_event_msg_t *eventMsg) override {
        Serial.println("[INPUT] USB device disconnected");
        keyboard_connected = false;
        mouse_connected = false;
        has_keyboard = false;
        modifier_state = 0;  // Reset modifier state
    }
    
    // Send LED state to keyboard
    void setKeyboardLEDs(uint8_t leds) {
        if (!has_keyboard || !isReady || deviceHandle == NULL) {
            return;
        }
        
        // USB HID SET_REPORT for keyboard LEDs
        // Report ID 0, Output report, LED byte
        usb_transfer_t *transfer;
        esp_err_t err = usb_host_transfer_alloc(8 + 1, 0, &transfer);
        if (err != ESP_OK) {
            Serial.printf("[INPUT] Failed to allocate transfer for LED: %x\n", err);
            return;
        }
        
        // Build SET_REPORT request
        // bmRequestType: 0x21 (Host to Device, Class, Interface)
        // bRequest: 0x09 (SET_REPORT)
        // wValue: 0x0200 (Report Type: Output, Report ID: 0)
        // wIndex: Interface number (0 for boot keyboard)
        // wLength: 1 (one byte for LED state)
        transfer->num_bytes = 8 + 1;
        transfer->data_buffer[0] = 0x21;  // bmRequestType
        transfer->data_buffer[1] = 0x09;  // bRequest (SET_REPORT)
        transfer->data_buffer[2] = 0x00;  // wValue low (Report ID)
        transfer->data_buffer[3] = 0x02;  // wValue high (Report Type: Output)
        transfer->data_buffer[4] = 0x00;  // wIndex low (Interface)
        transfer->data_buffer[5] = 0x00;  // wIndex high
        transfer->data_buffer[6] = 0x01;  // wLength low
        transfer->data_buffer[7] = 0x00;  // wLength high
        transfer->data_buffer[8] = leds;  // LED state byte
        
        transfer->device_handle = deviceHandle;
        transfer->bEndpointAddress = 0x00;
        transfer->callback = NULL;  // We don't need callback for LED
        transfer->context = NULL;
        
        err = usb_host_transfer_submit_control(clientHandle, transfer);
        if (err != ESP_OK) {
            Serial.printf("[INPUT] Failed to submit LED control transfer: %x\n", err);
        } else {
            Serial.printf("[INPUT] LED state sent: 0x%02X (Num:%d Caps:%d Scroll:%d)\n", 
                leds, 
                (leds & 0x01) ? 1 : 0,
                (leds & 0x02) ? 1 : 0,
                (leds & 0x04) ? 1 : 0);
        }
        
        // Note: transfer will be freed when it completes
        // For simplicity, we're using synchronous approach here
        usb_host_transfer_free(transfer);
    }
};

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
            // Touch just started - switch to absolute mode for touch
            ADBSetRelMouseMode(false);
            touch_was_pressed = true;
            
            // Move cursor to touch position
            ADBMouseMoved(mac_x, mac_y);
            
            // Immediately press mouse button for responsive feel
            ADBMouseDown(0);
            
            Serial.printf("[INPUT] Touch DOWN at (%d, %d) -> Mac (%d, %d)\n", 
                  touch_x, touch_y, mac_x, mac_y);
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
            
            Serial.println("[INPUT] Touch UP");
        }
    }
}

/*
 *  Check and update keyboard LED state
 */
static void updateKeyboardLEDs(void)
{
    if (usbHost == NULL || !usbHost->has_keyboard) {
        return;
    }
    
    uint32_t now = millis();
    if ((now - last_led_check_time) < LED_CHECK_INTERVAL_MS) {
        return;
    }
    last_led_check_time = now;
    
    // Get current LED state from Mac
    uint8_t current_leds = ADBGetKeyboardLEDs();
    
    // Update keyboard if state changed
    if (current_leds != last_led_state) {
        Serial.printf("[INPUT] LED state changed: 0x%02X -> 0x%02X\n", last_led_state, current_leds);
        usbHost->setKeyboardLEDs(current_leds);
        last_led_state = current_leds;
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
    
    // Initialize LED state
    last_led_state = 0;
    last_led_check_time = 0;
    
    // Set mouse to absolute mode for touch input (USB mouse will switch to relative)
    ADBSetRelMouseMode(false);
    
    Serial.println("[INPUT] Touch input enabled");
    
    // Initialize USB Host for keyboard/mouse on USB2 port
    Serial.println("[INPUT] Initializing USB Host on USB2...");
    usbHost = new MacUsbHost();
    if (usbHost != NULL) {
        usbHost->begin();
        Serial.println("[INPUT] USB Host initialized - connect keyboard/mouse to USB2 port");
    } else {
        Serial.println("[INPUT] ERROR: Failed to create USB Host instance");
    }
    
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
    
    // Cleanup USB Host
    if (usbHost != NULL) {
        delete usbHost;
        usbHost = NULL;
    }
}

void InputPoll(void)
{
    // Process touch input
    processTouchInput();
    
    // Process USB Host events
    if (usbHost != NULL) {
        usbHost->task();
    }
    
    // Update keyboard LEDs (Caps Lock, etc.)
    updateKeyboardLEDs();
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

// ============================================================================
// Legacy functions (kept for compatibility, now handled via EspUsbHost callbacks)
// ============================================================================

void InputProcessKeyboardReport(const uint8_t *report, int length)
{
    // No longer used - keyboard input comes via EspUsbHost callbacks
    (void)report;
    (void)length;
}

void InputProcessMouseReport(const uint8_t *report, int length)
{
    // No longer used - mouse input comes via EspUsbHost callbacks
    (void)report;
    (void)length;
}
