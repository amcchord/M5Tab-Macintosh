/*
 * board_wifi_firmware_tab5.cpp - No-op co-processor OTA on Tab5.
 *
 * The stock C6 runs the Wi-Fi radio over SDIO. The P4 configures that
 * link through Arduino WiFi.setPins(). This target does not bundle or
 * install a C6 firmware image.
 */

#include "board_wifi.h"

extern "C" bool BoardWifi_IsCoprocessorFirmwareOutdated(void)
{
    return false;
}

extern "C" BoardWifiFirmwareResult BoardWifi_ApplyCoprocessorFirmware(void)
{
    return BOARD_WIFI_FW_NOT_SUPPORTED;
}
