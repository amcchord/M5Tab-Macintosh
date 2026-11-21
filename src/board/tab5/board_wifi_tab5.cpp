/*
 * board_wifi_tab5.cpp - WiFi transport pin setup for M5Stack Tab5.
 *
 * The Tab5 routes WiFi 6 through an ESP32-C6 co-processor connected to the
 * P4 over SDIO2. The Arduino WiFi library needs these pins set explicitly
 * before any scan/begin call.
 */

#include "board_wifi.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <sdkconfig.h>

extern "C" {
#include <esp_hosted_os_abstraction.h>
}

// The pinned Hosted streaming driver grows its two SDIO receive buffers during
// bursts and allocates packet buffers from the same heap. TLS and radio traffic
// can exhaust SRAM during a download. The same memory hook serves both the
// packet pool and streaming buffers. P4 SDMMC supports external DMA.
static decltype(g_hosted_osi_funcs._h_malloc_align) originalAlignedAlloc;

static void *allocateRadioBuffer(size_t size, size_t alignment)
{
    constexpr size_t line = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
    if (size && size <= SIZE_MAX - (line - 1)) {
        size_t padded = (size + line - 1) & ~(line - 1);
        void *buffer = heap_caps_aligned_alloc(alignment < line ? line : alignment, padded,
                                               MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buffer) return buffer;
    }
    return originalAlignedAlloc(size, alignment);
}

/* Tab5 SDIO2 pins for ESP32-C6 communication. */
#define TAB5_WIFI_SDIO_CLK  GPIO_NUM_12
#define TAB5_WIFI_SDIO_CMD  GPIO_NUM_13
#define TAB5_WIFI_SDIO_D0   GPIO_NUM_11
#define TAB5_WIFI_SDIO_D1   GPIO_NUM_10
#define TAB5_WIFI_SDIO_D2   GPIO_NUM_9
#define TAB5_WIFI_SDIO_D3   GPIO_NUM_8
#define TAB5_WIFI_SDIO_RST  GPIO_NUM_15

extern "C" void BoardWifi_Prepare(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    originalAlignedAlloc = g_hosted_osi_funcs._h_malloc_align;
    g_hosted_osi_funcs._h_malloc_align = allocateRadioBuffer;
    WiFi.setPins(TAB5_WIFI_SDIO_CLK, TAB5_WIFI_SDIO_CMD, TAB5_WIFI_SDIO_D0,
                 TAB5_WIFI_SDIO_D1, TAB5_WIFI_SDIO_D2, TAB5_WIFI_SDIO_D3,
                 TAB5_WIFI_SDIO_RST);
}
