// Host adapters for the ESP/Arduino calls in starter_download.cpp.
// Files use real POSIX I/O. zlib implements the raw-DEFLATE streaming API.
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>
#include "../common/host_sha256.h"

using portMUX_TYPE = int;
constexpr int portMUX_INITIALIZER_UNLOCKED = 0;
inline void portENTER_CRITICAL(portMUX_TYPE *) {}
inline void portEXIT_CRITICAL(portMUX_TYPE *) {}
inline uint32_t host_ms;
inline void (*host_read_hook)();
inline uint32_t millis() { return host_ms; }
inline void (*host_delay_hook)();
inline int pdMS_TO_TICKS(int ms) { return ms; }
inline void vTaskDelay(int ticks) {
    host_ms += ticks;
    if (host_delay_hook) host_delay_hook();
}
inline void vTaskDelete(void *) {}
constexpr int MALLOC_CAP_8BIT = 1, MALLOC_CAP_SPIRAM = 2, pdPASS = 1;
constexpr size_t CONFIG_CACHE_L2_CACHE_LINE_SIZE = 128;
inline void *heap_caps_aligned_alloc(size_t alignment, size_t n, int) {
    void *buffer = nullptr;
    return posix_memalign(&buffer, alignment, n) == 0 ? buffer : nullptr;
}
inline int xTaskCreatePinnedToCore(void (*fn)(void *), const char *, int, void *arg, int, void *, int) {
    fn(arg);
    return pdPASS;
}

constexpr int TINFL_LZ_DICT_SIZE = 32768, TINFL_FLAG_HAS_MORE_INPUT = 2, TINFL_STATUS_DONE = 0;
inline std::string host_mode;
using tinfl_decompressor = z_stream;
using tinfl_status = int;
inline void tinfl_init(z_stream *s) {
    *s = {};
    assert(inflateInit2(s, -15) == Z_OK);
}
inline int tinfl_decompress(z_stream *s, unsigned char *src, size_t *n, unsigned char *base,
                            unsigned char *dest, size_t *out, int) {
    assert(dest == base + s->total_out % TINFL_LZ_DICT_SIZE);
    assert(*out == TINFL_LZ_DICT_SIZE - size_t(dest - base));
    if (host_mode == "fragmented") {
        *n = std::min(*n, size_t(379));
        *out = std::min(*out, size_t(247));
    }
    s->next_in = src;
    s->avail_in = *n;
    s->next_out = dest;
    s->avail_out = *out;
    const int rc = inflate(s, Z_NO_FLUSH);
    *n -= s->avail_in;
    *out -= s->avail_out;
    if (rc == Z_OK && (*n || *out)) return 1;
    inflateEnd(s);
    return rc == Z_STREAM_END ? 0 : -1;
}

using esp_err_t = int;
constexpr int ESP_OK = 0, HTTP_EVENT_ON_HEADER = 1;
constexpr int ESP_ERR_HTTP_CONNECT = 2, ESP_ERR_HTTP_WRITE_DATA = 3, ESP_ERR_HTTP_EAGAIN = 7;
inline uint64_t host_free = 1ull << 32;
inline int esp_vfs_fat_info(const char *, uint64_t *total, uint64_t *available) {
    *total = 1ull << 32;
    *available = host_free;
    return ESP_OK;
}
struct esp_http_client_event_t { int event_id; const char *header_key, *header_value; void *user_data; };
struct esp_http_client_config_t {
    const char *url;
    int timeout_ms, buffer_size;
    bool is_async, disable_auto_redirect;
    void *crt_bundle_attach;
    void *user_data;
    int (*event_handler)(esp_http_client_event_t *);
};
constexpr auto esp_crt_bundle_attach = nullptr;

inline std::vector<uint8_t> payload;
inline unsigned asset_offset = 12345;
inline unsigned host_attempt;
struct HostClient {
    esp_http_client_config_t cfg;
    unsigned first, last, pos;
};
using esp_http_client_handle_t = HostClient *;
inline HostClient *esp_http_client_init(esp_http_client_config_t *cfg) { ++host_attempt; return new HostClient{*cfg, 0, 0, 0}; }
inline int esp_http_client_set_header(HostClient *c, const char *key, const char *range) {
    assert(!strcmp(key, "Range"));
    assert(sscanf(range, "bytes=%u-%u", &c->first, &c->last) == 2);
    assert(c->first >= asset_offset && c->first <= c->last && c->last == asset_offset + payload.size() - 1);
    c->pos = c->first - asset_offset;
    printf("REQUEST %u %u\n", c->first, c->last);
    return ESP_OK;
}
inline int esp_http_client_open(HostClient *, int) {
    if (host_mode == "bad_cert" || (host_mode == "connect_once" && host_attempt == 1)) return ESP_ERR_HTTP_CONNECT;
    if (host_mode == "cancel_connect") return ESP_ERR_HTTP_EAGAIN;
    return ESP_OK;
}
inline int esp_http_client_get_and_clear_last_tls_error(HostClient *, int *, int *flags) {
    *flags = host_mode == "bad_cert" ? 4 : 0;
    return ESP_OK;
}
inline int esp_http_client_set_timeout_ms(HostClient *c, int timeout) {
    c->cfg.timeout_ms = timeout;
    return ESP_OK;
}
inline int64_t esp_http_client_fetch_headers(HostClient *c) {
    if (host_mode == "headers_once" && host_attempt == 1) return -1;
    if (host_mode == "cancel_headers") {
        vTaskDelay(c->cfg.timeout_ms);
        return -ESP_ERR_HTTP_EAGAIN;
    }
    std::string r = "bytes " + std::to_string(c->first) + "-" + std::to_string(c->last) +
        ((host_mode == "wrong_range" || (host_mode == "bad_resume" && host_attempt == 2)) ? "/1" : "/494083964");
    esp_http_client_event_t event{HTTP_EVENT_ON_HEADER, "content-range", r.c_str(), c->cfg.user_data};
    c->cfg.event_handler(&event);
    if (host_mode == "duplicate_range") c->cfg.event_handler(&event);
    return c->last - c->first + (host_mode == "wrong_length" ? 2 : 1);
}
inline int esp_http_client_get_status_code(HostClient *) { return host_mode == "http200" ? 200 : 206; }
void StarterDownloadCancel();
inline int esp_http_client_read(HostClient *c, char *buffer, size_t limit) {
    if (host_read_hook) {
        host_read_hook();
        host_ms += 100;
    }
    if (host_mode == "cancel_body" || (host_mode == "idle_once" && host_attempt == 1 && c->pos >= 4096))
        return -ESP_ERR_HTTP_EAGAIN;
    if (host_mode == "interrupt" && c->pos >= 4096) return -1;
    if ((host_mode == "recover" || host_mode == "bad_resume" || host_mode == "cancel_backoff" ||
         host_mode == "changed_file" || host_mode == "rate_recover") &&
        host_attempt == 1 && c->pos >= 40960) return 0;
    if (host_mode == "recover" && host_attempt == 2 && c->pos >= 49152) return -1;
    size_t n = std::min({limit, size_t(4096), payload.size() - c->pos});
    memcpy(buffer, payload.data() + c->pos, n);
    c->pos += n;
    if (host_mode == "cancel") StarterDownloadCancel();
    return n;
}
inline void esp_http_client_cleanup(HostClient *c) { delete c; }
