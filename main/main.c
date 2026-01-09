/**
 * BasiliskII ESP32-P4 Port - Main Entry Point
 * 
 * This is the ESP-IDF entry point that initializes the system
 * and starts the BasiliskII emulator.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

static const char *TAG = "BasiliskII";

// External C++ initialization function
extern void basilisk_main(void);

void app_main(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "BasiliskII Macintosh Emulator for ESP32-P4");
    ESP_LOGI(TAG, "===========================================");
    
    // Log memory information
    ESP_LOGI(TAG, "Free internal heap: %lu bytes", 
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Free PSRAM: %lu bytes", 
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "Total PSRAM size: %lu bytes", 
             (unsigned long)esp_psram_get_size());
    
    // Check if PSRAM is available
    if (esp_psram_get_size() == 0) {
        ESP_LOGE(TAG, "PSRAM not detected! This emulator requires PSRAM.");
        ESP_LOGE(TAG, "Please check hardware configuration.");
        return;
    }
    
    // Log PSRAM XIP status
#if CONFIG_SPIRAM_XIP_FROM_PSRAM
    ESP_LOGI(TAG, "PSRAM XIP: ENABLED (execute-in-place from PSRAM)");
#else
    ESP_LOGW(TAG, "PSRAM XIP: DISABLED (code running from flash only)");
#endif

#if CONFIG_SPIRAM_FETCH_INSTRUCTIONS
    ESP_LOGI(TAG, "PSRAM instruction fetch: ENABLED");
#endif

#if CONFIG_SPIRAM_RODATA
    ESP_LOGI(TAG, "PSRAM rodata: ENABLED");
#endif
    
    ESP_LOGI(TAG, "Starting BasiliskII emulator...");
    
    // Start the emulator (this function is implemented in main_esp32.cpp)
    basilisk_main();
    
    // If we get here, the emulator exited
    ESP_LOGI(TAG, "BasiliskII emulator exited.");
    
    // Keep the task alive
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
