// Read-only diagnostics served from the C6. The emulator and its SD card
// are independent of the HTTP task.
#include <inttypes.h>
#include <stdio.h>
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_hosted_coprocessor.h"
#include "interface.h"
#include "host_power_save.h"

#if !CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED || !CONFIG_ESP_SDIO_HOST_INTERFACE
#error "Tab5 diagnostics require the SDIO slave with network split"
#endif
#if CONFIG_ESP_HOSTED_COPROCESSOR_APP_MAIN
#error "The diagnostics application supplies app_main"
#endif

// Updated by ESP-Hosted when the P4 opens/closes the SDIO data path.
extern volatile uint8_t datapath;

static esp_err_t status(httpd_req_t *req)
{
    char body[200];
    int len = snprintf(body, sizeof(body),
            "{\"processor\":\"ESP32-C6\",\"hosted\":\"2.12.13\","
            "\"uptime_s\":%" PRId64 ",\"free_heap_bytes\":%u,"
            "\"p4_sdio_open\":%s,\"p4_power_save\":%s}\n",
            esp_timer_get_time() / 1000000,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
            datapath ? "true" : "false", is_host_power_saving() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, len);
}

static esp_err_t index_page(httpd_req_t *req)
{
    extern const char page[] asm("_binary_index_html_start");
    return httpd_resp_sendstr(req, page);
}

static esp_err_t transfer(httpd_req_t *req)
{
    uint8_t block[1024];
    for (size_t i = 0; i < sizeof(block); ++i) block[i] = (uint8_t)i;
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    for (int i = 0; i < 64; ++i) {
        esp_err_t err = httpd_resp_send_chunk(req, (const char *)block, sizeof(block));
        if (err != ESP_OK) return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_hosted_coprocessor_init());
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 62080;  // C6-owned TCP range 61440-65535
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_page},
        {.uri = "/status", .method = HTTP_GET, .handler = status},
        {.uri = "/transfer", .method = HTTP_GET, .handler = transfer}
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
}
