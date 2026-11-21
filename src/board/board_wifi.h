/*
 * board_wifi.h - WiFi transport bring-up
 *
 * Both boards ship an ESP32-C6 co-processor and expose its WiFi to the P4
 * over SDIO. Tab5 uses the Arduino WiFi.setPins() path with hard-coded pins.
 * Waveshare uses esp_wifi_remote / esp_hosted which auto-configures SDIO
 * based on sdkconfig, so no explicit pin setup is needed.
 *
 * Call BoardWifi_Prepare() exactly once, before any WiFi.begin() or
 * equivalent call. Both boards still use the Arduino WiFi API on top of
 * this transport, so boot_gui.cpp's scan/connect flow works unchanged.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure the WiFi transport pins and power on the co-processor.
 *        Must be called before WiFi.begin(). Safe to call once per boot.
 */
void BoardWifi_Prepare(void);

/** Select DMA-capable external memory before starting the P4 SDIO radio. */
void BoardWifi_PrepareDmaMemory(void);

/**
 * Result of @ref BoardWifi_MaybeUpdateCoprocessorFirmware.
 */
typedef enum {
    BOARD_WIFI_FW_NOT_SUPPORTED = 0, /**< Board has no OTA-able co-processor. */
    BOARD_WIFI_FW_UP_TO_DATE,        /**< Slave already matches host version. */
    BOARD_WIFI_FW_UPDATED,           /**< Flashed successfully; caller should reboot. */
    BOARD_WIFI_FW_FAILED,            /**< Tried and failed; continue without WiFi. */
} BoardWifiFirmwareResult;

/**
 * @brief Cheap yes/no check for whether an OTA is needed.
 *
 * Safe to call anytime after the hosted transport is up. Returns
 * ``true`` only when the host library version is strictly newer than
 * the slave - equal/older slaves are considered "up to date" so we
 * never try to downgrade. On Tab5 (no co-processor OTA path) this
 * always returns ``false``.
 *
 * Use this to decide whether to put up a user-visible "Updating WiFi
 * firmware..." splash before calling @ref BoardWifi_ApplyCoprocessorFirmware.
 */
bool BoardWifi_IsCoprocessorFirmwareOutdated(void);

/**
 * @brief Stream the embedded co-processor firmware into the slave.
 *
 * On Waveshare this streams the baked-in ``esp32c6-v<host>.bin`` into
 * the C6 via the ESP-Hosted slave OTA API (no network required; the
 * blob lives in flash, see scripts/fetch_c6_firmware.py). The call
 * takes 10-30s and blocks the caller - serial shows byte-level
 * progress.
 *
 * @note WiFi must already be initialized (``WiFi.mode(WIFI_STA)`` /
 *       esp_hosted link up) before calling this.
 * @return BOARD_WIFI_FW_UPDATED on success. The caller is responsible
 *         for rebooting so the new firmware comes up cleanly - the
 *         hosted RPC connection is torn down as part of activate.
 */
BoardWifiFirmwareResult BoardWifi_ApplyCoprocessorFirmware(void);

#ifdef __cplusplus
}
#endif
