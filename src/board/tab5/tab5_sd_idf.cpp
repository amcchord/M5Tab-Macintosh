#include "tab5_sd_idf.h"

#if TAB5_SD_IDF
#include <Arduino.h>
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_vfs_fat.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <vfs_api.h>

static sdmmc_card_t *mounted_card;
static constexpr esp_vfs_fat_mount_config_t mount_options = {
    .format_if_mount_failed = false,
    .max_files = 5,
};

const sdmmc_card_t *Tab5SD_Card()
{
    return mounted_card;
}

fs::FS &BoardSD_Filesystem()
{
    if (!mounted_card) return SD;
    static fs::FS filesystem([]() -> fs::FSImplPtr {
        auto impl = std::make_shared<VFSImpl>();
        impl->mountpoint(BOARD_SD_MOUNT_POINT);
        return impl;
    }());
    return filesystem;
}

static bool finishMount(esp_err_t err, const char *name, sdmmc_card_t *card)
{
    if (err != ESP_OK) {
        Serial.printf("[SD] %s mount failed: %s\n", name, esp_err_to_name(err));
        return false;
    }
    mounted_card = card;
    int khz = 0;
    card->host.get_real_freq(card->host.slot, &khz);
    Serial.printf("[SD] %s %d kHz, %llu MB\n", name, khz,
                  (unsigned long long)BoardSD_CardSizeMB());
    return true;
}

#if defined(TAB5_SD_DMA) || defined(TAB5_SD_BENCHMARK)
bool Tab5SD_MountDma()
{
    spi_bus_config_t bus = {};
    bus.mosi_io_num = BOARD_SD_SPI_MOSI;
    bus.miso_io_num = BOARD_SD_SPI_MISO;
    bus.sclk_io_num = BOARD_SD_SPI_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 16 * 1024;

    sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
    device.gpio_cs = (gpio_num_t)BOARD_SD_SPI_CS;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 10000;
    esp_err_t err = spi_bus_initialize(device.host_id, &bus, SPI_DMA_CH_AUTO);
    sdmmc_card_t *card = nullptr;
    if (err == ESP_OK) {
        err = esp_vfs_fat_sdspi_mount(BOARD_SD_MOUNT_POINT, &host, &device, &mount_options, &card);
        if (err != ESP_OK) spi_bus_free(device.host_id);
    }
    return finishMount(err, "SPI DMA", card);
}
#endif

#if defined(TAB5_SD_NATIVE) || defined(TAB5_SD_BENCHMARK)
bool Tab5SD_MountNative()
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    // Slot 1 belongs to the C6. Retain SDMMC_HOST_FLAG_DEINIT_ARG so IDF
    // tears down just the card's slot when a warm card rejects native mode.
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = (gpio_num_t)BOARD_SD_SPI_SCK;
    slot.cmd = (gpio_num_t)BOARD_SD_SPI_MOSI;
    slot.d0 = (gpio_num_t)BOARD_SD_SPI_MISO;
    slot.d1 = GPIO_NUM_40;
    slot.d2 = GPIO_NUM_41;
    slot.d3 = (gpio_num_t)BOARD_SD_SPI_CS;

    sd_pwr_ctrl_ldo_config_t ldo = {};
    ldo.ldo_chan_id = 4;
    // Keep SD I/O at 3.3 V for the lifetime of the boot, including a
    // fallback to SPI. Releasing channel 4 here would disable the rail.
    static sd_pwr_ctrl_handle_t supply;
    esp_err_t err = ESP_OK;
    if (!supply) err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &supply);
    if (err == ESP_OK) err = sd_pwr_ctrl_set_io_voltage(supply, 3300);
    host.pwr_ctrl_handle = supply;
    sdmmc_card_t *card = nullptr;
    if (err == ESP_OK) {
        err = esp_vfs_fat_sdmmc_mount(BOARD_SD_MOUNT_POINT, &host, &slot, &mount_options, &card);
    }
    return finishMount(err, "SDMMC slot 0", card);
}
#endif
#endif
