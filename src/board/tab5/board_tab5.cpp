/*
 * board_tab5.cpp - top-level board lifecycle for M5Stack Tab5.
 *
 * All the heavy lifting (display, touch, audio, power) is done by
 * M5Unified. This file just calls M5.begin() once, forwards M5.update()
 * for per-frame touch polling, and returns true.
 */

#include "board.h"

#include <M5Unified.h>
#include <Wire.h>
#include "esp_system.h"

static bool s_board_inited = false;

static bool writePi4Register(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(0x43);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

static void resetWarmBootPanel(void)
{
    // M5GFX uses controller 0 for the Tab5 internal I2C bus. Using Wire1 here
    // left controller 0's stale warm-reset state untouched, so the reset pulse
    // could succeed electrically yet the subsequent touch probe still fail.
    Wire.end();
    if (!Wire.begin(31, 32, 100000)) return;
    pinMode(23, OUTPUT);
    digitalWrite(23, HIGH);  // Touch-controller address select during reset.
    // PI4IOE5V6408 at 0x43: bit 4 is LCD_RESET and bit 5 is TP_RESET.
    // M5GFX pulses these too, but its 10 ms low interval is insufficient
    // after the P4 restarts while the panel and IO expander remain powered.
    (void)writePi4Register(0x03, 0x7f);
    (void)writePi4Register(0x07, 0x00);
    (void)writePi4Register(0x0d, 0x7f);
    (void)writePi4Register(0x0b, 0x7f);
    (void)writePi4Register(0x05, 0x46);
    delay(500);
    (void)writePi4Register(0x05, 0x76);
    delay(500);
    pinMode(23, INPUT);
    Wire.end();
}

extern "C" bool Board_Init(void)
{
    if (s_board_inited) {
        return true;
    }
    // The P4 can restart without removing power from the DSI panel. Give the
    // panel MCU time to leave its previous transaction state before M5GFX's
    // autodetection probes it; rapid USB/software resets otherwise sometimes
    // leave M5.Display with no panel handle for the entire boot.
    if (esp_reset_reason() != ESP_RST_POWERON) {
        delay(500);
        resetWarmBootPanel();
        delay(500);
    }
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(3);  /* landscape - matches pre-HAL behaviour */
    s_board_inited = true;
    return true;
}

extern "C" void Board_Update(void)
{
    M5.update();
}
