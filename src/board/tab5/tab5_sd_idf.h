#pragma once

#include "board_sd.h"

#if TAB5_SD_IDF
#include <sdmmc_cmd.h>

sdmmc_card_t *Tab5SD_Card();
bool Tab5SD_UnmountIdf();
bool Tab5SD_MountDma();
bool Tab5SD_MountNative();
#endif

#if TAB5_SD_BENCHMARK
bool Tab5SD_MountArduino();
#endif
