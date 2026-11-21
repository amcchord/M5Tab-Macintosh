#include "tab5_sd_idf.h"
#include "tab5_sd_host.h"

#include <Arduino.h>
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_vfs_fat.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <vfs_api.h>

esp_err_t Tab5SD_InitSpiBus(sdmmc_host_t &host, sdspi_device_config_t &device)
{
    spi_bus_config_t bus = {};
    bus.mosi_io_num = BOARD_SD_SPI_MOSI;
    bus.miso_io_num = BOARD_SD_SPI_MISO;
    bus.sclk_io_num = BOARD_SD_SPI_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 16 * 1024;

    device = SDSPI_DEVICE_CONFIG_DEFAULT();
    device.gpio_cs = (gpio_num_t)BOARD_SD_SPI_CS;
    host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 10000;
    return spi_bus_initialize(device.host_id, &bus, SPI_DMA_CH_AUTO);
}

esp_err_t Tab5SD_NativeConfig(sdmmc_host_t &host, sdmmc_slot_config_t &slot)
{
    host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    // Slot 1 belongs to the C6. Retain SDMMC_HOST_FLAG_DEINIT_ARG so IDF
    // tears down just the card's slot when a warm card rejects native mode.
    slot = SDMMC_SLOT_CONFIG_DEFAULT();
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
    return err;
}

static bool can_retry_mount = true;

void Tab5SD_BlockMounts()
{
    can_retry_mount = false;
}

bool Tab5SD_CanRetryMount()
{
    return can_retry_mount;
}

#if TAB5_SD_IDF
static sdmmc_card_t *mounted_card;
static constexpr esp_vfs_fat_mount_config_t mount_options = {
    .format_if_mount_failed = false,
    .max_files = 5,
};

sdmmc_card_t *Tab5SD_Card()
{
    return mounted_card;
}

bool Tab5SD_UnmountIdf()
{
    if (!mounted_card) return true;
    const bool spi = mounted_card->host.flags & SDMMC_HOST_FLAG_SPI;
    esp_err_t err = esp_vfs_fat_sdcard_unmount(BOARD_SD_MOUNT_POINT, mounted_card);
    mounted_card = nullptr;
    if (err == ESP_OK && spi) err = spi_bus_free(SDSPI_DEFAULT_HOST);
    can_retry_mount = err == ESP_OK;
    return can_retry_mount;
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
        // IDF 5.5 can publish out_card before allocating the final mount
        // context. On that failure /sd is already registered and the card
        // pointer is stale. Never reuse either the pointer or mount point.
        if (card) can_retry_mount = false;
        Serial.printf("[SD] %s mount failed: %s\n", name, esp_err_to_name(err));
        if (!can_retry_mount) Serial.println("[SD] Bus or mount point still owned; restart required");
        return false;
    }
    mounted_card = card;
    int khz = 0;
    card->host.get_real_freq(card->host.slot, &khz);
    Serial.printf("[SD] %s %d kHz, %llu MB\n", name, khz,
                  (unsigned long long)BoardSD_CardSizeMB());
    return true;
}

#if TAB5_SD_DMA || TAB5_SD_BENCHMARK
bool Tab5SD_MountDma()
{
    sdmmc_host_t host;
    sdspi_device_config_t device;
    esp_err_t err = Tab5SD_InitSpiBus(host, device);
    sdmmc_card_t *card = nullptr;
    if (err == ESP_OK) {
        err = esp_vfs_fat_sdspi_mount(BOARD_SD_MOUNT_POINT, &host, &device, &mount_options, &card);
        if (err != ESP_OK && spi_bus_free(device.host_id) != ESP_OK) {
            can_retry_mount = false;
        }
    } else if (err == ESP_ERR_INVALID_STATE) {
        // Arduino SPI also uses SPI2. Do not attach it to an occupied bus.
        can_retry_mount = false;
    }
    return finishMount(err, "SPI DMA", card);
}
#endif

#if TAB5_SD_NATIVE || TAB5_SD_BENCHMARK
bool Tab5SD_MountNative()
{
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    esp_err_t err = Tab5SD_NativeConfig(host, slot);
    sdmmc_card_t *card = nullptr;
    if (err == ESP_OK) {
        err = esp_vfs_fat_sdmmc_mount(BOARD_SD_MOUNT_POINT, &host, &slot, &mount_options, &card);
    }
    return finishMount(err, "SDMMC slot 0", card);
}
#endif
#endif
