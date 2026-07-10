/*
 *  input_esp32.cpp - Input handling for ESP32 with M5Unified
 *
 *  BasiliskII ESP32 Port
 *
 *  Handles:
 *  - Touch panel input (as mouse via M5Unified)
 *  - USB HID keyboard input (via ESP-IDF USB Host Library)
 *  - USB HID mouse input (via ESP-IDF USB Host Library)
 *
 *  USB Host uses USB2 port on M5Stack Tab5
 *  Supports USB hubs for simultaneous keyboard + mouse
 *
 *  Mouse right-click is translated to Control+Click for Mac OS 8
 *  contextual menu compatibility.
 */

#include "sysdeps.h"
#include "input.h"
#include "adb.h"
#include "video.h"

#include "board.h"
#include "board_keyboard.h"
#include "board_touch.h"
#include "board_display.h"
#include "touch_overlay.h"

#if defined(BOARD_M5STACK_TAB5)
#include <M5Unified.h>
#endif
#include <usb/usb_host.h>
#include <class/hid/hid.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hid_descriptor.h"

#ifndef USB_INTERFACE_DESC
#define USB_INTERFACE_DESC 0x04
#endif
#ifndef USB_ENDPOINT_DESC
#define USB_ENDPOINT_DESC 0x05
#endif
/* HID class descriptors per HID 1.11 §7.1: 0x21 = HID, 0x22 = Report. */
#ifndef USB_HID_DESC_TYPE
#define USB_HID_DESC_TYPE        0x21
#endif
#ifndef USB_HID_REPORT_DESC_TYPE
#define USB_HID_REPORT_DESC_TYPE 0x22
#endif

/* Maximum HID report descriptor we will accept. Real-world mice top out
 * around 100-200 bytes; allocate a comfortable upper bound. */
#define HID_REPORT_DESC_MAX_BYTES 512

#define DEBUG 0
#include "debug.h"

// ============================================================================
// Input Task Configuration (runs on Core 0 to offload CPU emulation)
// ============================================================================
#define INPUT_TASK_STACK_SIZE 8192
#define INPUT_TASK_PRIORITY   1
#define INPUT_TASK_CORE       0  // Run on Core 0, leaving Core 1 for CPU emulation
#define INPUT_POLL_INTERVAL_MS 20  // 50Hz polling
#define USB_POLL_DIV_ACTIVE   1   // Poll USB every cycle when devices are active
#define USB_POLL_DIV_IDLE     4   // Poll USB every 64ms when idle (16ms * 4)

static TaskHandle_t input_task_handle = NULL;
static SemaphoreHandle_t input_task_stopped = NULL;
static volatile bool input_task_running = false;

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

// USB HID to Mac ADB keycode translation - in internal SRAM for fast lookup
// Accessed on every keystroke
DRAM_ATTR static const uint8_t usb_to_mac_keycode[256] = {
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
/* Mac framebuffer dimensions come from the board config (640x360 on Tab5,
 * 640x400 on Waveshare). InputSetScreenSize() can still override at runtime
 * if the emulator switches modes. */
static int mac_screen_width  = BOARD_MAC_SCREEN_WIDTH;
static int mac_screen_height = BOARD_MAC_SCREEN_HEIGHT;

// Display dimensions (populated from BoardDisplay_* in InputInit)
static int display_width  = BOARD_DISPLAY_WIDTH;
static int display_height = BOARD_DISPLAY_HEIGHT;

// Input enable flags
static bool keyboard_enabled = true;
// Touch enable is owned by touch_overlay.cpp now; we keep a local shadow
// for InputSetTouchEnabled() to forward without pulling in a getter.
static bool touch_enabled = true;

// USB device connection state
static bool usb_keyboard_connected = false;
static bool mouse_connected = false;

// USB mouse button state
static uint8_t usb_mouse_buttons = 0;

// Right-click to Control+Click translation state (Mac OS 8 contextual menus)
static bool right_click_ctrl_injected = false;

/* Physical-key claims from every input source, indexed by final Mac ADB
 * keycode. USB slots, the Tab5 Keyboard matrix, and synthetic right-click
 * Control each own an independent claim. ADB sees a down event only on the
 * first claim and an up event only when the last claim is released, so two
 * keyboards can hold the same key/modifier without releasing each other. */
DRAM_ATTR static uint8_t adb_key_claims[128] = {};

static void claimAdbKey(uint8_t mac_keycode)
{
    if (mac_keycode >= 0x80) return;
    uint8_t &count = adb_key_claims[mac_keycode];
    if (count == 0) {
        ADBKeyDown(mac_keycode);
    }
    if (count != 0xFF) {
        ++count;
    } else {
        Serial.printf("[INPUT] WARN: ADB key 0x%02X claim count saturated\n",
                      mac_keycode);
    }
}

static void releaseAdbKey(uint8_t mac_keycode)
{
    if (mac_keycode >= 0x80) return;
    uint8_t &count = adb_key_claims[mac_keycode];
    if (count == 0) {
        return;  // Ignore orphan releases from a reset/disconnected source.
    }
    --count;
    if (count == 0) {
        ADBKeyUp(mac_keycode);
    }
}

extern "C" void InputKeyDown(uint8_t mac_keycode)
{
    claimAdbKey(mac_keycode);
}

extern "C" void InputKeyUp(uint8_t mac_keycode)
{
    releaseAdbKey(mac_keycode);
}

// LED state tracking
static uint8_t last_led_state = 0;
static uint32_t last_led_check_time = 0;
static const uint32_t LED_CHECK_INTERVAL_MS = 100;  // Check every 100ms

// ============================================================================
// Multi-Device USB Host Types
// ============================================================================

#define MAX_USB_DEVICES 4
#define MAX_TRANSFERS_PER_DEVICE 8
#define MAX_INTERFACES_PER_DEVICE 8
#define MAX_ENDPOINTS 17

struct EndpointInfo {
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
};

struct UsbDeviceSlot {
    bool active;
    usb_device_handle_t handle;
    bool has_keyboard;
    bool has_mouse;
    usb_transfer_t *transfers[MAX_TRANSFERS_PER_DEVICE];
    uint8_t transfer_count;
    uint8_t interfaces[MAX_INTERFACES_PER_DEVICE];
    uint8_t interface_count;
    EndpointInfo endpoint_info[MAX_ENDPOINTS];
    hid_keyboard_report_t last_kb_report;
    uint8_t interval;
    bool ready;
    unsigned long last_poll;

    /* Parsed HID mouse layout (X/Y/Wheel/Buttons bit map). Populated
     * asynchronously by handleNewDevice -> kickHidReportRead(). Stays
     * { .valid = false } if parsing failed or the device isn't a
     * mouse, in which case processMouseReport() falls back to its
     * heuristic byte-layout decoder. */
    HidMouseLayout mouse_layout;
};

// Forward declarations
class MultiDeviceUsbHost;
static MultiDeviceUsbHost *usbHost = NULL;

// ============================================================================
// Keyboard Input Processing
// ============================================================================

static bool isControlPhysicallyHeld() {
    return adb_key_claims[0x36] != 0;
}

static void processKeyboardReport(hid_keyboard_report_t *report,
                                   hid_keyboard_report_t *last_report,
                                   bool force_release = false) {
    if (!keyboard_enabled && !force_release) return;

    static const uint8_t modifier_to_mac[8] = {
        0x36, 0x38, 0x3A, 0x37,  // left Ctrl, Shift, Alt/Option, GUI/Command
        0x36, 0x38, 0x3A, 0x37   // right variants
    };

    // Every USB slot owns its own last report, so modifiers held by two
    // keyboards remain independent claims even when they map to one ADB key.
    for (uint8_t bit = 0; bit < 8; ++bit) {
        const uint8_t mask = (uint8_t)(1U << bit);
        const bool was_pressed = (last_report->modifier & mask) != 0;
        const bool is_pressed  = (report->modifier & mask) != 0;
        if (is_pressed && !was_pressed) {
            claimAdbKey(modifier_to_mac[bit]);
        } else if (!is_pressed && was_pressed) {
            releaseAdbKey(modifier_to_mac[bit]);
        }
    }

    // Process key releases BEFORE key presses (important for key transitions)
    for (int i = 0; i < 6; i++) {
        uint8_t old_key = last_report->keycode[i];
        if (old_key == 0) continue;

        bool still_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (report->keycode[j] == old_key) {
                still_pressed = true;
                break;
            }
        }

        if (!still_pressed) {
            uint8_t mac_code = usb_to_mac_keycode[old_key];
            if (mac_code != 0xFF) {
                releaseAdbKey(mac_code);
            }
        }
    }

    // Process key presses
    for (int i = 0; i < 6; i++) {
        uint8_t new_key = report->keycode[i];
        if (new_key == 0) continue;

        bool was_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (last_report->keycode[j] == new_key) {
                was_pressed = true;
                break;
            }
        }

        if (!was_pressed) {
            uint8_t mac_code = usb_to_mac_keycode[new_key];
            if (mac_code != 0xFF) {
                claimAdbKey(mac_code);
            }
        }
    }
}

// ============================================================================
// Official M5Stack Tab5 Keyboard (Ext.Port1 I2C matrix)
// ============================================================================

struct Tab5HidMapping {
    uint8_t usage;
    uint8_t modifiers;
};

/* Official US-layout matrix mapping published in M5Unit-KEYBOARD. Normal
 * mode is used instead of the module's lossy one-key HID event mode so every
 * matrix key and modifier has an independent press/release claim. */
static const Tab5HidMapping tab5_base_map[70] = {
    // Row 0: Esc 1 2 3 4 5 6 7 8 9 0 - + Del
    {0x29,0}, {0x1E,0}, {0x1F,0}, {0x20,0}, {0x21,0}, {0x22,0}, {0x23,0},
    {0x24,0}, {0x25,0}, {0x26,0}, {0x27,0}, {0x2D,0}, {0x2E,0x02}, {0x4C,0},
    // Row 1: ` ! @ # $ % ^ & * ( ) [ ] backslash
    {0x35,0}, {0x1E,0x02}, {0x1F,0x02}, {0x20,0x02}, {0x21,0x02},
    {0x22,0x02}, {0x23,0x02}, {0x24,0x02}, {0x25,0x02}, {0x26,0x02},
    {0x27,0x02}, {0x2F,0}, {0x30,0}, {0x31,0},
    // Row 2: Tab q w e r t y u i o p ; ' Backspace
    {0x2B,0}, {0x14,0}, {0x1A,0}, {0x08,0}, {0x15,0}, {0x17,0}, {0x1C,0},
    {0x18,0}, {0x0C,0}, {0x12,0}, {0x13,0}, {0x33,0}, {0x34,0}, {0x2A,0},
    // Row 3: Sym Aa a s d f g h j k l Up _ Enter
    {0,0}, {0,0}, {0x04,0}, {0x16,0}, {0x07,0}, {0x09,0}, {0x0A,0},
    {0x0B,0}, {0x0D,0}, {0x0E,0}, {0x0F,0}, {0x52,0}, {0x2D,0x02}, {0x28,0},
    // Row 4: Ctrl Alt z x c v b n m . Left Down Right Space
    {0,0}, {0,0}, {0x1D,0}, {0x1B,0}, {0x06,0}, {0x19,0}, {0x05,0},
    {0x11,0}, {0x10,0}, {0x37,0}, {0x50,0}, {0x51,0}, {0x4F,0}, {0x2C,0}
};

static_assert(sizeof(tab5_base_map) / sizeof(tab5_base_map[0]) == 70,
              "Tab5 keyboard matrix must contain 70 keys");

static Tab5HidMapping tab5Mapping(uint8_t row, uint8_t col, bool sym)
{
    const uint8_t index = (uint8_t)(row * 14U + col);
    Tab5HidMapping mapping = tab5_base_map[index];
    if (!sym) return mapping;

    // Sym-layer differences from the base printing on the physical keys.
    switch (index) {
        case 14: mapping = {0x35,0x02}; break; // ` -> ~
        case 15: mapping = {0x38,0x02}; break; // ! -> ?
        case 22: mapping = {0x38,0x00}; break; // * -> /
        case 23: mapping = {0x36,0x02}; break; // ( -> <
        case 24: mapping = {0x37,0x02}; break; // ) -> >
        case 25: mapping = {0x2F,0x02}; break; // [ -> {
        case 26: mapping = {0x30,0x02}; break; // ] -> }
        case 27: mapping = {0x31,0x02}; break; // backslash -> |
        case 39: mapping = {0x33,0x02}; break; // ; -> :
        case 40: mapping = {0x34,0x02}; break; // ' -> "
        case 54: mapping = {0x2E,0x00}; break; // _ -> =
        case 65: mapping = {0x36,0x00}; break; // . -> ,
        default: break;
    }
    return mapping;
}

struct Tab5KeyBinding {
    bool active;
    uint8_t mac_keycode;
    uint8_t forced_modifiers;
};

static Tab5KeyBinding tab5_bindings[70] = {};
static bool tab5_sym_active = false;
static bool tab5_keyboard_connected = false;

static const uint8_t hid_modifier_to_mac[8] = {
    0x36, 0x38, 0x3A, 0x37, 0x36, 0x38, 0x3A, 0x37
};

static void applyForcedModifiers(uint8_t modifiers, bool pressed)
{
    for (uint8_t bit = 0; bit < 8; ++bit) {
        if ((modifiers & (1U << bit)) == 0) continue;
        if (pressed) claimAdbKey(hid_modifier_to_mac[bit]);
        else releaseAdbKey(hid_modifier_to_mac[bit]);
    }
}

static bool isTab5ModifierIndex(uint8_t index)
{
    return index == 42 || index == 43 || index == 56 || index == 57;
}

static void releaseTab5Binding(uint8_t index)
{
    Tab5KeyBinding &binding = tab5_bindings[index];
    if (!binding.active) return;
    if (binding.mac_keycode != 0xFF) {
        releaseAdbKey(binding.mac_keycode);
    }
    applyForcedModifiers(binding.forced_modifiers, false);
    binding = {};
}

static void releaseAllTab5Bindings(void)
{
    // Release ordinary keys before their physical/synthetic modifiers.
    for (uint8_t i = 0; i < 70; ++i) {
        if (!isTab5ModifierIndex(i)) releaseTab5Binding(i);
    }
    for (uint8_t i = 0; i < 70; ++i) {
        if (isTab5ModifierIndex(i)) releaseTab5Binding(i);
    }
    tab5_sym_active = false;
}

static void processTab5KeyboardEvent(const BoardKeyboardEvent &event)
{
    if (event.row >= 5 || event.col >= 14) return;
    const uint8_t index = (uint8_t)(event.row * 14U + event.col);
    Tab5KeyBinding &binding = tab5_bindings[index];

    if (event.pressed == binding.active) {
        return; // Duplicate down or orphan up from a stale/replayed FIFO event.
    }
    if (!event.pressed) {
        releaseTab5Binding(index);
        if (index == 42) tab5_sym_active = false;
        return;
    }

    binding.active = true;
    binding.mac_keycode = 0xFF;
    binding.forced_modifiers = 0;

    switch (index) {
        case 42: // Sym selects the alternate printed layer; it is not a Mac key.
            tab5_sym_active = true;
            return;
        case 43: binding.mac_keycode = 0x38; break; // Aa -> Shift
        case 56: binding.mac_keycode = 0x36; break; // Ctrl
        case 57: binding.mac_keycode = 0x3A; break; // Alt -> Option
        default: {
            const Tab5HidMapping mapping = tab5Mapping(event.row, event.col,
                                                       tab5_sym_active);
            const uint8_t mac = usb_to_mac_keycode[mapping.usage];
            if (mapping.usage == 0 || mac == 0xFF) {
                binding = {};
                return;
            }
            binding.mac_keycode = mac;
            binding.forced_modifiers = mapping.modifiers;
            applyForcedModifiers(binding.forced_modifiers, true);
            break;
        }
    }
    claimAdbKey(binding.mac_keycode);
}

static void pollTab5Keyboard(void)
{
    BoardKeyboardEvent event = {};
    int event_budget = 32;
    while (event_budget-- > 0 && BoardKeyboard_Poll(&event)) {
        processTab5KeyboardEvent(event);
    }

    const bool connected_now = BoardKeyboard_IsConnected();
    if (tab5_keyboard_connected && !connected_now) {
        releaseAllTab5Bindings();
    }
    tab5_keyboard_connected = connected_now;
}

// ============================================================================
// Mouse Input Processing (with Mac OS 8 right-click -> Control+Click)
// ============================================================================

static void processMouseReport(const usb_transfer_t *transfer, EndpointInfo *ep_info,
                               const HidMouseLayout *layout) {
    if (ep_info->bInterfaceClass != USB_CLASS_HID) return;
    if (ep_info->bInterfaceProtocol == HID_ITF_PROTOCOL_KEYBOARD) return;
    if (transfer->actual_num_bytes < 3) return;

    mouse_connected = true;

    uint8_t buttons = 0;
    int16_t dx = 0;
    int16_t dy = 0;
    int16_t dw = 0;

    /* Preferred path: parsed HID report descriptor. The parser ran
     * asynchronously after device enumeration; if it succeeded, the
     * layout knows exactly where X/Y/Wheel/Buttons live regardless of
     * whether the device is in boot or report protocol or uses a
     * Report ID prefix. The heuristic decoder below stays as a
     * fallback for vintage boot-mode mice and devices whose report
     * descriptor is unusual enough that the parser bails. */
    bool layout_decoded = false;
    if (layout != NULL && layout->valid) {
        layout_decoded = HidDecodeMouseReport(transfer->data_buffer,
                                              transfer->actual_num_bytes,
                                              layout, &buttons, &dx, &dy, &dw);
    }

    if (!layout_decoded) {
        // Logitech MX Master and similar mice: Report ID 0x02, 16-bit movement
        if (transfer->actual_num_bytes >= 7 && transfer->data_buffer[0] == 0x02) {
            buttons = transfer->data_buffer[1];
            dx = (int16_t)(transfer->data_buffer[3] | (transfer->data_buffer[4] << 8));
            dy = (int16_t)(transfer->data_buffer[5] | (transfer->data_buffer[6] << 8));
        } else if (transfer->actual_num_bytes >= 4 && transfer->data_buffer[0] <= 0x07) {
            // Standard boot protocol: buttons, X, Y, wheel
            buttons = transfer->data_buffer[0];
            dx = (int8_t)transfer->data_buffer[1];
            dy = (int8_t)transfer->data_buffer[2];
            if (transfer->actual_num_bytes >= 4) {
                dw = (int8_t)transfer->data_buffer[3];
            }
        } else if (transfer->actual_num_bytes >= 5) {
            // Report ID format: ReportID, buttons, X, Y
            buttons = transfer->data_buffer[1];
            dx = (int8_t)transfer->data_buffer[2];
            dy = (int8_t)transfer->data_buffer[3];
        } else {
            // Fallback: assume boot protocol
            buttons = transfer->data_buffer[0];
            dx = (int8_t)transfer->data_buffer[1];
            dy = (int8_t)transfer->data_buffer[2];
        }
    }

    uint8_t old_buttons = usb_mouse_buttons;

    // Mac OS 8 right-click -> Control+Click translation.
    // Inject/release a synthetic Control key when right button changes.
    bool right_now = (buttons & 0x02) != 0;
    bool right_was = (old_buttons & 0x02) != 0;

    if (right_now && !right_was) {
        if (!isControlPhysicallyHeld()) {
            claimAdbKey(0x36);  // synthetic Control claim
            right_click_ctrl_injected = true;
        }
    } else if (!right_now && right_was) {
        if (right_click_ctrl_injected) {
            releaseAdbKey(0x36);
            right_click_ctrl_injected = false;
        }
    }

    // Mac button 0 maps from USB left (0x01) OR right (0x02).
    // Both physical buttons drive a single Mac left-click so that
    // releasing one while the other is still held keeps the button down.
    bool mac_btn0_now = (buttons & 0x01) || (buttons & 0x02);
    bool mac_btn0_was = (old_buttons & 0x01) || (old_buttons & 0x02);

    if (mac_btn0_now && !mac_btn0_was) {
        ADBMouseDown(0);
    } else if (!mac_btn0_now && mac_btn0_was) {
        ADBMouseUp(0);
    }

    // Middle button (0x04) passes through as Mac button 2
    bool mid_now = (buttons & 0x04) != 0;
    bool mid_was = (old_buttons & 0x04) != 0;

    if (mid_now && !mid_was) {
        ADBMouseDown(2);
    } else if (!mid_now && mid_was) {
        ADBMouseUp(2);
    }

    usb_mouse_buttons = buttons;

    if (dx != 0 || dy != 0) {
        ADBSetRelMouseMode(true);
        ADBMouseMoved(dx, dy);
    }

    /* Forward scroll wheel as Mac arrow keys. Classic Mac OS doesn't
     * emulate ADB scroll natively (it pre-dates the wheel), so we
     * drive arrow-up/arrow-down keystrokes per wheel detent. Most
     * scroll-aware apps from the era (Netscape, IE 5, scrollable
     * dialogs) handle that fine; games can ignore. We rate-limit to
     * one keystroke per wheel detent rather than streaming so a
     * single notch produces one arrow press. */
    if (dw != 0) {
        const uint8_t mac_arrow_up   = 0x7E;
        const uint8_t mac_arrow_down = 0x7D;
        int16_t notches = dw;
        if (notches > 4)  notches = 4;
        if (notches < -4) notches = -4;
        while (notches > 0) {
            claimAdbKey(mac_arrow_up);
            releaseAdbKey(mac_arrow_up);
            notches--;
        }
        while (notches < 0) {
            claimAdbKey(mac_arrow_down);
            releaseAdbKey(mac_arrow_down);
            notches++;
        }
    }
}

// ============================================================================
// MultiDeviceUsbHost - USB Host with hub and multi-device support
// ============================================================================

class MultiDeviceUsbHost {
public:
    usb_host_client_handle_t clientHandle;
    UsbDeviceSlot devices[MAX_USB_DEVICES];
    uint32_t eventFlags;

    MultiDeviceUsbHost() {
        clientHandle = NULL;
        eventFlags = 0;
        for (int i = 0; i < MAX_USB_DEVICES; i++) {
            memset(&devices[i], 0, sizeof(UsbDeviceSlot));
        }
    }

    void begin() {
        const usb_host_config_t host_config = {
            .skip_phy_setup = false,
            .intr_flags = ESP_INTR_FLAG_LEVEL1,
        };
        esp_err_t err = usb_host_install(&host_config);
        if (err != ESP_OK) {
            Serial.printf("[USB] usb_host_install() err=0x%x\n", err);
        }

        const usb_host_client_config_t client_config = {
            .is_synchronous = true,
            .max_num_event_msg = 10,
            .async = {
                .client_event_callback = clientEventCallback,
                .callback_arg = this,
            }
        };
        err = usb_host_client_register(&client_config, &clientHandle);
        if (err != ESP_OK) {
            Serial.printf("[USB] usb_host_client_register() err=0x%x\n", err);
        }
    }

    void task() {
        esp_err_t err = usb_host_lib_handle_events(1, &eventFlags);
        (void)err;

        err = usb_host_client_handle_events(clientHandle, 1);
        (void)err;

        unsigned long now = millis();
        for (int d = 0; d < MAX_USB_DEVICES; d++) {
            UsbDeviceSlot *dev = &devices[d];
            if (!dev->active || !dev->ready) continue;
            if ((now - dev->last_poll) < dev->interval) continue;
            dev->last_poll = now;

            for (int t = 0; t < dev->transfer_count; t++) {
                if (dev->transfers[t] == NULL) continue;
                usb_host_transfer_submit(dev->transfers[t]);
            }
        }
    }

    int findFreeSlot() {
        for (int i = 0; i < MAX_USB_DEVICES; i++) {
            if (!devices[i].active) return i;
        }
        return -1;
    }

    int findDeviceByHandle(usb_device_handle_t handle) {
        for (int i = 0; i < MAX_USB_DEVICES; i++) {
            if (devices[i].active && devices[i].handle == handle) return i;
        }
        return -1;
    }

    int findKeyboardDevice() {
        for (int i = 0; i < MAX_USB_DEVICES; i++) {
            if (devices[i].active && devices[i].has_keyboard) return i;
        }
        return -1;
    }

    bool hasAnyKeyboard() {
        return findKeyboardDevice() >= 0;
    }

    void setKeyboardLEDs(uint8_t leds) {
        int dev_idx = findKeyboardDevice();
        if (dev_idx < 0) return;
        UsbDeviceSlot *dev = &devices[dev_idx];
        if (!dev->ready || dev->handle == NULL) return;

        usb_transfer_t *transfer;
        esp_err_t err = usb_host_transfer_alloc(8 + 1, 0, &transfer);
        if (err != ESP_OK) return;

        transfer->num_bytes = 8 + 1;
        transfer->data_buffer[0] = 0x21;  // bmRequestType (Host->Device, Class, Interface)
        transfer->data_buffer[1] = 0x09;  // bRequest (SET_REPORT)
        transfer->data_buffer[2] = 0x00;  // wValue low (Report ID 0)
        transfer->data_buffer[3] = 0x02;  // wValue high (Output Report)
        transfer->data_buffer[4] = 0x00;  // wIndex low (Interface 0)
        transfer->data_buffer[5] = 0x00;  // wIndex high
        transfer->data_buffer[6] = 0x01;  // wLength low
        transfer->data_buffer[7] = 0x00;  // wLength high
        transfer->data_buffer[8] = leds;

        transfer->device_handle = dev->handle;
        transfer->bEndpointAddress = 0x00;
        transfer->callback = NULL;
        transfer->context = NULL;

        err = usb_host_transfer_submit_control(clientHandle, transfer);
        if (err != ESP_OK) {
            Serial.printf("[USB] LED control transfer failed: 0x%x\n", err);
        }
        usb_host_transfer_free(transfer);
    }

    void releaseAllKeyboardClaims() {
        hid_keyboard_report_t empty = {};
        for (int i = 0; i < MAX_USB_DEVICES; ++i) {
            UsbDeviceSlot *dev = &devices[i];
            if (!dev->active || !dev->has_keyboard) continue;
            processKeyboardReport(&empty, &dev->last_kb_report, true);
            dev->last_kb_report = {};
        }
    }

private:
    static void clientEventCallback(const usb_host_client_event_msg_t *eventMsg, void *arg) {
        MultiDeviceUsbHost *host = (MultiDeviceUsbHost *)arg;

        switch (eventMsg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            host->handleNewDevice(eventMsg->new_dev.address);
            break;
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            host->handleDeviceGone(eventMsg->dev_gone.dev_hdl);
            break;
        default:
            break;
        }
    }

    void handleNewDevice(uint8_t dev_addr) {
        int slot = findFreeSlot();
        if (slot < 0) {
            Serial.println("[USB] No free device slots");
            return;
        }

        UsbDeviceSlot *dev = &devices[slot];
        memset(dev, 0, sizeof(UsbDeviceSlot));

        esp_err_t err = usb_host_device_open(clientHandle, dev_addr, &dev->handle);
        if (err != ESP_OK) {
            Serial.printf("[USB] device_open(%d) err=0x%x\n", dev_addr, err);
            return;
        }

        dev->active = true;

        usb_device_info_t dev_info;
        if (usb_host_device_info(dev->handle, &dev_info) == ESP_OK) {
            Serial.printf("[USB] Device %d connected (addr=%d, speed=%d)\n",
                          slot, dev_addr, dev_info.speed);
        }

        const usb_config_desc_t *config_desc;
        err = usb_host_get_active_config_descriptor(dev->handle, &config_desc);
        if (err != ESP_OK) {
            Serial.printf("[USB] get_config_desc err=0x%x\n", err);
            return;
        }

        enumerateDevice(slot, config_desc);

        if (dev->has_keyboard) {
            Serial.printf("[USB] Device %d: keyboard detected\n", slot);
            usb_keyboard_connected = true;
        }
        if (dev->has_mouse) {
            Serial.printf("[USB] Device %d: mouse detected\n", slot);
            mouse_connected = true;
        }
    }

    void handleDeviceGone(usb_device_handle_t dev_hdl) {
        int slot = findDeviceByHandle(dev_hdl);
        if (slot < 0) {
            usb_host_device_close(clientHandle, dev_hdl);
            return;
        }

        UsbDeviceSlot *dev = &devices[slot];
        bool was_keyboard = dev->has_keyboard;
        bool was_mouse = dev->has_mouse;

        Serial.printf("[USB] Device %d disconnected\n", slot);

        if (was_keyboard) {
            hid_keyboard_report_t empty = {};
            processKeyboardReport(&empty, &dev->last_kb_report, true);
        }

        for (int i = 0; i < dev->transfer_count; i++) {
            if (dev->transfers[i] != NULL) {
                usb_host_endpoint_clear(dev->handle, dev->transfers[i]->bEndpointAddress);
                usb_host_transfer_free(dev->transfers[i]);
                dev->transfers[i] = NULL;
            }
        }

        for (int i = 0; i < dev->interface_count; i++) {
            usb_host_interface_release(clientHandle, dev->handle, dev->interfaces[i]);
        }

        usb_host_device_close(clientHandle, dev->handle);

        memset(dev, 0, sizeof(UsbDeviceSlot));

        // Only reset connection flags if no other device provides the capability
        if (was_keyboard) {
            usb_keyboard_connected = false;
            for (int i = 0; i < MAX_USB_DEVICES; i++) {
                if (devices[i].active && devices[i].has_keyboard) {
                    usb_keyboard_connected = true;
                    break;
                }
            }
        }
        if (was_mouse) {
            mouse_connected = false;
            for (int i = 0; i < MAX_USB_DEVICES; i++) {
                if (devices[i].active && devices[i].has_mouse) {
                    mouse_connected = true;
                    break;
                }
            }
            if (!mouse_connected) {
                usb_mouse_buttons = 0;
                if (right_click_ctrl_injected) {
                    releaseAdbKey(0x36);
                    right_click_ctrl_injected = false;
                }
            }
        }
    }

    void enumerateDevice(int slot, const usb_config_desc_t *config_desc) {
        UsbDeviceSlot *dev = &devices[slot];
        const uint8_t *p = &config_desc->val[0];
        uint8_t bLength;

        uint8_t cur_intf_class = 0;
        uint8_t cur_intf_subclass = 0;
        uint8_t cur_intf_protocol = 0;
        uint8_t cur_intf_number = 0;
        bool cur_intf_claimed = false;
        bool cur_intf_is_mouse = false;

        /* If we find a HID class descriptor for the mouse interface,
         * remember its number and the report-descriptor length so we
         * can fetch it after enumeration completes. */
        bool     mouse_hid_found  = false;
        uint8_t  mouse_intf_num   = 0;
        uint16_t mouse_rdesc_len  = 0;

        for (int i = 0; i < config_desc->wTotalLength; i += bLength, p += bLength) {
            bLength = *p;
            if (bLength == 0 || (i + bLength) > config_desc->wTotalLength) break;

            uint8_t bDescriptorType = *(p + 1);

            if (bDescriptorType == USB_INTERFACE_DESC) {
                const usb_intf_desc_t *intf = (const usb_intf_desc_t *)p;
                cur_intf_class = intf->bInterfaceClass;
                cur_intf_subclass = intf->bInterfaceSubClass;
                cur_intf_protocol = intf->bInterfaceProtocol;
                cur_intf_number = intf->bInterfaceNumber;
                cur_intf_claimed = false;
                cur_intf_is_mouse = false;

                if (cur_intf_class == USB_CLASS_HID) {
                    esp_err_t err = usb_host_interface_claim(
                        clientHandle, dev->handle,
                        intf->bInterfaceNumber, intf->bAlternateSetting);
                    if (err == ESP_OK) {
                        cur_intf_claimed = true;
                        if (dev->interface_count < MAX_INTERFACES_PER_DEVICE) {
                            dev->interfaces[dev->interface_count++] = intf->bInterfaceNumber;
                        }
                        if (cur_intf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
                            dev->has_keyboard = true;
                        } else {
                            dev->has_mouse = true;
                            cur_intf_is_mouse = true;
                        }
                    }
                }

            } else if (bDescriptorType == USB_HID_DESC_TYPE && cur_intf_claimed) {
                /* HID class descriptor: 9 bytes minimum.
                 *   bLength, bDescriptorType, bcdHID(2), bCountryCode,
                 *   bNumDescriptors, bDescriptorType_subord(=0x22 Report),
                 *   wDescriptorLength(2)
                 * We only consume the first subordinate descriptor entry,
                 * which is the standard layout for ~all mice. */
                if (bLength >= 9 && cur_intf_is_mouse && !mouse_hid_found) {
                    uint8_t  sub_type = p[6];
                    uint16_t sub_len  = (uint16_t)p[7] | ((uint16_t)p[8] << 8);
                    if (sub_type == USB_HID_REPORT_DESC_TYPE && sub_len > 0 &&
                        sub_len <= HID_REPORT_DESC_MAX_BYTES) {
                        mouse_hid_found = true;
                        mouse_intf_num  = cur_intf_number;
                        mouse_rdesc_len = sub_len;
                    }
                }

            } else if (bDescriptorType == USB_ENDPOINT_DESC && cur_intf_claimed) {
                const usb_ep_desc_t *ep_desc = (const usb_ep_desc_t *)p;

                if ((ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK)
                    != USB_BM_ATTRIBUTES_XFER_INT)
                    continue;
                if (!(ep_desc->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK))
                    continue;

                uint8_t ep_num = USB_EP_DESC_GET_EP_NUM(ep_desc);
                if (ep_num < MAX_ENDPOINTS) {
                    dev->endpoint_info[ep_num].bInterfaceClass = cur_intf_class;
                    dev->endpoint_info[ep_num].bInterfaceSubClass = cur_intf_subclass;
                    dev->endpoint_info[ep_num].bInterfaceProtocol = cur_intf_protocol;
                }

                if (dev->transfer_count < MAX_TRANSFERS_PER_DEVICE) {
                    usb_transfer_t *xfer = NULL;
                    esp_err_t err = usb_host_transfer_alloc(
                        ep_desc->wMaxPacketSize + 1, 0, &xfer);
                    if (err == ESP_OK && xfer != NULL) {
                        xfer->device_handle = dev->handle;
                        xfer->bEndpointAddress = ep_desc->bEndpointAddress;
                        xfer->callback = transferCallback;
                        xfer->context = (void *)(uintptr_t)slot;
                        xfer->num_bytes = ep_desc->wMaxPacketSize;
                        dev->transfers[dev->transfer_count++] = xfer;
                        dev->interval = ep_desc->bInterval;
                        dev->ready = true;
                    }
                }
            }
        }

        /* If we found a mouse HID interface and recorded its report-
         * descriptor length, fire off a control transfer to fetch the
         * descriptor. The completion callback will parse it into
         * dev->mouse_layout. Until that lands, processMouseReport()
         * uses its existing heuristic decoder. */
        if (mouse_hid_found && mouse_rdesc_len > 0) {
            kickHidReportRead(slot, mouse_intf_num, mouse_rdesc_len);
        }
    }

    /*
     * Issue GET_DESCRIPTOR(REPORT) on the device's default control
     * endpoint. The transfer buffer has 8 bytes of setup packet
     * followed by the requested wLength of data; on completion the
     * callback parses bytes [8 .. 8+wLength) as the report descriptor.
     */
    void kickHidReportRead(int slot, uint8_t intf_num, uint16_t length) {
        UsbDeviceSlot *dev = &devices[slot];
        if (dev->handle == NULL) return;

        usb_transfer_t *xfer = NULL;
        esp_err_t err = usb_host_transfer_alloc(8 + length, 0, &xfer);
        if (err != ESP_OK || xfer == NULL) {
            Serial.printf("[USB] HID descriptor alloc failed: 0x%x\n", err);
            return;
        }

        xfer->num_bytes = 8 + length;
        xfer->data_buffer[0] = 0x81;  /* bmRequestType: D->H, Standard, Interface */
        xfer->data_buffer[1] = 0x06;  /* GET_DESCRIPTOR */
        xfer->data_buffer[2] = 0x00;  /* wValue low: descriptor index 0 */
        xfer->data_buffer[3] = USB_HID_REPORT_DESC_TYPE;  /* wValue high: Report */
        xfer->data_buffer[4] = intf_num;
        xfer->data_buffer[5] = 0x00;
        xfer->data_buffer[6] = (uint8_t)(length & 0xFF);
        xfer->data_buffer[7] = (uint8_t)((length >> 8) & 0xFF);

        xfer->device_handle = dev->handle;
        xfer->bEndpointAddress = 0x00;  /* default control pipe */
        xfer->callback = hidReportDescCallback;
        /* Pack slot index into the context so the callback knows which
         * device this came from. */
        xfer->context = (void *)(uintptr_t)slot;

        err = usb_host_transfer_submit_control(clientHandle, xfer);
        if (err != ESP_OK) {
            Serial.printf("[USB] HID descriptor submit failed: 0x%x\n", err);
            usb_host_transfer_free(xfer);
        }
    }

    static void hidReportDescCallback(usb_transfer_t *transfer) {
        if (!usbHost) {
            usb_host_transfer_free(transfer);
            return;
        }
        uint8_t slot = (uint8_t)(uintptr_t)transfer->context;
        if (slot >= MAX_USB_DEVICES) {
            usb_host_transfer_free(transfer);
            return;
        }
        UsbDeviceSlot *dev = &usbHost->devices[slot];

        do {
            if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
                Serial.printf("[USB] HID descriptor transfer status=%d\n",
                              transfer->status);
                break;
            }
            if (transfer->actual_num_bytes <= 8) {
                Serial.println("[USB] HID descriptor: no payload");
                break;
            }
            if (!dev->active) {
                break;
            }

            const uint8_t *desc = transfer->data_buffer + 8;
            size_t desc_len = transfer->actual_num_bytes - 8;

            HidMouseLayout layout;
            bool ok = HidParseMouseDescriptor(desc, desc_len, &layout);
            if (ok) {
                dev->mouse_layout = layout;
                Serial.printf("[HID] parsed report desc: btns=%d x=%d%c y=%d%c "
                              "wheel=%d%c report_id=%u bytes=%u\n",
                              layout.button_count,
                              layout.x_size_bits, layout.x_signed ? 's' : 'u',
                              layout.y_size_bits, layout.y_signed ? 's' : 'u',
                              layout.has_wheel ? layout.wheel_size_bits : 0,
                              (layout.has_wheel && layout.wheel_signed) ? 's' : 'u',
                              layout.report_id,
                              layout.report_bytes);
            } else {
                Serial.println("[HID] report desc parse failed - falling back to heuristics");
            }
        } while (0);

        usb_host_transfer_free(transfer);
    }

    static void transferCallback(usb_transfer_t *transfer) {
        if (!usbHost) return;
        if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) return;
        if (transfer->actual_num_bytes == 0) return;

        uint8_t dev_idx = (uint8_t)(uintptr_t)transfer->context;
        if (dev_idx >= MAX_USB_DEVICES) return;

        UsbDeviceSlot *dev = &usbHost->devices[dev_idx];
        if (!dev->active) return;

        uint8_t ep_num = transfer->bEndpointAddress & 0x0F;
        if (ep_num >= MAX_ENDPOINTS) return;

        EndpointInfo *ep_info = &dev->endpoint_info[ep_num];

        // Boot keyboard: HID class, Boot subclass, Keyboard protocol
        if (ep_info->bInterfaceClass == USB_CLASS_HID &&
            ep_info->bInterfaceSubClass == HID_SUBCLASS_BOOT &&
            ep_info->bInterfaceProtocol == HID_ITF_PROTOCOL_KEYBOARD &&
            transfer->actual_num_bytes >= 8) {

            hid_keyboard_report_t report;
            report.modifier = transfer->data_buffer[0];
            report.reserved = transfer->data_buffer[1];
            report.keycode[0] = transfer->data_buffer[2];
            report.keycode[1] = transfer->data_buffer[3];
            report.keycode[2] = transfer->data_buffer[4];
            report.keycode[3] = transfer->data_buffer[5];
            report.keycode[4] = transfer->data_buffer[6];
            report.keycode[5] = transfer->data_buffer[7];

            if (keyboard_enabled &&
                memcmp(&report, &dev->last_kb_report, sizeof(report)) != 0) {
                processKeyboardReport(&report, &dev->last_kb_report);
                memcpy(&dev->last_kb_report, &report, sizeof(report));
            }

        } else if (ep_info->bInterfaceClass == USB_CLASS_HID &&
                   ep_info->bInterfaceProtocol != HID_ITF_PROTOCOL_KEYBOARD) {
            processMouseReport(transfer, ep_info, &dev->mouse_layout);
        }
    }
};

// ============================================================================
// Touch Input Handling
// ============================================================================
//
// All touch handling (single-finger mouse, 3/4-finger gesture, overlay key
// press/release, 25% stipple compositor hooks) now lives in touch_overlay.cpp
// so the mouse pipeline and the overlay pipeline share one view of the
// touchscreen. We pump it here with the current multi-point state.

static void processTouchInput(void)
{
    if (!touch_enabled) return;

    BoardTouchMulti multi;
    BoardTouch_GetMulti(&multi);
    TouchOverlay_Update(&multi);
}

/*
 *  Check and update keyboard LED state
 */
static void updateKeyboardLEDs(void)
{
    if (usbHost == NULL || !usbHost->hasAnyKeyboard()) {
        return;
    }
    
    uint32_t now = millis();
    if ((now - last_led_check_time) < LED_CHECK_INTERVAL_MS) {
        return;
    }
    last_led_check_time = now;
    
    uint8_t current_leds = ADBGetKeyboardLEDs();
    
    if (current_leds != last_led_state) {
        usbHost->setKeyboardLEDs(current_leds);
        last_led_state = current_leds;
    }
}

// ============================================================================
// Input Task (runs on Core 0)
// ============================================================================

/*
 *  Input polling task - runs on Core 0 independently of CPU emulation
 *  This offloads the ~2.3ms USB host processing from the CPU emulation loop
 */
static void inputTask(void *param)
{
    (void)param;
    Serial.println("[INPUT] Input task started on Core 0");
    
    const TickType_t poll_interval = pdMS_TO_TICKS(INPUT_POLL_INTERVAL_MS);
    uint8_t usb_poll_divider = USB_POLL_DIV_ACTIVE;
    uint8_t usb_poll_counter = 0;
    
    while (input_task_running) {
        Board_Update();
        
        processTouchInput();
        pollTab5Keyboard();
        
        if (usbHost != NULL) {
            bool usb_active = usb_keyboard_connected || mouse_connected;
            uint8_t target_divider = usb_active ? USB_POLL_DIV_ACTIVE : USB_POLL_DIV_IDLE;
            if (target_divider != usb_poll_divider) {
                usb_poll_divider = target_divider;
                usb_poll_counter = 0;
            }

            usb_poll_counter++;
            if (usb_poll_counter >= usb_poll_divider) {
                usb_poll_counter = 0;
                usbHost->task();
            }
        }
        
        updateKeyboardLEDs();
        
        vTaskDelay(poll_interval);
    }
    
    if (input_task_stopped != NULL) {
        xSemaphoreGive(input_task_stopped);
    }
    input_task_handle = NULL;
    Serial.println("[INPUT] Input task exiting");
    vTaskDelete(NULL);
}

// ============================================================================
// Public API
// ============================================================================

bool InputInit(void)
{
    Serial.println("[INPUT] Initializing input subsystem...");
    
    display_width  = BoardDisplay_Width();
    display_height = BoardDisplay_Height();
    
    Serial.printf("[INPUT] Display size: %dx%d\n", display_width, display_height);
    Serial.printf("[INPUT] Mac screen size: %dx%d\n", mac_screen_width, mac_screen_height);

    last_led_state = 0;
    last_led_check_time = 0;
    memset(adb_key_claims, 0, sizeof(adb_key_claims));
    memset(tab5_bindings, 0, sizeof(tab5_bindings));
    tab5_sym_active = false;
    tab5_keyboard_connected = false;
    usb_keyboard_connected = false;

    ADBSetRelMouseMode(false);

    /* Overlay module owns mouse routing, multi-touch gesture detection,
     * and on-screen keyboard key dispatch. Initialize it with the actual
     * physical display size and Mac framebuffer size so the stipple
     * compositor and mouse-coordinate scaling are both correct. */
    TouchOverlay_Init(display_width, display_height,
                      mac_screen_width, mac_screen_height);

    Serial.println("[INPUT] Touch input enabled (multi-touch overlay ready)");

    if (!BoardKeyboard_Init()) {
        Serial.println("[INPUT] WARN: Tab5 Keyboard bus unavailable; continuing without it");
    }
    tab5_keyboard_connected = BoardKeyboard_IsConnected();
    
    Serial.println("[INPUT] Initializing USB Host (hub support enabled)...");
    usbHost = new MultiDeviceUsbHost();
    if (usbHost != NULL) {
        usbHost->begin();
        Serial.println("[INPUT] USB Host initialized - connect keyboard/mouse (hub supported)");
    } else {
        Serial.println("[INPUT] ERROR: Failed to create USB Host instance");
    }
    
    if (input_task_stopped == NULL) {
        input_task_stopped = xSemaphoreCreateBinary();
    }
    if (input_task_stopped == NULL) {
        Serial.println("[INPUT] ERROR: Failed to create task-stop semaphore");
        return false;
    }

    input_task_running = true;
    BaseType_t result = xTaskCreatePinnedToCore(
        inputTask,
        "InputTask",
        INPUT_TASK_STACK_SIZE,
        NULL,
        INPUT_TASK_PRIORITY,
        &input_task_handle,
        INPUT_TASK_CORE
    );
    
    if (result != pdPASS) {
        Serial.println("[INPUT] ERROR: Failed to create input task");
        input_task_running = false;
    } else {
        Serial.printf("[INPUT] Input task created on Core %d\n", INPUT_TASK_CORE);
    }
    
    return true;
}

void InputExit(void)
{
    Serial.println("[INPUT] Shutting down input subsystem");
    
    if (input_task_handle != NULL) {
        input_task_running = false;
        if (input_task_stopped == NULL ||
            xSemaphoreTake(input_task_stopped, pdMS_TO_TICKS(5000)) != pdTRUE) {
            /* Do not force-delete a task that may own Wire/USB state. Leaving
             * its resources allocated is safer than tearing them out from
             * underneath it; this path indicates a deeper driver deadlock. */
            Serial.println("[INPUT] ERROR: input task did not stop; resources left intact");
            return;
        }
    }

    /* Release any held overlay keys / mouse state. */
    TouchOverlay_Shutdown();

    releaseAllTab5Bindings();
    tab5_keyboard_connected = false;
    BoardKeyboard_Exit();

    // Release any injected right-click Control key
    if (right_click_ctrl_injected) {
        releaseAdbKey(0x36);
        right_click_ctrl_injected = false;
    }
    
    if (usbHost != NULL) {
        usbHost->releaseAllKeyboardClaims();
        delete usbHost;
        usbHost = NULL;
    }
    if (input_task_stopped != NULL) {
        vSemaphoreDelete(input_task_stopped);
        input_task_stopped = NULL;
    }
}

void InputPoll(void)
{
    processTouchInput();
    
    if (usbHost != NULL) {
        usbHost->task();
    }
    
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
    TouchOverlay_SetTouchEnabled(enabled);
}

void InputSetKeyboardEnabled(bool enabled)
{
    if (!enabled && keyboard_enabled && usbHost != NULL) {
        /* last_kb_report is the ownership snapshot for each USB slot. Clear
         * every claim before reports are ignored so a later disconnect cannot
         * decrement a key owned by the Tab5 keyboard or touch overlay. */
        usbHost->releaseAllKeyboardClaims();
    }
    keyboard_enabled = enabled;
}

bool InputIsKeyboardConnected(void)
{
    return usb_keyboard_connected || tab5_keyboard_connected;
}

bool InputIsMouseConnected(void)
{
    return mouse_connected;
}

// ============================================================================
// Legacy functions (kept for compatibility, now handled via USB Host callbacks)
// ============================================================================

void InputProcessKeyboardReport(const uint8_t *report, int length)
{
    (void)report;
    (void)length;
}

void InputProcessMouseReport(const uint8_t *report, int length)
{
    (void)report;
    (void)length;
}
