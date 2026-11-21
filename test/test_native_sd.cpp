#include "platform.h"
#include <vector>

namespace fs { class FS {}; }
static unsigned spi_starts;
struct {
    bool begin(int, int, int, int) { ++spi_starts; return true; }
    void end() {}
} SPI;
struct {
    bool begin(int, decltype(SPI) &, int, const char *, int, bool) { return true; }
    uint64_t cardSize() { return uint64_t{32} << 30; }
    void end() {}
} SD;
int sdcard_init(int, decltype(SPI) *, int) { return 0xff; }
bool sd_read_raw(int, uint8_t *, int) { return false; }
void sdcard_uninit(int) {}

#include "../src/board/tab5/board_sd_tab5.cpp"
#include "../src/board/tab5/board_sd_format_tab5.cpp"

static std::vector<int> rates;
static int working_rate;
static bool safe_to_mount = true;
static bool invalidate_on_failure;
bool Tab5SD_MountNative(int rate) {
    rates.push_back(rate);
    if (rate == working_rate) return true;
    if (invalidate_on_failure) safe_to_mount = false;
    return false;
}
esp_err_t Tab5SD_NativeConfig(sdmmc_host_t &host, sdmmc_slot_config_t &) {
    host = {};
    host.get_real_freq = [](int, int *khz) { *khz = SDMMC_FREQ_DEFAULT; return ESP_OK; };
    return ESP_OK;
}
esp_err_t Tab5SD_InitSpiBus(sdmmc_host_t &, sdspi_device_config_t &) {
    assert(false && "native recovery must not send SPI commands");
    return ESP_ERR_INVALID_STATE;
}
bool Tab5SD_CanRetryMount() { return safe_to_mount; }
void Tab5SD_BlockMounts() { safe_to_mount = false; }
sdmmc_card_t *Tab5SD_Card() { return nullptr; }
bool Tab5SD_UnmountIdf() { return true; }

int main() {
    // Healthy FAT card mounts at 40 MHz with no extra card initialization.
    working_rate = 40000;
    assert(BoardSD_Init() && rates == std::vector<int>{40000});
    assert(!native_probes && !spi_starts);
    // A supported FAT card with a marginal high-speed link stays four-bit.
    working_rate = 20000;
    rates.clear();
    assert(BoardSD_Init() && (rates == std::vector<int>{40000, 20000}));
    assert(native_probes == 1 && native_releases == 1 && !spi_starts);
    // A responsive native card with unmountable media goes to recovery.
    working_rate = 0;
    rates.clear();
    assert(!BoardSD_Init() && (rates == std::vector<int>{40000, 20000}));
    assert(native_probes == 2 && native_releases == 2 && !spi_starts);
    BoardSdCardInfo info = {};
    assert(!BoardSD_InspectCard(&info) && info.bus_width == 4 && !strcmp(info.layout, "Unknown or blank"));
    assert(!write_calls && !mkfs_calls);
    working_rate = 20000;
    rates.clear();
    assert(!BoardSD_FormatCard(info.token) && mkfs_calls == 1);
    assert((rates == std::vector<int>{40000, 20000}) && !spi_starts);
    working_rate = 0;
    // An incomplete mount cleanup forbids probing or trying another driver.
    invalidate_on_failure = true;
    assert(!BoardSD_Init() && native_probes == 4 && !spi_starts);
    invalidate_on_failure = false;
    safe_to_mount = true;
    // Native commands identified this card, but a later negotiation failed.
    native_init_error = 1;
    assert(!BoardSD_Init() && !spi_starts);
    assert(BoardSD_InspectCard(&info) && !spi_starts);
    native_init_error = 0;
    // A warm SPI card cannot answer native commands and uses compatibility.
    native_response = false;
    rates.clear();
    assert(BoardSD_Init() && rates == std::vector<int>{40000});
    assert(native_probes == 7 && native_releases == 7 && spi_starts == 1);
}
