/*
 * board_keyboard_tab5.cpp - M5Stack Tab5 Keyboard raw I2C driver
 *
 * The optional 70-key keyboard connects to Ext.Port1 on GPIO0/1 and reports
 * raw 5x14 matrix transitions. Normal mode is deliberately used instead of
 * the device's two-byte HID mode: Normal mode preserves modifier-only events
 * and simultaneous key state, leaving layout and ADB translation to the
 * emulator input layer.
 */

#include "board_keyboard.h"

#include <Arduino.h>
#include <Wire.h>

#include "esp_log.h"

namespace {

static const char *TAG = "board_keyboard";

constexpr uint8_t TAB5_KEYBOARD_ADDRESS = 0x6D;
constexpr int TAB5_KEYBOARD_SDA = 0;
constexpr int TAB5_KEYBOARD_SCL = 1;
constexpr int TAB5_KEYBOARD_INT = 50;
constexpr uint32_t TAB5_KEYBOARD_I2C_HZ = 400000;
constexpr uint16_t TAB5_KEYBOARD_I2C_TIMEOUT_MS = 20;

constexpr uint8_t REG_INT_CFG = 0x00;
constexpr uint8_t REG_INT_STAT = 0x01;
constexpr uint8_t REG_EVENT_NUM = 0x02;
constexpr uint8_t REG_KEYBOARD_MODE = 0x10;
constexpr uint8_t REG_KEY_EVENT = 0x20;
constexpr uint8_t REG_FIRMWARE_VERSION = 0xFE;

constexpr uint8_t MODE_NORMAL = 0;
constexpr uint8_t INT_NORMAL_ENABLE = 0x01;
constexpr uint8_t EVENT_QUEUE_MAX = 32;

/* Connected devices get three quick recovery attempts before being declared
 * gone. Once absent, probing is deliberately slow so an unpopulated port has
 * effectively no steady-state cost. */
constexpr uint32_t IO_RETRY_MS = 75;
constexpr uint32_t PROBE_INTERVAL_MS = 1500;
constexpr uint32_t HEALTH_INTERVAL_MS = 1000;
constexpr uint8_t FAILURE_LIMIT = 3;

bool s_bus_ready = false;
bool s_connected = false;
uint8_t s_firmware_version = 0;
uint8_t s_pending_events = 0;
uint8_t s_consecutive_failures = 0;
uint32_t s_next_io_ms = 0;
uint32_t s_next_probe_ms = 0;
uint32_t s_next_health_ms = 0;

bool deadline_reached(uint32_t now, uint32_t deadline)
{
    return static_cast<int32_t>(now - deadline) >= 0;
}

bool begin_bus()
{
    /* M5Unified uses I2C1 for Tab5's internal devices. Arduino Wire is I2C0,
     * which is the controller used by M5Stack's official keyboard example. */
    Wire.end();
    Wire.setTimeOut(TAB5_KEYBOARD_I2C_TIMEOUT_MS);
    s_bus_ready = Wire.begin(TAB5_KEYBOARD_SDA, TAB5_KEYBOARD_SCL,
                             TAB5_KEYBOARD_I2C_HZ);
    if (!s_bus_ready) {
        ESP_LOGW(TAG, "failed to initialize Ext.Port1 I2C bus");
    }
    return s_bus_ready;
}

bool read_registers(uint8_t reg, uint8_t *data, size_t length)
{
    if (!s_bus_ready || data == nullptr || length == 0) return false;

    Wire.beginTransmission(TAB5_KEYBOARD_ADDRESS);
    if (Wire.write(reg) != 1 || Wire.endTransmission(false) != 0) {
        return false;
    }

    const size_t received = Wire.requestFrom(
        TAB5_KEYBOARD_ADDRESS, length, true);
    if (received != length) {
        while (Wire.available() > 0) (void)Wire.read();
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        if (Wire.available() <= 0) return false;
        data[i] = static_cast<uint8_t>(Wire.read());
    }
    return true;
}

bool read_register(uint8_t reg, uint8_t &value)
{
    return read_registers(reg, &value, 1);
}

bool write_register(uint8_t reg, uint8_t value)
{
    if (!s_bus_ready) return false;

    Wire.beginTransmission(TAB5_KEYBOARD_ADDRESS);
    if (Wire.write(reg) != 1 || Wire.write(value) != 1) return false;
    return Wire.endTransmission(true) == 0;
}

void mark_success()
{
    s_consecutive_failures = 0;
    s_next_io_ms = 0;
}

void mark_failure(uint32_t now)
{
    if (s_consecutive_failures < UINT8_MAX) ++s_consecutive_failures;
    s_next_io_ms = now + IO_RETRY_MS;

    if (s_consecutive_failures < FAILURE_LIMIT) return;

    if (s_connected) {
        ESP_LOGW(TAG, "Tab5 Keyboard disconnected after %u I2C failures",
                 static_cast<unsigned>(s_consecutive_failures));
    }
    s_connected = false;
    s_pending_events = 0;
    s_firmware_version = 0;
    s_next_probe_ms = now + PROBE_INTERVAL_MS;

    /* A hard I2C failure can leave the controller busy. Recreate it before
     * the next slow attach probe rather than repeatedly blocking input. */
    Wire.end();
    s_bus_ready = false;
}

bool validate_identity(uint8_t &firmware_version)
{
    /* 0xFE and 0xFF are contiguous: firmware version followed by the address
     * stored in the keyboard's flash. Checking both prevents writes to an
     * unrelated device that happens to acknowledge 0x6D. */
    uint8_t identity[2] = {};
    if (!read_registers(REG_FIRMWARE_VERSION, identity, sizeof(identity))) {
        return false;
    }
    if (identity[0] == 0x00 || identity[0] == 0xFF ||
        identity[1] != TAB5_KEYBOARD_ADDRESS) {
        return false;
    }
    firmware_version = identity[0];
    return true;
}

bool probe_keyboard(uint32_t now)
{
    if (!s_bus_ready && !begin_bus()) {
        s_next_probe_ms = now + PROBE_INTERVAL_MS;
        return false;
    }

    uint8_t firmware_version = 0;
    if (!validate_identity(firmware_version)) {
        s_next_probe_ms = now + PROBE_INTERVAL_MS;
        return false;
    }

    /* Stop device interrupts while switching modes. A mode write alone does
     * not clear the FIFO when the device was already in Normal mode, so clear
     * the queue explicitly before exposing the connection to the input layer. */
    if (!write_register(REG_INT_CFG, 0x00) ||
        !write_register(REG_KEYBOARD_MODE, MODE_NORMAL) ||
        !write_register(REG_EVENT_NUM, 0x00) ||
        !write_register(REG_INT_STAT, 0x00) ||
        !write_register(REG_INT_CFG, INT_NORMAL_ENABLE)) {
        s_next_probe_ms = now + PROBE_INTERVAL_MS;
        return false;
    }

    uint8_t mode = 0xFF;
    if (!read_register(REG_KEYBOARD_MODE, mode) || mode != MODE_NORMAL) {
        s_next_probe_ms = now + PROBE_INTERVAL_MS;
        return false;
    }

    s_connected = true;
    s_firmware_version = firmware_version;
    s_pending_events = 0;
    s_consecutive_failures = 0;
    s_next_io_ms = 0;
    s_next_health_ms = now + HEALTH_INTERVAL_MS;
    ESP_LOGI(TAG, "Tab5 Keyboard detected (firmware 0x%02X)",
             s_firmware_version);
    return true;
}

bool refresh_event_count(uint32_t now)
{
    uint8_t count = 0;
    if (!read_register(REG_EVENT_NUM, count) || count > EVENT_QUEUE_MAX) {
        mark_failure(now);
        return false;
    }
    s_pending_events = count;
    mark_success();
    return true;
}

bool clear_drained_interrupt(uint32_t now)
{
    /* Only release INT after a successful live count confirms that the FIFO
     * is empty. On any read/write failure it remains asserted for retry. */
    uint8_t count = 0;
    if (!read_register(REG_EVENT_NUM, count) || count > EVENT_QUEUE_MAX) {
        mark_failure(now);
        return false;
    }
    s_pending_events = count;
    if (count != 0) {
        mark_success();
        return true;
    }
    if (!write_register(REG_INT_STAT, 0x00)) {
        mark_failure(now);
        return false;
    }
    mark_success();
    return true;
}

bool health_check(uint32_t now)
{
    uint8_t version = 0;
    if (!read_register(REG_FIRMWARE_VERSION, version) ||
        version != s_firmware_version) {
        mark_failure(now);
        return false;
    }
    mark_success();
    s_next_health_ms = now + HEALTH_INTERVAL_MS;
    return true;
}

} // namespace

extern "C" bool BoardKeyboard_Init(void)
{
    pinMode(TAB5_KEYBOARD_INT, INPUT_PULLUP);

    s_connected = false;
    s_firmware_version = 0;
    s_pending_events = 0;
    s_consecutive_failures = 0;
    s_next_io_ms = 0;
    s_next_probe_ms = 0;
    s_next_health_ms = 0;

    /* Keyboard absence is expected and non-fatal. Even a transient bus setup
     * failure is retried later from Poll(). */
    (void)begin_bus();
    const uint32_t now = millis();
    if (!probe_keyboard(now)) s_next_probe_ms = now + PROBE_INTERVAL_MS;
    return true;
}

extern "C" bool BoardKeyboard_Poll(BoardKeyboardEvent *event)
{
    const uint32_t now = millis();

    if (!s_connected) {
        if (deadline_reached(now, s_next_probe_ms)) (void)probe_keyboard(now);
        return false;
    }

    if (!deadline_reached(now, s_next_io_ms)) return false;

    const bool health_due = deadline_reached(now, s_next_health_ms);
    if (health_due && !health_check(now)) return false;

    /* Never consume a queued key unless the caller supplied storage for it. */
    if (event == nullptr) return false;

    /* INT is active-low. The periodic count read on health checks doubles as
     * recovery if an edge/level was missed or the line was disturbed. */
    if (s_pending_events == 0 &&
        digitalRead(TAB5_KEYBOARD_INT) != LOW && !health_due) {
        return false;
    }

    if (s_pending_events == 0 && !refresh_event_count(now)) return false;
    if (s_pending_events == 0) {
        if (digitalRead(TAB5_KEYBOARD_INT) == LOW) {
            (void)clear_drained_interrupt(now);
        }
        return false;
    }

    /* A malformed event is dropped, but the remaining FIFO stays available
     * to subsequent Poll calls. Consume at most the original queue depth here
     * so corrupt data can never spin forever. */
    for (uint8_t attempts = 0;
         attempts < EVENT_QUEUE_MAX && s_pending_events > 0; ++attempts) {
        uint8_t raw = 0xFF;
        if (!read_register(REG_KEY_EVENT, raw)) {
            mark_failure(now);
            return false;
        }
        mark_success();
        --s_pending_events;

        if (s_pending_events == 0) (void)clear_drained_interrupt(now);

        if (raw == 0xFF) continue;
        const uint8_t row = static_cast<uint8_t>((raw >> 4) & 0x07);
        const uint8_t col = static_cast<uint8_t>(raw & 0x0F);
        if (row >= 5 || col >= 14) {
            ESP_LOGW(TAG, "dropping invalid key event 0x%02X", raw);
            continue;
        }

        event->row = row;
        event->col = col;
        event->pressed = (raw & 0x80) != 0;
        return true;
    }

    return false;
}

extern "C" bool BoardKeyboard_IsConnected(void)
{
    return s_connected;
}

extern "C" void BoardKeyboard_Exit(void)
{
    if (s_bus_ready && s_connected) {
        /* Best effort only: input teardown must not block on a removed module. */
        (void)write_register(REG_INT_CFG, 0x00);
        (void)write_register(REG_INT_STAT, 0x00);
    }
    Wire.end();
    s_bus_ready = false;
    s_connected = false;
    s_firmware_version = 0;
    s_pending_events = 0;
    s_consecutive_failures = 0;
}
