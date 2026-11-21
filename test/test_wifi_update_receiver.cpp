// Drive the production OTA state machine with fragmented HTTP reads and a
// byte-for-byte inactive flash slot. The SHA-256 implementation is real.
#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "common/host_sha256.h"

using String = std::string;
using esp_err_t = int;
using esp_http_client_handle_t = void *;
using esp_ota_handle_t = unsigned;
constexpr int ESP_OK = 0, ESP_ERR_HTTP_EAGAIN = 0x7007, FIONREAD = 1;
constexpr int ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_TYPE_APP = 2;
constexpr int ESP_PARTITION_SUBTYPE_DATA_OTA = 0, ESP_PARTITION_SUBTYPE_APP_OTA_0 = 16, ESP_PARTITION_SUBTYPE_APP_OTA_1 = 17;
constexpr int OTA_WITH_SEQUENTIAL_WRITES = 1, WL_CONNECTED = 3;
constexpr int ESP_IMAGE_HEADER_MAGIC = 0xe9, ESP_CHIP_ID_ESP32P4 = 18, ESP_APP_DESC_MAGIC_WORD = 0x1a2b3c4d;
#define CONFIG_ESP32P4_REV_MIN_FULL 100
struct esp_image_header_t { uint8_t magic, hash_appended; uint16_t chip_id, min_chip_rev_full, max_chip_rev_full; };
struct esp_image_segment_header_t { uint32_t load, size; };
struct esp_app_desc_t { uint32_t magic; uint8_t reserved[252]; };
struct esp_chip_info_t { unsigned revision; };
struct esp_partition_t { unsigned address, size; const char *label; };
struct esp_http_client_config_t { const char *url; int timeout_ms; bool disable_auto_redirect; };
static const esp_partition_t data{0xe000,0x2000,"otadata"}, a{0x10000,0x600000,"ota_0"}, b{0x610000,0x600000,"ota_1"};
static const esp_partition_t *boot = &a;
static std::vector<uint8_t> image, flash;
static uint32_t tick;
static unsigned calls, headers, aborted, begins, writes;
static size_t cursor;
static const char *mode;
static struct { template<typename... Args> void printf(const char *, Args...) {} } Serial;
static struct { int status() { return WL_CONNECTED; } } WiFi;
static uint32_t millis() { return tick; }
static void esp_chip_info(esp_chip_info_t *chip) { chip->revision = 100; }
static const esp_partition_t *esp_ota_get_running_partition() { return &a; }
static const esp_partition_t *esp_ota_get_boot_partition() { return boot; }
static const esp_partition_t *esp_partition_find_first(int type, int sub, const char *) {
    return type == ESP_PARTITION_TYPE_DATA ? &data : sub == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? &a : &b;
}
static int esp_ota_abort(esp_ota_handle_t) { ++aborted; return 0; }
static int esp_ota_begin(const esp_partition_t *target, int, esp_ota_handle_t *writer) {
    assert(target == &b); ++begins; flash.clear(); *writer = 1; return 0;
}
static int esp_ota_write(esp_ota_handle_t writer, const uint8_t *p, size_t n) {
    assert(writer && n && n<=4096); ++writes; flash.insert(flash.end(),p,p+n); return 0;
}
static int esp_ota_end(esp_ota_handle_t) { return 0; }
static int esp_partition_read(const esp_partition_t *p,size_t offset, uint8_t *out,size_t n) {
    assert(p == &b && offset+n <= flash.size());
    memcpy(out,flash.data()+offset,n);
    if (!strcmp(mode,"corrupt") && !offset) out[0] ^= 1;
    return 0;
}
static int esp_ota_set_boot_partition(const esp_partition_t *selected) { boot = selected; return 0; }
static esp_http_client_handle_t esp_http_client_init(esp_http_client_config_t *) { return &cursor; }
static void esp_http_client_cleanup(esp_http_client_handle_t) {}
static int esp_http_client_open(esp_http_client_handle_t,int) { return 0; }
static int64_t esp_http_client_fetch_headers(esp_http_client_handle_t) {
    if (!headers++ || !strcmp(mode,"no_headers")) return -ESP_ERR_HTTP_EAGAIN;
    return !strcmp(mode,"length") ? image.size()-1 : image.size();
}
static int esp_http_client_get_status_code(esp_http_client_handle_t) { return 200; }
static bool esp_http_client_is_chunked_response(esp_http_client_handle_t) { return false; }
static int esp_http_client_set_timeout_ms(esp_http_client_handle_t,int ms) { assert(ms>0 && ms<=250); return 0; }
static int esp_http_client_get_socket(esp_http_client_handle_t) { return 3; }
static int lwip_ioctl(int fd,int,int *avail) { assert(fd==3); *avail=(calls%7 == 1) ? 0 : 4096; return 0; }
static int esp_http_client_read(esp_http_client_handle_t,char *out,size_t wanted) {
    ++calls;
    if (!strcmp(mode,"idle") || calls%7 == 3) return -ESP_ERR_HTTP_EAGAIN;
    if (calls%7 == 4) { errno = EAGAIN; return -1; }
    if (!strcmp(mode,"eof") && cursor >= 4800) return 0;
    if (!strcmp(mode,"io") && cursor >= 4800) { errno = ECONNRESET; return -1; }
    size_t n = std::min({wanted, image.size()-cursor, size_t(!strcmp(mode,"trickle") ? 1 : calls%3 == 0 ? 173 : 831)});
    assert(n);
    memcpy(out, image.data()+cursor, n); cursor+=n; return n;
}
#ifndef __APPLE__
static size_t strlcpy(char *dst,const char *src,size_t n) { snprintf(dst,n,"%s",src); return strlen(src); }
#else
#define section(x) section("__DATA,__otatest")
#endif
#include "../src/basilisk/firmware_update.cpp"
#ifdef __APPLE__
#undef section
#endif

static void start(const char *test, size_t size = 9007) {
    mode=test; calls=headers=begins=writes=aborted=0; cursor=0; boot=&a; flash.clear();
    image.resize(size);
    for (size_t i=0; i<size; ++i) image[i]=i*17+3;
    esp_image_header_t header{ESP_IMAGE_HEADER_MAGIC,1,ESP_CHIP_ID_ESP32P4,0,0};
    uint32_t magic = ESP_APP_DESC_MAGIC_WORD;
    memcpy(image.data(),&header,sizeof(header));
    memcpy(image.data()+sizeof(header)+sizeof(esp_image_segment_header_t),&magic,sizeof(magic));
    memcpy(image.data()+DescriptorOffset,&board_tag,sizeof(board_tag));
    if (!strcmp(test,"incompatible")) image[DescriptorOffset]^=1;
    mbedtls_sha256_context h; unsigned char digest[32]; char hex[65];
    mbedtls_sha256_init(&h); mbedtls_sha256_starts(&h,0); mbedtls_sha256_update(&h,image.data(),size);
    mbedtls_sha256_finish(&h,digest); mbedtls_sha256_free(&h);
    for (int i=0;i<32;++i) snprintf(hex+i*2,3,"%02x",digest[i]);
    char args[160]; snprintf(args,sizeof(args),"%zu %s http://127.0.0.1:12345/firmware.bin",size,hex);
    assert(FirmwareUpdateStart(args) == "OK BOOT UPDATE START");
}
static void run(unsigned step_ms=10) {
    while (FirmwareUpdateBusy()) {
        const unsigned before=calls;
        FirmwareUpdatePoll();
        assert(calls-before <= 1); // return for USB status and cancellation after each read
        tick+=step_ms;
    }
}
int main() {
    for (size_t size : {size_t(9007), DescriptorOffset+sizeof(board_tag)}) {
        start("normal",size); run();
        assert(FirmwareUpdateReady() && boot==&b && flash==image && t.verified==size);
        assert(begins==1 && writes==(size+4095)/4096);
        assert(FirmwareUpdateCancel()=="OK BOOT UPDATE CANCEL" && boot==&a);
    }
    for (auto test : {"no_headers","length","eof","io","idle","trickle","corrupt","incompatible"}) {
        start(test); run(!strcmp(test,"trickle") ? 500 : !strcmp(test,"idle") ? 200 : 10);
        const char *expected = !strcmp(test,"no_headers") ? "header_timeout" :
            !strcmp(test,"length") ? "http_headers" : !strcmp(test,"eof") ? "short_read" : !strcmp(test,"io") ? "read_error" :
            !strcmp(test,"idle") ? "download_idle" : !strcmp(test,"trickle") ? "timeout" :
            !strcmp(test,"corrupt") ? "digest" : "wrong_image";
        assert(t.phase == Transfer::Failed && !strcmp(t.reason,expected) && boot==&a);
        if (!strcmp(test,"eof") || !strcmp(test,"io")) assert(aborted==1);
        if (!strcmp(test,"incompatible")) assert(!begins && flash.empty());
    }
    start("normal");
    while (!t.received) FirmwareUpdatePoll();
    assert(t.buffered && !begins && FirmwareUpdateCancel()=="OK BOOT UPDATE CANCEL");
    start("normal"); run(); assert(boot==&b && flash==image);
}
