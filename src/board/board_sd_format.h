// The caller must hold exclusive preboot ownership of the card for either
// operation, with all files closed and USB Disk and downloader idle.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t capacity_bytes;
    uint64_t volume_bytes;
    uint32_t cluster_bytes;
    uint32_t partition_lba;
    uint32_t clock_khz;
    uint8_t partition_type;
    uint8_t bus_width;
    bool format_supported;
    char layout[24];
    char driver[20];
    char identity[48];
    char token[33];
} BoardSdCardInfo;

// NULL means success. Errors point to static strings suitable for the UI.
// A successful inspection arms a single format confirmation for this card.
const char *BoardSD_InspectCard(BoardSdCardInfo *info);
const char *BoardSD_FormatCard(const char *confirmed_token);

#ifdef __cplusplus
}
#endif
