/* microSD mount and raw-media geometry for file I/O and USB Disk. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sector_size;
    uint32_t sectors;
    uint8_t drive;
} BoardSdMedia;

bool BoardSD_Init(void);
BoardSdMedia BoardSD_Media(void);

/** @brief Card capacity in MB, or 0 if not mounted. */
uint64_t BoardSD_CardSizeMB(void);

#ifdef __cplusplus
}

#include "board_config.h"
#if defined(BOARD_M5STACK_TAB5)
  #include <SD.h>
  #if defined(TAB5_SD_DMA) || defined(TAB5_SD_NATIVE) || defined(TAB5_SD_BENCHMARK)
    #define TAB5_SD_IDF 1
    fs::FS &BoardSD_Filesystem();
    #define SD_FS BoardSD_Filesystem()
  #else
    #define SD_FS SD
  #endif
#elif defined(BOARD_WAVESHARE_P4_101)
  #include <SD_MMC.h>
  #define SD_FS SD_MMC
#endif

#endif /* __cplusplus */
