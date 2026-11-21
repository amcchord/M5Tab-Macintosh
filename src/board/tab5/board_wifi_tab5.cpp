/*
 * board_wifi_tab5.cpp - WiFi transport pin setup for M5Stack Tab5.
 *
 * The Tab5 routes WiFi 6 through an ESP32-C6 co-processor connected to the
 * P4 over SDIO2. The Arduino WiFi library needs these pins set explicitly
 * before any scan/begin call.
 */

#include "board_wifi.h"

#include <Arduino.h>
#include <WiFi.h>

/* Tab5 SDIO2 pins for ESP32-C6 communication. */
#define TAB5_WIFI_SDIO_CLK  GPIO_NUM_12
#define TAB5_WIFI_SDIO_CMD  GPIO_NUM_13
#define TAB5_WIFI_SDIO_D0   GPIO_NUM_11
#define TAB5_WIFI_SDIO_D1   GPIO_NUM_10
#define TAB5_WIFI_SDIO_D2   GPIO_NUM_9
#define TAB5_WIFI_SDIO_D3   GPIO_NUM_8
#define TAB5_WIFI_SDIO_RST  GPIO_NUM_15

extern "C" void BoardWifi_Prepare(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    BoardWifi_PrepareDmaMemory();
    WiFi.setPins(TAB5_WIFI_SDIO_CLK, TAB5_WIFI_SDIO_CMD, TAB5_WIFI_SDIO_D0,
                 TAB5_WIFI_SDIO_D1, TAB5_WIFI_SDIO_D2, TAB5_WIFI_SDIO_D3,
                 TAB5_WIFI_SDIO_RST);
}
