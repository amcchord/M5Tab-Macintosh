// Pull one USB-authorized application into the other ESP32-P4 OTA partition.
#include "firmware_update.h"
#include "board_config.h"
#include <WiFi.h>
#include <algorithm>
#include <sdkconfig.h>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_chip_info.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <lwip/inet.h>
#include <errno.h>

namespace {
constexpr uint32_t SlotSize = 0x600000;
constexpr size_t DescriptorOffset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
struct TargetDescriptor {
    char board[40];
    uint16_t sdk_min_revision;
    uint16_t format;
};
#if defined(BOARD_M5STACK_TAB5)
#define M5TAB_OTA_BOARD "M5Tab-Macintosh/Tab5/1"
#elif defined(BOARD_WAVESHARE_P4_101)
#define M5TAB_OTA_BOARD "M5Tab-Macintosh/Waveshare-P4-10.1/1"
#endif
// pioarduino application image headers can report min revision 0 on both
// SDKs. Encode the silicon requirement from the SDK actually compiled.
const TargetDescriptor board_tag __attribute__((section(".rodata_custom_desc"), used)) =
    {M5TAB_OTA_BOARD, CONFIG_ESP32P4_REV_MIN_FULL, 1};

struct Transfer {
    enum Phase { Idle, Connect, Download, Verify, Ready, Failed } phase = Idle;
    const char *reason = "none";
    const esp_partition_t *target = nullptr;
    esp_http_client_handle_t http = nullptr;
    esp_ota_handle_t writer = 0;
    mbedtls_sha256_context hash = {};
    uint32_t size = 0, received = 0, verified = 0, started = 0;
    uint8_t expected[32] = {};
    char url[128] = {};
    // Allocated only during updates. Standard single-slot installations never use it.
    uint8_t *buffer = nullptr;
} t;
constexpr size_t Chunk = 4096;

const esp_partition_t *other_slot() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return nullptr;
    auto p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (!p || p->address != 0xe000 || p->size != 0x2000) return nullptr;
    auto a = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    auto b = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    if (!a || !b || a->address != 0x10000 || b->address != 0x610000 ||
        a->size != SlotSize || b->size != SlotSize) return nullptr;
    if (running->address == a->address) return b;
    return running->address == b->address ? a : nullptr;
}

void close_http() {
    if (t.http) {
        esp_http_client_cleanup(t.http);
        t.http = nullptr;
    }
}

void finish(Transfer::Phase phase, const char *why) {
    close_http();
    if (t.writer) esp_ota_abort(t.writer);
    t.writer = 0;
    mbedtls_sha256_free(&t.hash);
    free(t.buffer);
    t.buffer = nullptr;
    t.reason = why;
    t.phase = phase;
    Serial.printf("[UPDATE] %s (%lu bytes)\n", why, (unsigned long)t.received);
}

bool compatible(const uint8_t *data) {
    esp_image_header_t header;
    memcpy(&header, data, sizeof(header));
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32P4 ||
        !header.hash_appended || header.min_chip_rev_full > chip.revision ||
        (header.max_chip_rev_full && chip.revision > header.max_chip_rev_full)) return false;
    uint32_t magic;
    memcpy(&magic, data + sizeof(header) + sizeof(esp_image_segment_header_t), sizeof(magic));
    TargetDescriptor candidate;
    memcpy(&candidate, data + DescriptorOffset, sizeof(candidate));
    return magic == ESP_APP_DESC_MAGIC_WORD && candidate.format == board_tag.format &&
           !memcmp(candidate.board, board_tag.board, sizeof(candidate.board)) &&
           candidate.sdk_min_revision <= chip.revision &&
           (candidate.sdk_min_revision >= 300) == (chip.revision >= 300);
}

bool valid_url(const char *s) {
    // A specific LAN IPv4 endpoint and a fixed resource; no credentials,
    // redirects, DNS or URL reinterpretation by a second parser.
    char ip[16] = {};
    unsigned port = 0;
    int used = 0;
    if (sscanf(s, "http://%15[0-9.]:%5u/firmware.bin%n", ip, &port, &used) != 2 ||
        !used || s[used] || !port || port > 65535) return false;
    in_addr addr;
    return inet_pton(AF_INET, ip, &addr) == 1;
}

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

bool FirmwareUpdateHasDualSlots() { return other_slot() != nullptr; }
bool FirmwareUpdateBusy() { return t.phase == Transfer::Connect || t.phase == Transfer::Download || t.phase == Transfer::Verify; }
bool FirmwareUpdateReady() { return t.phase == Transfer::Ready; }

String FirmwareUpdateStatus() {
    static const char *states[] = {"idle", "connecting", "downloading", "verifying", "ready", "failed"};
    auto running = esp_ota_get_running_partition();
    auto boot = esp_ota_get_boot_partition();
    char result[192];
    snprintf(result, sizeof(result), "OK BOOT UPDATE STATUS state=%s layout=%s running=%s boot=%s bytes=%lu verified=%lu size=%lu result=%s",
             states[t.phase], FirmwareUpdateHasDualSlots() ? "ab" : "single", running ? running->label : "?",
             boot ? boot->label : "?", (unsigned long)t.received, (unsigned long)t.verified, (unsigned long)t.size, t.reason);
    return String(result);
}

String FirmwareUpdateCancel() {
    if (FirmwareUpdateReady()) {
        // Revoke the new boot selection before restart. The current slot
        // will pass the normal startup confirmation again when it boots.
        if (esp_ota_set_boot_partition(esp_ota_get_running_partition()) != ESP_OK)
            return "ERR BOOT update_cancel_failed";
    } else if (!FirmwareUpdateBusy()) {
        return "ERR BOOT no_update_pending";
    }
    finish(Transfer::Idle, "cancelled");
    return "OK BOOT UPDATE CANCEL";
}

String FirmwareUpdateStart(char *args) {
    if (FirmwareUpdateBusy()) return "ERR BOOT update_busy";
    auto target = other_slot();
    if (!target) return "ERR BOOT ota_layout_required";
    if (esp_ota_get_boot_partition() != esp_ota_get_running_partition()) return "ERR BOOT reboot_pending";
    if (WiFi.status() != WL_CONNECTED) return "ERR BOOT wifi_disconnected";
    char *end;
    errno = 0;
    unsigned long size = strtoul(args, &end, 10);
    if (*args < '0' || *args > '9' || *end != ' ' || errno || size < DescriptorOffset + sizeof(board_tag) || size > target->size)
        return "ERR BOOT invalid_size";
    char *digest = end + 1;
    char *url = strchr(digest, ' ');
    if (!url || url - digest != 64 || !valid_url(url + 1)) return "ERR BOOT invalid_update_request";
    uint8_t expected[32];
    for (unsigned i = 0; i < sizeof(expected); ++i) {
        int hi = nibble(digest[i * 2]), lo = nibble(digest[i * 2 + 1]);
        if (hi < 0 || lo < 0) return "ERR BOOT invalid_digest";
        expected[i] = (hi << 4) | lo;
    }
    t.buffer = (uint8_t *)malloc(Chunk);
    if (!t.buffer) return "ERR BOOT update_memory";
    t.target = target;
    t.size = size;
    t.received = t.verified = 0;
    t.started = millis();
    memcpy(t.expected, expected, sizeof(expected));
    strlcpy(t.url, url + 1, sizeof(t.url));
    mbedtls_sha256_init(&t.hash);
    t.reason = "none";
    t.phase = Transfer::Connect;
    return "OK BOOT UPDATE START";
}

void FirmwareUpdatePoll() {
    if (!FirmwareUpdateBusy()) return;
    if (millis() - t.started > 15u * 60u * 1000u) return finish(Transfer::Failed, "timeout");
    if (t.phase == Transfer::Connect) {
        esp_http_client_config_t config = {};
        config.url = t.url;
        config.timeout_ms = 3000;
        config.disable_auto_redirect = true;
        t.http = esp_http_client_init(&config);
        if (!t.http || esp_http_client_open(t.http, 0) != ESP_OK) return finish(Transfer::Failed, "connect");
        if (esp_http_client_fetch_headers(t.http) != t.size || esp_http_client_get_status_code(t.http) != 200 ||
            esp_http_client_is_chunked_response(t.http)) return finish(Transfer::Failed, "http_headers");
        t.phase = Transfer::Download;
    }
    if (t.phase == Transfer::Download) {
        size_t need = std::min<size_t>(Chunk, t.size - t.received);
        int n = esp_http_client_read_response(t.http, (char *)t.buffer, need);
        if (n != (int)need) return finish(Transfer::Failed, "short_read");
        if (!t.received) {
            if (!compatible(t.buffer)) return finish(Transfer::Failed, "wrong_image");
            if (esp_ota_begin(t.target, OTA_WITH_SEQUENTIAL_WRITES, &t.writer) != ESP_OK)
                return finish(Transfer::Failed, "ota_begin");
        }
        if (esp_ota_write(t.writer, t.buffer, need) != ESP_OK) return finish(Transfer::Failed, "ota_write");
        t.received += need;
        if (t.received < t.size) return;
        close_http();
        esp_err_t result = esp_ota_end(t.writer);
        t.writer = 0;
        if (result != ESP_OK) return finish(Transfer::Failed, "image_validation");
        if (mbedtls_sha256_starts(&t.hash, 0)) return finish(Transfer::Failed, "sha256");
        t.phase = Transfer::Verify;
    }
    size_t count = std::min<size_t>(Chunk, t.size - t.verified);
    if (esp_partition_read(t.target, t.verified, t.buffer, count) != ESP_OK ||
        mbedtls_sha256_update(&t.hash, t.buffer, count)) return finish(Transfer::Failed, "readback");
    t.verified += count;
    if (t.verified < t.size) return;
    uint8_t hash[32];
    if (mbedtls_sha256_finish(&t.hash, hash) || memcmp(t.expected, hash, sizeof(hash))) return finish(Transfer::Failed, "digest");
    if (esp_ota_set_boot_partition(t.target) != ESP_OK) return finish(Transfer::Failed, "boot_select");
    finish(Transfer::Ready, "verified");
}

// Arduino's early default confirmation happens before peripherals initialize.
extern "C" bool verifyRollbackLater() { return true; }
void FirmwareUpdateHealth(bool passed) {
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        const esp_err_t err = passed ? esp_ota_mark_app_valid_cancel_rollback() :
            esp_ota_mark_app_invalid_rollback_and_reboot();
        Serial.printf("[UPDATE] startup %s: %s\n", passed ? "verified" : "failed", esp_err_to_name(err));
    }
#endif
}
