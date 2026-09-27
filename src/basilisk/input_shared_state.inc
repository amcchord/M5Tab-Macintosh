// Shared by physical input and serial automation. A recursive task mutex
// covers the claim transition AND its ADB event so another task cannot send
// key-up before the corresponding key-down. ADB can block; use no spinlock.
static StaticSemaphore_t s_input_state_storage;
static SemaphoreHandle_t inputStateMutex()
{
    static SemaphoreHandle_t mutex = xSemaphoreCreateRecursiveMutexStatic(&s_input_state_storage);
    return mutex;
}
struct InputStateGuard {
    InputStateGuard() { xSemaphoreTakeRecursive(inputStateMutex(), portMAX_DELAY); }
    ~InputStateGuard() { xSemaphoreGiveRecursive(inputStateMutex()); }
};

/* Physical-key claims from every input source, indexed by final Mac ADB
 * keycode. USB slots, the Tab5 Keyboard matrix, and synthetic right-click
 * Control each own an independent claim. ADB sees a down event only on the
 * first claim and an up event only when the last claim is released, so two
 * keyboards can hold the same key/modifier without releasing each other. */
DRAM_ATTR static uint8_t adb_key_claims[128] = {};
DRAM_ATTR static bool automation_keys_down[128] = {};
static bool automation_mouse_buttons[3] = {};

static void claimAdbKey(uint8_t mac_keycode)
{
    InputStateGuard guard;
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
    InputStateGuard guard;
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

extern "C" void InputAutomationKey(uint8_t mac_keycode, bool pressed)
{
    InputStateGuard guard;
    if (mac_keycode >= 0x80 || automation_keys_down[mac_keycode] == pressed) {
        return;
    }
    automation_keys_down[mac_keycode] = pressed;
    if (pressed) {
        claimAdbKey(mac_keycode);
    } else {
        releaseAdbKey(mac_keycode);
    }
}

extern "C" void InputAutomationMouseMove(int x, int y, bool relative)
{
    InputStateGuard guard;
    if (!relative) {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= mac_screen_width) x = mac_screen_width - 1;
        if (y >= mac_screen_height) y = mac_screen_height - 1;
    }
    ADBSetRelMouseMode(relative);
    ADBMouseMoved(x, y);
}

extern "C" void InputAutomationMouseButton(uint8_t button, bool pressed)
{
    InputStateGuard guard;
    if (button >= 3 || automation_mouse_buttons[button] == pressed) {
        return;
    }
    automation_mouse_buttons[button] = pressed;
    if (pressed) {
        ADBMouseDown(button);
    } else {
        ADBMouseUp(button);
    }
}

extern "C" void InputAutomationReleaseAll(void)
{
    InputStateGuard guard;
    for (uint16_t code = 0; code < 128; ++code) {
        if (automation_keys_down[code]) {
            automation_keys_down[code] = false;
            releaseAdbKey((uint8_t)code);
        }
    }
    for (uint8_t button = 0; button < 3; ++button) {
        if (automation_mouse_buttons[button]) {
            automation_mouse_buttons[button] = false;
            ADBMouseUp(button);
        }
    }
}

static bool controlKeyClaimed()
{
    InputStateGuard guard;
    return adb_key_claims[0x36] != 0;
}

static void resetInputClaims()
{
    InputStateGuard guard;
    memset(adb_key_claims, 0, sizeof(adb_key_claims));
    memset(automation_keys_down, 0, sizeof(automation_keys_down));
    memset(automation_mouse_buttons, 0, sizeof(automation_mouse_buttons));
}
