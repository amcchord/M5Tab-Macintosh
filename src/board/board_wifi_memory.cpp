#include "board_wifi.h"
#include <esp_heap_caps.h>
#include <sdkconfig.h>

extern "C" {
#include <esp_hosted_os_abstraction.h>
#include <mbedtls/platform.h>
#include <esp_mem.h>
}

// Both P4 boards use Hosted SDIO for Wi-Fi. Its streaming and packet buffers
// share the aligned allocator. SRAM alone cannot hold a large receive burst
// alongside TLS. SDMMC DMA can access cache-line aligned PSRAM on the P4.
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

// The SDK's TLS allocator prefers internal memory. Two TLS record buffers
// can leave too little SRAM for the SD driver's next sector buffer.
static void *allocateTlsMemory(size_t count, size_t size)
{
    if (size && count <= SIZE_MAX / size && count * size >= 1024) {
        void *buffer = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buffer) return buffer;
    }
    return esp_mbedtls_mem_calloc(count, size);
}

extern "C" void BoardWifi_PrepareDmaMemory(void)
{
    if (originalAlignedAlloc) return;
    originalAlignedAlloc = g_hosted_osi_funcs._h_malloc_align;
    g_hosted_osi_funcs._h_malloc_align = allocateRadioBuffer;
    mbedtls_platform_set_calloc_free(allocateTlsMemory, esp_mbedtls_mem_free);
}
