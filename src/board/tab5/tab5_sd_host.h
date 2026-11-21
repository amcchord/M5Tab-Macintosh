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

// Used with the VFS unmounted and all storage users stopped. Regular-speed
// native initialization identifies the card independently of its filesystem.
inline esp_err_t Tab5SD_OpenNativeProbe(sdmmc_card_t &card, bool &slot_owned)
{
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    esp_err_t err = Tab5SD_NativeConfig(host, slot);
    if (err != ESP_OK) return err;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    err = sdmmc_host_init();
    if (err == ESP_OK) err = sdmmc_host_init_slot(host.slot, &slot);
    slot_owned = err == ESP_OK;
    return slot_owned ? sdmmc_card_init(&host, &card) : err;
}

// A card can supply its native CID/RCA before a later initialization step
// fails. Preserve its mode even in that case, or when probing lacked resources.
inline bool Tab5SD_MayTrySpi(const sdmmc_card_t &card, bool slot_owned, esp_err_t err)
{
    return slot_owned && err != ESP_OK && err != ESP_ERR_NO_MEM && !card.rca && !card.cid.mfg_id;
}

inline void Tab5SD_CloseNativeProbe(bool &slot_owned)
{
    if (slot_owned && sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_0) != ESP_OK) Tab5SD_BlockMounts();
    slot_owned = false;
}
