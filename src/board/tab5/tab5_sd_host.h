#pragma once

#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <sdmmc_cmd.h>

enum class Tab5SdMode { None, ArduinoSpi, DmaSpi, Native };
void Tab5SD_BlockMounts();
bool Tab5SD_CanRetryMount();
Tab5SdMode Tab5SD_MountedMode();
bool Tab5SD_Unmount();
bool Tab5SD_Restore(Tab5SdMode mode);
esp_err_t Tab5SD_InitSpiBus(sdmmc_host_t &host, sdspi_device_config_t &device);
esp_err_t Tab5SD_NativeConfig(sdmmc_host_t &host, sdmmc_slot_config_t &slot);
