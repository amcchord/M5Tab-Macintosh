/*
 * board_wifi_waveshare.cpp - WiFi transport prep on Waveshare.
 *
 * Arduino-ESP32 (pioarduino 55.x) on the ESP32-P4 routes all WiFi calls
 * through esp_wifi_remote / esp_hosted, which talk to the ESP32-C6 co-
 * processor over SDIO. The SDIO pin configuration lives in sdkconfig (and
 * in the esp_hosted component defaults) so no explicit WiFi.setPins() is
 * needed. Prepare the shared P4 radio buffers before starting Wi-Fi.
 */

#include "board_wifi.h"

extern "C" void BoardWifi_Prepare(void)
{
    BoardWifi_PrepareDmaMemory();
}
