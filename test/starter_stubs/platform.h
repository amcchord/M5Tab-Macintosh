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
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
using mbedtls_sha256_context = CC_SHA256_CTX;
inline int mbedtls_sha256_starts(mbedtls_sha256_context *c, int) { return !CC_SHA256_Init(c); }
inline int mbedtls_sha256_update(mbedtls_sha256_context *c, const void *p, size_t n) { return !CC_SHA256_Update(c, p, n); }
inline int mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char *out) { return !CC_SHA256_Final(out, c); }
#else
#include <openssl/sha.h>
using mbedtls_sha256_context = SHA256_CTX;
inline int mbedtls_sha256_starts(mbedtls_sha256_context *c, int) { return !SHA256_Init(c); }
inline int mbedtls_sha256_update(mbedtls_sha256_context *c, const void *p, size_t n) { return !SHA256_Update(c, p, n); }
inline int mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char *out) { return !SHA256_Final(out, c); }
#endif
inline void mbedtls_sha256_init(mbedtls_sha256_context *) {}
inline void mbedtls_sha256_free(mbedtls_sha256_context *) {}

using portMUX_TYPE = int;
constexpr int portMUX_INITIALIZER_UNLOCKED = 0;
inline void portENTER_CRITICAL(portMUX_TYPE *) {}
inline void portEXIT_CRITICAL(portMUX_TYPE *) {}
inline void vTaskDelay(int) {}
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
    bool disable_auto_redirect;
    void *crt_bundle_attach;
    void *user_data;
    int (*event_handler)(esp_http_client_event_t *);
};
constexpr auto esp_crt_bundle_attach = nullptr;
inline std::string host_mode;
inline std::vector<uint8_t> payload;
inline unsigned asset_offset = 12345;
struct HostClient {
    esp_http_client_config_t cfg;
    unsigned first, last, pos;
};
using esp_http_client_handle_t = HostClient *;
inline HostClient *esp_http_client_init(esp_http_client_config_t *cfg) { return new HostClient{*cfg, 0, 0, 0}; }
inline int esp_http_client_set_header(HostClient *c, const char *key, const char *range) {
    assert(!strcmp(key, "Range"));
    assert(sscanf(range, "bytes=%u-%u", &c->first, &c->last) == 2);
    assert(c->first >= asset_offset && c->first <= c->last && c->last == asset_offset + payload.size() - 1);
    c->pos = c->first - asset_offset;
    printf("REQUEST %u %u\n", c->first, c->last);
    return ESP_OK;
}
inline int esp_http_client_open(HostClient *, int) { return ESP_OK; }
inline int64_t esp_http_client_fetch_headers(HostClient *c) {
    std::string r = "bytes " + std::to_string(c->first) + "-" + std::to_string(c->last) +
        (host_mode == "wrong_range" ? "/1" : "/494083964");
    esp_http_client_event_t event{HTTP_EVENT_ON_HEADER, "content-range", r.c_str(), c->cfg.user_data};
    c->cfg.event_handler(&event);
    return c->last - c->first + (host_mode == "wrong_length" ? 2 : 1);
}
inline int esp_http_client_get_status_code(HostClient *) { return host_mode == "http200" ? 200 : 206; }
void StarterDownloadCancel();
inline int esp_http_client_read(HostClient *c, char *buffer, size_t limit) {
    if (host_mode == "interrupt" && c->pos >= 4096) return -1;
    size_t n = std::min({limit, size_t(4096), payload.size() - c->pos});
    memcpy(buffer, payload.data() + c->pos, n);
    c->pos += n;
    if (host_mode == "cancel") StarterDownloadCancel();
    return n;
}
inline void esp_http_client_cleanup(HostClient *c) { delete c; }
