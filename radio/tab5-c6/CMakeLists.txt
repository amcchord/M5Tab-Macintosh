cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET esp32c6)

# The slave/main component uses sources from ../../common in its own checkout.
get_filename_component(hosted "$ENV{ESP_HOSTED_DIR}" ABSOLUTE)
execute_process(COMMAND git -C "${hosted}" rev-parse HEAD
                OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT revision STREQUAL "dd0176e5fc959d79b306f6677a2f878123c58e9a")
    message(FATAL_ERROR "Set ESP_HOSTED_DIR to the ESP-Hosted v2.12.13 checkout. See README.md.")
endif()

set(EXTRA_COMPONENT_DIRS "${hosted}/slave/main")
file(WRITE "${CMAKE_BINARY_DIR}/radio-partitions.defaults"
    "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${hosted}/slave/partitions.esp32c6.csv\"\n")
set(SDKCONFIG_DEFAULTS "${hosted}/slave/sdkconfig.defaults;${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.tab5;${CMAKE_BINARY_DIR}/radio-partitions.defaults")
set(COMPONENTS main esp_timer esptool_py bootloader nvs_flash esp_rom esp_wifi
    protocomm driver mqtt wpa_supplicant wifi_provisioning esp_driver_sdio
    esp_pm openthread app_update esp_http_server)
set(PROJECT_VER "2.12.13")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_c6_diagnostics)

idf_component_get_property(radio_lib main COMPONENT_LIB)
target_sources(${radio_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics.c")
target_add_binary_data(${PROJECT_NAME}.elf "${CMAKE_CURRENT_SOURCE_DIR}/index.html" TEXT)
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_compile_options(${lwip_lib} PRIVATE -include "${hosted}/common/esp_hosted_lwip_src_port_hook.h")
