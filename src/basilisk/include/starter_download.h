#pragma once
#include <stdint.h>

enum class StarterState { Idle, Working, Done, Failed, Cancelled };
struct StarterDownloadInfo {
    StarterState state;
    uint32_t completed;
    uint32_t total;
    char message[128];
    int32_t network_bytes_per_second = -1;  // unavailable outside a sampled download
};

bool StarterDownloadStart(bool system7);
StarterDownloadInfo StarterDownloadStatus();
void StarterDownloadCancel();
