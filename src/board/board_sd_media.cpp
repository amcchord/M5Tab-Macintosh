#include "board_sd.h"

#if defined(BOARD_M5STACK_TAB5)
#include "tab5/tab5_sd_idf.h"
#if TAB5_SD_IDF
#include <ff.h>
#include <diskio_sdmmc.h>
#endif
#define ARDUINO_SD_CARD SD
#elif defined(BOARD_WAVESHARE_P4_101)
#define ARDUINO_SD_CARD SD_MMC
#endif

#if defined(ARDUINO_SD_CARD)
// The two pinned Arduino classes store their FatFs drive in a protected member.
struct MountedArduinoSd : decltype(ARDUINO_SD_CARD) {
    static uint8_t drive() { return ARDUINO_SD_CARD.*(&MountedArduinoSd::_pdrv); }
};

extern "C" BoardSdMedia BoardSD_Media(void)
{
#if TAB5_SD_IDF
    if (const sdmmc_card_t *card = Tab5SD_Card()) {
        return {(uint32_t)card->csd.sector_size, (uint32_t)card->csd.capacity,
                ff_diskio_get_pdrv_card(card)};
    }
#endif
    return {(uint32_t)ARDUINO_SD_CARD.sectorSize(), (uint32_t)ARDUINO_SD_CARD.numSectors(),
            MountedArduinoSd::drive()};
}
#endif
