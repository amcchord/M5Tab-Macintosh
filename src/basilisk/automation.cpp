/*
 * automation.cpp - serial screenshot/input bridge for closed-loop testing
 *
 * Protocol commands (one ASCII line each):
 *   @B2 PING
 *   @B2 INFO
 *   @B2 SCREENSHOT
 *   @B2 MOUSE MOVE <x> <y>
 *   @B2 MOUSE REL <dx> <dy>
 *   @B2 MOUSE DOWN|UP <button>
 *   @B2 MOUSE CLICK <x> <y> [button]
 *   @B2 KEY DOWN|UP|TAP <ADB keycode>
 *   @B2 TYPE <base64-encoded US-ASCII>
 *   @B2 RELEASE_ALL
 *
 * Replies are also prefixed with "@B2 ". Small input commands stay on serial.
 * Screenshots prefer a tokenized HTTP endpoint when WiFi is connected, with a
 * CRC-checked serial pull protocol retained as a universal fallback.
 */

#include "sysdeps.h"
#include "automation.h"
#include "boot_gui.h"
#include "input.h"
#include "quickdraw_accel.h"
#include "video.h"

#include "board_config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

extern void CPUTrapProfileReset(void);
extern uint32 CPUTrapProfileRead(uint16 trap_index);
extern uint32 CPUTrapProfileReadLayer(uint8 selector);

#define AUTOMATION_PROTOCOL_VERSION 3
#define AUTOMATION_TASK_STACK_SIZE 8192
#define AUTOMATION_TASK_PRIORITY 3
#define AUTOMATION_TASK_CORE 0
#define AUTOMATION_NETWORK_TASK_STACK_SIZE 8192
#define AUTOMATION_NETWORK_TASK_PRIORITY 3
#define AUTOMATION_NETWORK_TASK_CORE 0
#define AUTOMATION_LINE_CAPACITY 1024
#define AUTOMATION_TYPE_KEY_HOLD_MS 10
#define AUTOMATION_TYPE_KEY_GAP_MS 15
/* ASCII encoding avoids XON/XOFF and other control bytes being interpreted by
 * host tty layers. Forty-eight payload bytes become a line under 160 bytes. */
#define AUTOMATION_SCREENSHOT_CHUNK_SIZE 48
#define AUTOMATION_SCREENSHOT_RECORD_CAPACITY 160
#define AUTOMATION_SCREENSHOT_BATCH_MAX 16
#define AUTOMATION_SERIAL_WRITE_RETRIES 5
#define AUTOMATION_SERIAL_WRITE_CHUNK_SIZE 32
#define AUTOMATION_HTTP_PORT 8052
#define AUTOMATION_HTTP_REQUEST_CAPACITY 160
#define AUTOMATION_HTTP_TIMEOUT_MS 30000
#define AUTOMATION_HTTP_WRITE_CHUNK_SIZE 1024
#define AUTOMATION_SERIAL_SCREENSHOT_LEASE_MS 120000
#define AUTOMATION_LZ_HASH_BITS 12
#define AUTOMATION_LZ_HASH_SIZE (1U << AUTOMATION_LZ_HASH_BITS)
#define AUTOMATION_LZ_MAX_MATCH 130

static TaskHandle_t automation_task_handle = NULL;
static TaskHandle_t automation_network_task_handle = NULL;
static volatile bool automation_task_running = false;
static volatile bool automation_network_task_running = false;
static uint8_t *s_screenshot_payload = NULL;
static size_t s_screenshot_payload_size = 0;
static uint32_t s_screenshot_id = 0;
static uint16_t s_screenshot_width = 0;
static uint16_t s_screenshot_height = 0;
static size_t s_screenshot_raw_size = 0;
static uint32_t s_screenshot_crc = 0;
static WiFiServer s_automation_http_server(AUTOMATION_HTTP_PORT);
static bool s_automation_http_started = false;
static char s_automation_http_token[9] = {};
static SemaphoreHandle_t s_screenshot_mutex = NULL;
static bool s_serial_screenshot_locked = false;
static uint32_t s_serial_screenshot_last_activity_ms = 0;
static portMUX_TYPE s_network_state_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_automation_wifi_status = WL_IDLE_STATUS;
static bool s_automation_wifi_configured = false;
static bool s_automation_wifi_auto_connect = false;
static char s_automation_wifi_ip[16] = "0.0.0.0";
static char s_automation_http_url[96] = {};
static volatile bool s_automation_wifi_has_ip = false;
static bool s_automation_wifi_events_registered = false;

extern "C" bool AutomationSerialCaptureActive(void)
{
    return s_serial_screenshot_locked;
}

struct AutomationKeyStroke {
    uint8_t code;
    bool shift;
};

static bool asciiToKeyStroke(uint8_t ch, AutomationKeyStroke *stroke)
{
    if (stroke == NULL) return false;
    stroke->shift = false;

    if (ch >= 'a' && ch <= 'z') {
        static const uint8_t letters[26] = {
            0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22,
            0x26, 0x28, 0x25, 0x2E, 0x2D, 0x1F, 0x23, 0x0C, 0x0F,
            0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06
        };
        stroke->code = letters[ch - 'a'];
        return true;
    }
    if (ch >= 'A' && ch <= 'Z') {
        if (!asciiToKeyStroke((uint8_t)(ch - 'A' + 'a'), stroke)) return false;
        stroke->shift = true;
        return true;
    }

    switch (ch) {
        case '1': stroke->code = 0x12; return true;
        case '2': stroke->code = 0x13; return true;
        case '3': stroke->code = 0x14; return true;
        case '4': stroke->code = 0x15; return true;
        case '5': stroke->code = 0x17; return true;
        case '6': stroke->code = 0x16; return true;
        case '7': stroke->code = 0x1A; return true;
        case '8': stroke->code = 0x1C; return true;
        case '9': stroke->code = 0x19; return true;
        case '0': stroke->code = 0x1D; return true;
        case '-': stroke->code = 0x1B; return true;
        case '=': stroke->code = 0x18; return true;
        case '[': stroke->code = 0x21; return true;
        case ']': stroke->code = 0x1E; return true;
        case '\\': stroke->code = 0x2A; return true;
        case ';': stroke->code = 0x29; return true;
        case '\'': stroke->code = 0x27; return true;
        case '`': stroke->code = 0x32; return true;
        case ',': stroke->code = 0x2B; return true;
        case '.': stroke->code = 0x2F; return true;
        case '/': stroke->code = 0x2C; return true;
        case ' ': stroke->code = 0x31; return true;
        case '\t': stroke->code = 0x30; return true;
        case '\r':
        case '\n': stroke->code = 0x24; return true;
        case '\b': stroke->code = 0x33; return true;

        case '!': stroke->code = 0x12; stroke->shift = true; return true;
        case '@': stroke->code = 0x13; stroke->shift = true; return true;
        case '#': stroke->code = 0x14; stroke->shift = true; return true;
        case '$': stroke->code = 0x15; stroke->shift = true; return true;
        case '%': stroke->code = 0x17; stroke->shift = true; return true;
        case '^': stroke->code = 0x16; stroke->shift = true; return true;
        case '&': stroke->code = 0x1A; stroke->shift = true; return true;
        case '*': stroke->code = 0x1C; stroke->shift = true; return true;
        case '(': stroke->code = 0x19; stroke->shift = true; return true;
        case ')': stroke->code = 0x1D; stroke->shift = true; return true;
        case '_': stroke->code = 0x1B; stroke->shift = true; return true;
        case '+': stroke->code = 0x18; stroke->shift = true; return true;
        case '{': stroke->code = 0x21; stroke->shift = true; return true;
        case '}': stroke->code = 0x1E; stroke->shift = true; return true;
        case '|': stroke->code = 0x2A; stroke->shift = true; return true;
        case ':': stroke->code = 0x29; stroke->shift = true; return true;
        case '"': stroke->code = 0x27; stroke->shift = true; return true;
        case '~': stroke->code = 0x32; stroke->shift = true; return true;
        case '<': stroke->code = 0x2B; stroke->shift = true; return true;
        case '>': stroke->code = 0x2F; stroke->shift = true; return true;
        case '?': stroke->code = 0x2C; stroke->shift = true; return true;
        default: return false;
    }
}

static void tapKey(const AutomationKeyStroke &stroke)
{
    if (stroke.shift) InputAutomationKey(0x38, true);
    InputAutomationKey(stroke.code, true);
    vTaskDelay(pdMS_TO_TICKS(AUTOMATION_TYPE_KEY_HOLD_MS));
    InputAutomationKey(stroke.code, false);
    if (stroke.shift) InputAutomationKey(0x38, false);
    vTaskDelay(pdMS_TO_TICKS(AUTOMATION_TYPE_KEY_GAP_MS));
}

static int base64Value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool decodeBase64(const char *src, uint8_t *dst, size_t capacity,
                         size_t *decoded_size)
{
    const size_t length = strlen(src);
    if ((length & 3U) != 0) return false;
    size_t out = 0;
    for (size_t offset = 0; offset < length; offset += 4) {
        const int a = base64Value(src[offset]);
        const int b = base64Value(src[offset + 1]);
        const bool pad_c = src[offset + 2] == '=';
        const bool pad_d = src[offset + 3] == '=';
        const int c = pad_c ? 0 : base64Value(src[offset + 2]);
        const int d = pad_d ? 0 : base64Value(src[offset + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (pad_c && !pad_d) ||
            ((pad_c || pad_d) && offset + 4 != length)) {
            return false;
        }
        const uint32_t value = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                               ((uint32_t)c << 6) | (uint32_t)d;
        const size_t bytes = pad_c ? 1 : (pad_d ? 2 : 3);
        if (out + bytes > capacity) return false;
        dst[out++] = (uint8_t)(value >> 16);
        if (!pad_c) dst[out++] = (uint8_t)(value >> 8);
        if (!pad_d) dst[out++] = (uint8_t)value;
    }
    *decoded_size = out;
    return true;
}

static uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t length)
{
    while (length-- > 0) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xedb88320U & mask);
        }
    }
    return crc;
}

static uint32_t automationLzHash(const uint8_t *data)
{
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    value ^= value >> 15;
    value *= 0x9E3779B1U;
    return value >> (32 - AUTOMATION_LZ_HASH_BITS);
}

static bool automationLzWriteLiterals(const uint8_t *input, size_t begin,
                                      size_t end, uint8_t *output,
                                      size_t capacity, size_t *written)
{
    while (begin < end) {
        const size_t remaining = end - begin;
        const size_t length = remaining < 127 ? remaining : 127;
        if (*written + 1 + length > capacity) return false;
        output[(*written)++] = (uint8_t)(length - 1);
        memcpy(output + *written, input + begin, length);
        *written += length;
        begin += length;
    }
    return true;
}

/* A compact LZ stream optimized for the emulator's indexed framebuffer.
 * Literal record: 0LLLLLLL (length = token + 1), followed by bytes.
 * Match record:   1LLLLLLL (length = low bits + 4), then uint16 distance.
 *
 * A 16 KB internal-RAM hash table keeps this linear and cache-friendly. The
 * ROM deflater's ~300 KB state lived in PSRAM and took around 15 seconds per
 * frame on the P4 despite producing a similar payload size. */
static size_t automationLzCompress(const uint8_t *input, size_t input_size,
                                   uint8_t *output, size_t output_capacity,
                                   uint32_t *hash_table)
{
    for (size_t i = 0; i < AUTOMATION_LZ_HASH_SIZE; ++i) {
        hash_table[i] = UINT32_MAX;
    }

    size_t position = 0;
    size_t literal_begin = 0;
    size_t written = 0;
    while (position + 4 <= input_size) {
        const uint32_t hash = automationLzHash(input + position);
        const uint32_t previous = hash_table[hash];
        hash_table[hash] = (uint32_t)position;

        size_t match_length = 0;
        size_t distance = 0;
        if (previous != UINT32_MAX && position > previous &&
            position - previous <= UINT16_MAX &&
            memcmp(input + previous, input + position, 4) == 0) {
            distance = position - previous;
            match_length = 4;
            const size_t maximum =
                (input_size - position) < AUTOMATION_LZ_MAX_MATCH
                    ? input_size - position
                    : AUTOMATION_LZ_MAX_MATCH;
            while (match_length < maximum &&
                   input[previous + match_length] ==
                       input[position + match_length]) {
                ++match_length;
            }
        }

        if (match_length < 4) {
            ++position;
            continue;
        }
        if (!automationLzWriteLiterals(input, literal_begin, position, output,
                                       output_capacity, &written) ||
            written + 3 > output_capacity) {
            return 0;
        }
        output[written++] = 0x80U | (uint8_t)(match_length - 4);
        output[written++] = (uint8_t)(distance & 0xff);
        output[written++] = (uint8_t)(distance >> 8);
        position += match_length;
        literal_begin = position;
    }
    if (!automationLzWriteLiterals(input, literal_begin, input_size, output,
                                   output_capacity, &written)) {
        return 0;
    }
    return written;
}

static bool writeScreenshotRecord(const uint8_t *data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const size_t remaining = size - offset;
        const size_t write_size =
            remaining < AUTOMATION_SERIAL_WRITE_CHUNK_SIZE
                ? remaining
                : AUTOMATION_SERIAL_WRITE_CHUNK_SIZE;
        bool completed = false;
        for (int attempt = 0; attempt < AUTOMATION_SERIAL_WRITE_RETRIES;
             ++attempt) {
            const size_t written = Serial.write(data + offset, write_size);
            Serial.flush();
            if (written != 0) {
                offset += written;
                completed = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!completed) return false;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    return true;
}

static void releaseScreenshot(void)
{
    if (s_screenshot_payload != NULL) free(s_screenshot_payload);
    s_screenshot_payload = NULL;
    s_screenshot_payload_size = 0;
    s_screenshot_id = 0;
    s_screenshot_width = 0;
    s_screenshot_height = 0;
    s_screenshot_raw_size = 0;
    s_screenshot_crc = 0;
}

static bool captureScreenshot(bool announce)
{
    releaseScreenshot();
    const size_t pixel_capacity =
        (size_t)BOARD_MAC_SCREEN_WIDTH * BOARD_MAC_SCREEN_HEIGHT;
    uint8_t *pixels = (uint8_t *)ps_malloc(pixel_capacity);
    const size_t compressed_capacity =
        pixel_capacity + (pixel_capacity + 126) / 127 + 16;
    uint8_t *compressed = (uint8_t *)ps_malloc(compressed_capacity);
    // Screenshot compression is off the emulator hot path. Keep its 16 KiB
    // transient dictionary in PSRAM so CPU dispatch can retain scarce SRAM.
    uint32_t *hash_table = (uint32_t *)heap_caps_malloc(
        AUTOMATION_LZ_HASH_SIZE * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (pixels == NULL || compressed == NULL || hash_table == NULL) {
        if (pixels != NULL) free(pixels);
        if (compressed != NULL) free(compressed);
        if (hash_table != NULL) heap_caps_free(hash_table);
        if (announce) Serial.println("@B2 ERR SCREENSHOT out_of_memory");
        return false;
    }

    uint16_t palette[256];
    uint16_t width = 0;
    uint16_t height = 0;
    if (!VideoCaptureFrame(pixels, pixel_capacity, palette, &width, &height)) {
        heap_caps_free(hash_table);
        free(compressed);
        free(pixels);
        if (announce) Serial.println("@B2 ERR SCREENSHOT frame_unavailable");
        return false;
    }

    const size_t raw_size = (size_t)width * height;
    const size_t compressed_size = automationLzCompress(
        pixels, raw_size, compressed, compressed_capacity, hash_table);
    heap_caps_free(hash_table);
    if (compressed_size == 0) {
        free(compressed);
        free(pixels);
        if (announce) Serial.println("@B2 ERR SCREENSHOT compression_failed");
        return false;
    }

    uint8_t palette_bytes[sizeof(palette)];
    for (size_t i = 0; i < 256; ++i) {
        palette_bytes[i * 2] = (uint8_t)(palette[i] & 0xff);
        palette_bytes[i * 2 + 1] = (uint8_t)(palette[i] >> 8);
    }

    const size_t payload_size = sizeof(palette_bytes) + compressed_size;
    const uint32_t frame_id = millis();
    uint8_t *payload = (uint8_t *)ps_malloc(payload_size);
    if (payload == NULL) {
        free(compressed);
        free(pixels);
        if (announce) Serial.println("@B2 ERR SCREENSHOT out_of_memory");
        return false;
    }
    memcpy(payload, palette_bytes, sizeof(palette_bytes));
    memcpy(payload + sizeof(palette_bytes), compressed, compressed_size);
    const uint32_t crc =
        crc32Update(0xffffffffU, payload, payload_size) ^ 0xffffffffU;
    free(compressed);
    free(pixels);

    s_screenshot_payload = payload;
    s_screenshot_payload_size = payload_size;
    s_screenshot_id = frame_id;
    s_screenshot_width = width;
    s_screenshot_height = height;
    s_screenshot_raw_size = raw_size;
    s_screenshot_crc = crc;

    const size_t chunk_count =
        (payload_size + AUTOMATION_SCREENSHOT_CHUNK_SIZE - 1) /
        AUTOMATION_SCREENSHOT_CHUNK_SIZE;
    /* Compact framing keeps the complete header in one 64-byte USB FIFO. The
     * pixel format is fixed by protocol version 1. */
    if (announce) {
        Serial.printf("@B2 F %lu %u %u %u %u %08lX %u L\n",
                      (unsigned long)frame_id, width, height,
                      (unsigned)payload_size, (unsigned)raw_size,
                      (unsigned long)crc, (unsigned)chunk_count);
    }
    return true;
}

static bool captureMonochromeScreenshot(void)
{
    releaseScreenshot();
    const size_t pixel_capacity =
        (size_t)BOARD_MAC_SCREEN_WIDTH * BOARD_MAC_SCREEN_HEIGHT;
    uint8_t *pixels = (uint8_t *)ps_malloc(pixel_capacity);
    if (pixels == NULL) {
        Serial.println("@B2 ERR SCREENSHOT out_of_memory");
        return false;
    }

    uint16_t palette[256];
    uint16_t width = 0;
    uint16_t height = 0;
    if (!VideoCaptureFrame(pixels, pixel_capacity, palette, &width, &height)) {
        free(pixels);
        Serial.println("@B2 ERR SCREENSHOT frame_unavailable");
        return false;
    }

    const size_t pixel_count = (size_t)width * height;
    const size_t packed_size = (pixel_count + 7) / 8;
    uint8_t *packed = (uint8_t *)ps_malloc(packed_size);
    const size_t compressed_capacity =
        packed_size + (packed_size + 126) / 127 + 16;
    uint8_t *compressed = (uint8_t *)ps_malloc(compressed_capacity);
    uint32_t *hash_table = (uint32_t *)heap_caps_malloc(
        AUTOMATION_LZ_HASH_SIZE * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (packed == NULL || compressed == NULL || hash_table == NULL) {
        if (packed != NULL) free(packed);
        if (compressed != NULL) free(compressed);
        if (hash_table != NULL) heap_caps_free(hash_table);
        free(pixels);
        Serial.println("@B2 ERR SCREENSHOT out_of_memory");
        return false;
    }

    bool palette_is_light[256];
    for (size_t i = 0; i < 256; ++i) {
        const uint16_t color = palette[i];
        const unsigned red = ((color >> 11) & 0x1f) * 255 / 31;
        const unsigned green = ((color >> 5) & 0x3f) * 255 / 63;
        const unsigned blue = (color & 0x1f) * 255 / 31;
        palette_is_light[i] =
            (77 * red + 150 * green + 29 * blue) >= (128U * 256U);
    }
    memset(packed, 0, packed_size);
    for (size_t i = 0; i < pixel_count; ++i) {
        if (palette_is_light[pixels[i]]) {
            packed[i >> 3] |= (uint8_t)(0x80U >> (i & 7));
        }
    }
    free(pixels);

    const size_t compressed_size = automationLzCompress(
        packed, packed_size, compressed, compressed_capacity, hash_table);
    heap_caps_free(hash_table);
    free(packed);
    if (compressed_size == 0) {
        free(compressed);
        Serial.println("@B2 ERR SCREENSHOT compression_failed");
        return false;
    }

    s_screenshot_payload = compressed;
    s_screenshot_payload_size = compressed_size;
    s_screenshot_id = millis();
    s_screenshot_width = width;
    s_screenshot_height = height;
    s_screenshot_raw_size = packed_size;
    s_screenshot_crc =
        crc32Update(0xffffffffU, compressed, compressed_size) ^ 0xffffffffU;
    return true;
}

static void announceTokenizedScreenshot(uint16_t request_token,
                                        bool monochrome)
{
    const size_t chunk_count =
        (s_screenshot_payload_size + AUTOMATION_SCREENSHOT_CHUNK_SIZE - 1) /
        AUTOMATION_SCREENSHOT_CHUNK_SIZE;
    Serial.printf("@B2 %s %04X %lu %u %u %u %u %08lX %u L\n",
                  monochrome ? "M2" : "F2",
                  request_token, (unsigned long)s_screenshot_id,
                  s_screenshot_width, s_screenshot_height,
                  (unsigned)s_screenshot_payload_size,
                  (unsigned)s_screenshot_raw_size,
                  (unsigned long)s_screenshot_crc, (unsigned)chunk_count);
}

static bool writeHttpBody(WiFiClient &client, const uint8_t *data, size_t size)
{
    size_t offset = 0;
    const uint32_t deadline = millis() + AUTOMATION_HTTP_TIMEOUT_MS;
    while (offset < size && client.connected() &&
           (int32_t)(millis() - deadline) < 0) {
        const size_t remaining = size - offset;
        const size_t chunk = remaining < AUTOMATION_HTTP_WRITE_CHUNK_SIZE
                                 ? remaining
                                 : AUTOMATION_HTTP_WRITE_CHUNK_SIZE;
        const size_t written = client.write(data + offset, chunk);
        if (written > 0) {
            offset += written;
            vTaskDelay(1);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    return offset == size;
}

static void updateNetworkState(int status, const char *ip, const char *url)
{
    portENTER_CRITICAL(&s_network_state_lock);
    s_automation_wifi_status = status;
    snprintf(s_automation_wifi_ip, sizeof(s_automation_wifi_ip), "%s",
             ip != NULL ? ip : "0.0.0.0");
    snprintf(s_automation_http_url, sizeof(s_automation_http_url), "%s",
             url != NULL ? url : "");
    portEXIT_CRITICAL(&s_network_state_lock);
}

static void getNetworkState(int *status, char *ip, size_t ip_capacity,
                            char *url, size_t url_capacity)
{
    portENTER_CRITICAL(&s_network_state_lock);
    if (status != NULL) *status = s_automation_wifi_status;
    if (ip != NULL && ip_capacity > 0) {
        snprintf(ip, ip_capacity, "%s", s_automation_wifi_ip);
    }
    if (url != NULL && url_capacity > 0) {
        snprintf(url, url_capacity, "%s", s_automation_http_url);
    }
    portEXIT_CRITICAL(&s_network_state_lock);
}

static void automationWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
            const IPAddress address(info.got_ip.ip_info.ip.addr);
            const String ip = address.toString();
            s_automation_wifi_has_ip = true;
            updateNetworkState(WL_CONNECTED, ip.c_str(), "");
            break;
        }
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
        case ARDUINO_EVENT_WIFI_STA_STOP:
            s_automation_wifi_has_ip = false;
            updateNetworkState(WL_DISCONNECTED, "0.0.0.0", "");
            break;
        default:
            break;
    }
}

static void pollAutomationHttp(void)
{
    if (!s_automation_wifi_has_ip) return;
    int wifi_status = WL_IDLE_STATUS;
    char ip[16];
    getNetworkState(&wifi_status, ip, sizeof(ip), NULL, 0);
    char http_url[96];
    snprintf(http_url, sizeof(http_url), "http://%s:%u/frame/%s",
             ip, AUTOMATION_HTTP_PORT, s_automation_http_token);
    if (!s_automation_http_started) {
        s_automation_http_server.begin();
        s_automation_http_server.setNoDelay(true);
        s_automation_http_started = true;
        Serial.printf("[AUTOMATION] HTTP framebuffer ready at %s\n", http_url);
    }
    updateNetworkState(wifi_status, ip, http_url);

    WiFiClient client = s_automation_http_server.accept();
    if (!client) return;
    client.setTimeout(AUTOMATION_HTTP_TIMEOUT_MS);

    char request[AUTOMATION_HTTP_REQUEST_CAPACITY] = {};
    size_t length = 0;
    const uint32_t deadline = millis() + AUTOMATION_HTTP_TIMEOUT_MS;
    while (length + 1 < sizeof(request) &&
           (int32_t)(millis() - deadline) < 0) {
        if (client.available() <= 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        const int raw = client.read();
        if (raw < 0) continue;
        if (raw == '\n') break;
        if (raw != '\r') request[length++] = (char)raw;
    }

    char expected_path[32];
    snprintf(expected_path, sizeof(expected_path), "GET /frame/%s ",
             s_automation_http_token);
    if (strncmp(request, expected_path, strlen(expected_path)) != 0) {
        client.print("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                     "Connection: close\r\n\r\n");
        client.stop();
        return;
    }

    if (s_screenshot_mutex == NULL ||
        xSemaphoreTake(s_screenshot_mutex, 0) != pdTRUE) {
        client.print("HTTP/1.1 409 Conflict\r\nContent-Length: 0\r\n"
                     "Connection: close\r\n\r\n");
        client.stop();
        return;
    }

    if (!captureScreenshot(false)) {
        client.print("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                     "Connection: close\r\n\r\n");
        client.stop();
        xSemaphoreGive(s_screenshot_mutex);
        return;
    }

    client.printf(
        "HTTP/1.1 200 OK\r\nContent-Type: application/x-b2-indexed-frame\r\n"
        "Content-Length: %u\r\nX-B2-Width: %u\r\nX-B2-Height: %u\r\n"
        "X-B2-Raw-Size: %u\r\nX-B2-CRC32: %08lX\r\n"
        "X-B2-Encoding: b2lz\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        (unsigned)s_screenshot_payload_size, s_screenshot_width,
        s_screenshot_height, (unsigned)s_screenshot_raw_size,
        (unsigned long)s_screenshot_crc);
    (void)writeHttpBody(client, s_screenshot_payload,
                        s_screenshot_payload_size);
    client.stop();
    releaseScreenshot();
    xSemaphoreGive(s_screenshot_mutex);
}

static void sendScreenshotChunk(uint32_t frame_id, size_t chunk)
{
    if (s_screenshot_payload == NULL || frame_id != s_screenshot_id) {
        Serial.println("@B2 ERR SCREENSHOT invalid_frame");
        return;
    }
    const size_t chunk_count =
        (s_screenshot_payload_size + AUTOMATION_SCREENSHOT_CHUNK_SIZE - 1) /
        AUTOMATION_SCREENSHOT_CHUNK_SIZE;
    if (chunk >= chunk_count) {
        Serial.println("@B2 ERR SCREENSHOT invalid_chunk");
        return;
    }

    const size_t offset = chunk * AUTOMATION_SCREENSHOT_CHUNK_SIZE;
    const size_t remaining = s_screenshot_payload_size - offset;
    const size_t chunk_size = remaining < AUTOMATION_SCREENSHOT_CHUNK_SIZE
                                  ? remaining
                                  : AUTOMATION_SCREENSHOT_CHUNK_SIZE;
    if (chunk > UINT16_MAX) {
        Serial.println("@B2 ERR SCREENSHOT invalid_chunk");
        return;
    }
    static const char hex[] = "0123456789ABCDEF";
    char record[AUTOMATION_SCREENSHOT_RECORD_CAPACITY];
    int record_size = snprintf(record, sizeof(record), "@B2 D %lu %u ",
                               (unsigned long)frame_id, (unsigned)chunk);
    if (record_size < 0 ||
        (size_t)record_size + chunk_size * 2 + 10 > sizeof(record)) {
        Serial.println("@B2 ERR SCREENSHOT record_overflow");
        return;
    }
    for (size_t i = 0; i < chunk_size; ++i) {
        const uint8_t value = s_screenshot_payload[offset + i];
        record[record_size++] = hex[value >> 4];
        record[record_size++] = hex[value & 0x0f];
    }
    const uint32_t record_crc =
        crc32Update(0xffffffffU, s_screenshot_payload + offset, chunk_size) ^
        0xffffffffU;
    record_size += snprintf(record + record_size,
                            sizeof(record) - (size_t)record_size,
                            " %08lX\n", (unsigned long)record_crc);
    if (record_size <= 0 ||
        !writeScreenshotRecord((const uint8_t *)record, (size_t)record_size)) {
        Serial.println("@B2 ERR SCREENSHOT data_write_failed");
    }
}

static bool parseKeyCode(const char *text, uint8_t *code)
{
    if (text == NULL || code == NULL) return false;
    char *end = NULL;
    const long value = strtol(text, &end, 0);
    if (end == text || *end != '\0' || value < 0 || value > 0x7f) return false;
    *code = (uint8_t)value;
    return true;
}

static void processCommand(char *command)
{
    if (strcmp(command, "PING") == 0) {
        Serial.printf("@B2 OK PONG %d\n", AUTOMATION_PROTOCOL_VERSION);
        return;
    }
    if (strcmp(command, "INFO") == 0) {
        Serial.printf("@B2 OK INFO %d %s %d %d\n", AUTOMATION_PROTOCOL_VERSION,
                      BOARD_NAME, BOARD_MAC_SCREEN_WIDTH, BOARD_MAC_SCREEN_HEIGHT);
        return;
    }
    if (strcmp(command, "HTTP") == 0) {
        char url[96];
        getNetworkState(NULL, NULL, 0, url, sizeof(url));
        if (url[0] == '\0') {
            Serial.println("@B2 ERR HTTP unavailable");
        } else {
            Serial.printf("@B2 OK HTTP %s\n", url);
        }
        return;
    }
    if (strcmp(command, "NET") == 0) {
        int wifi_status = WL_IDLE_STATUS;
        char ip[16];
        char url[96];
        getNetworkState(&wifi_status, ip, sizeof(ip), url, sizeof(url));
        Serial.printf("@B2 OK NET status=%d ip=%s configured=%d auto=%d http=%d\n",
                      wifi_status, ip, s_automation_wifi_configured ? 1 : 0,
                      s_automation_wifi_auto_connect ? 1 : 0,
                      url[0] != '\0' ? 1 : 0);
        return;
    }
    bool tokenized_screenshot = false;
    bool monochrome_screenshot = false;
    unsigned long screenshot_request_token = 0;
    const char *screenshot_token_text = NULL;
    if (strncmp(command, "SCREENSHOT MONO ", 16) == 0) {
        screenshot_token_text = command + 16;
        monochrome_screenshot = true;
    } else if (strncmp(command, "SCREENSHOT ", 11) == 0) {
        screenshot_token_text = command + 11;
    }
    if (screenshot_token_text != NULL) {
        char *end = NULL;
        screenshot_request_token = strtoul(screenshot_token_text, &end, 16);
        tokenized_screenshot =
            end != screenshot_token_text && *end == '\0' &&
            screenshot_request_token <= UINT16_MAX;
    }
    if (strcmp(command, "SCREENSHOT") == 0 || tokenized_screenshot) {
        bool acquired_screenshot_lease = false;
        if (!s_serial_screenshot_locked) {
            if (s_screenshot_mutex == NULL ||
                xSemaphoreTake(s_screenshot_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
                Serial.println("@B2 ERR SCREENSHOT busy");
                return;
            }
            s_serial_screenshot_locked = true;
            acquired_screenshot_lease = true;
        }
        if (acquired_screenshot_lease) {
            // Let a panel DMA transfer that began just before the lease finish.
            // The video task observes AutomationSerialCaptureActive() and will
            // not start another transfer until SCREENSHOT CLOSE.
            vTaskDelay(pdMS_TO_TICKS(120));
        }
        const bool captured = monochrome_screenshot
                                  ? captureMonochromeScreenshot()
                                  : captureScreenshot(!tokenized_screenshot);
        if (!captured) {
            s_serial_screenshot_locked = false;
            xSemaphoreGive(s_screenshot_mutex);
        } else {
            s_serial_screenshot_last_activity_ms = millis();
            if (tokenized_screenshot) {
                announceTokenizedScreenshot(
                    (uint16_t)screenshot_request_token,
                    monochrome_screenshot);
            }
        }
        return;
    }
    unsigned long screenshot_frame = 0;
    unsigned screenshot_chunk = 0;
    unsigned screenshot_batch_count = 0;
    if (sscanf(command, "SCREENSHOT BATCH %lu %u %u",
               &screenshot_frame, &screenshot_chunk,
               &screenshot_batch_count) == 3) {
        if (s_screenshot_payload == NULL ||
            (uint32_t)screenshot_frame != s_screenshot_id) {
            Serial.println("@B2 ERR SCREENSHOT invalid_frame");
            return;
        }
        const size_t chunk_count =
            (s_screenshot_payload_size + AUTOMATION_SCREENSHOT_CHUNK_SIZE - 1) /
            AUTOMATION_SCREENSHOT_CHUNK_SIZE;
        if (screenshot_batch_count == 0 ||
            screenshot_batch_count > AUTOMATION_SCREENSHOT_BATCH_MAX ||
            screenshot_chunk >= chunk_count ||
            screenshot_batch_count > chunk_count - screenshot_chunk) {
            Serial.println("@B2 ERR SCREENSHOT invalid_batch");
            return;
        }
        s_serial_screenshot_last_activity_ms = millis();
        for (unsigned offset = 0; offset < screenshot_batch_count; ++offset) {
            sendScreenshotChunk((uint32_t)screenshot_frame,
                                screenshot_chunk + offset);
            /* P4 HWCDC consistently loses every other long write under this
             * workload. Duplicate each CRC/sequence record; the host accepts
             * the intact copy and de-duplicates it without ambiguity. */
            sendScreenshotChunk((uint32_t)screenshot_frame,
                                screenshot_chunk + offset);
        }
        Serial.printf("@B2 OK SCREENSHOT BATCH %lu %u %u\n",
                      screenshot_frame, screenshot_chunk,
                      screenshot_batch_count);
        return;
    }
    if (sscanf(command, "SCREENSHOT CHUNK %lu %u",
               &screenshot_frame, &screenshot_chunk) == 2) {
        s_serial_screenshot_last_activity_ms = millis();
        sendScreenshotChunk((uint32_t)screenshot_frame, screenshot_chunk);
        sendScreenshotChunk((uint32_t)screenshot_frame, screenshot_chunk);
        return;
    }
    if (sscanf(command, "SCREENSHOT CLOSE %lu", &screenshot_frame) == 1) {
        if (s_screenshot_payload == NULL ||
            (uint32_t)screenshot_frame != s_screenshot_id) {
            Serial.println("@B2 ERR SCREENSHOT invalid_frame");
        } else {
            releaseScreenshot();
            Serial.printf("@B2 OK SCREENSHOT CLOSE %lu\n", screenshot_frame);
            if (s_serial_screenshot_locked) {
                s_serial_screenshot_locked = false;
                s_serial_screenshot_last_activity_ms = 0;
                xSemaphoreGive(s_screenshot_mutex);
            }
        }
        return;
    }
    if (strcmp(command, "RELEASE_ALL") == 0) {
        InputAutomationReleaseAll();
        Serial.println("@B2 OK RELEASE_ALL");
        return;
    }
    if (strcmp(command, "TRAPS RESET") == 0) {
        CPUTrapProfileReset();
        QuickDrawAccelResetStats();
        Serial.println("@B2 OK TRAPS RESET");
        return;
    }
    if (strcmp(command, "QDACCEL") == 0) {
        QuickDrawAccelStats stats = {};
        QuickDrawAccelReadStats(&stats);
        Serial.printf("@B2 OK QDACCEL ca=%lu ch=%lu carg=%lu cbm=%lu cport=%lu cexec=%lu "
                      "sa=%lu sh=%lu sarg=%lu sport=%lu srgn=%lu sbg=%lu sexec=%lu "
                      "sha=%lu shh=%lu shf=%lu "
                      "ma=%lu mh=%lu mf=%lu la=%lu lh=%lu lf=%lu "
                      "pa=%lu prec=%lu pbm=%lu prect=%lu pvis=%lu pclip=%lu "
                      "vsz=%lu csz=%lu pic=%08lX rgn=%08lX poly=%08lX gp=%08lX "
                      "copy_bytes=%llu scroll_bytes=%llu shape_bytes=%llu line_bytes=%llu\n",
                      (unsigned long)stats.copy_attempts,
                      (unsigned long)stats.copy_calls,
                      (unsigned long)stats.copy_arg_failures,
                      (unsigned long)stats.copy_bitmap_failures,
                      (unsigned long)stats.copy_port_failures,
                      (unsigned long)stats.copy_execution_failures,
                      (unsigned long)stats.scroll_attempts,
                      (unsigned long)stats.scroll_calls,
                      (unsigned long)stats.scroll_arg_failures,
                      (unsigned long)stats.scroll_port_failures,
                      (unsigned long)stats.scroll_region_failures,
                      (unsigned long)stats.scroll_background_failures,
                      (unsigned long)stats.scroll_execution_failures,
                      (unsigned long)stats.shape_attempts,
                      (unsigned long)stats.shape_calls,
                      (unsigned long)stats.shape_failures,
                      (unsigned long)stats.move_attempts,
                      (unsigned long)stats.move_calls,
                      (unsigned long)stats.move_failures,
                      (unsigned long)stats.line_attempts,
                      (unsigned long)stats.line_calls,
                      (unsigned long)stats.line_failures,
                      (unsigned long)stats.port_address_failures,
                      (unsigned long)stats.port_recording_failures,
                      (unsigned long)stats.port_bitmap_failures,
                      (unsigned long)stats.port_rect_failures,
                      (unsigned long)stats.port_visible_region_failures,
                      (unsigned long)stats.port_clip_region_failures,
                      (unsigned long)stats.last_vis_region_size,
                      (unsigned long)stats.last_clip_region_size,
                      (unsigned long)stats.last_pic_save,
                      (unsigned long)stats.last_rgn_save,
                      (unsigned long)stats.last_poly_save,
                      (unsigned long)stats.last_graf_procs,
                      (unsigned long long)stats.copy_bytes,
                      (unsigned long long)stats.scroll_bytes,
                      (unsigned long long)stats.shape_bytes,
                      (unsigned long long)stats.line_bytes);
        return;
    }
    if (strcmp(command, "QDREGION") == 0) {
        uint16 words[64] = {};
        const uint32 count = QuickDrawAccelReadRegionSnapshot(words, 64);
        char response[384];
        size_t used = (size_t)snprintf(response, sizeof(response),
                                      "@B2 OK QDREGION %lu",
                                      (unsigned long)count);
        for (uint32 index = 0; index < count && used + 6 < sizeof(response); ++index) {
            const int written = snprintf(response + used, sizeof(response) - used,
                                         " %04X", words[index]);
            if (written < 0 || (size_t)written >= sizeof(response) - used) break;
            used += (size_t)written;
        }
        if (used + 1 < sizeof(response)) response[used++] = '\n';
        Serial.write((const uint8_t *)response, used);
        return;
    }
    int layer_profile_offset = 0;
    if (strcmp(command, "LAYER") == 0 ||
        sscanf(command, "LAYER %d", &layer_profile_offset) == 1) {
        if (layer_profile_offset < 0 || layer_profile_offset >= 16) {
            Serial.println("@B2 ERR LAYER invalid_offset");
            return;
        }
        uint8_t top_selector[16] = {};
        uint32_t top_count[16] = {};
        for (uint16_t selector = 0; selector < 256; ++selector) {
            const uint32_t count = CPUTrapProfileReadLayer((uint8_t)selector);
            if (count <= top_count[15]) continue;
            int position = 15;
            while (position > 0 && count > top_count[position - 1]) {
                top_count[position] = top_count[position - 1];
                top_selector[position] = top_selector[position - 1];
                --position;
            }
            top_count[position] = count;
            top_selector[position] = (uint8_t)selector;
        }
        char response[96];
        size_t used = (size_t)snprintf(response, sizeof(response),
                                      "@B2 OK LAYER %d", layer_profile_offset);
        const int profile_end = layer_profile_offset + 3 < 16
                                    ? layer_profile_offset + 3 : 16;
        for (int position = layer_profile_offset;
             position < profile_end && top_count[position]; ++position) {
            if (used >= sizeof(response)) break;
            const int written = snprintf(response + used, sizeof(response) - used,
                                         " %02X:%lu", top_selector[position],
                                         (unsigned long)top_count[position]);
            if (written < 0 || (size_t)written >= sizeof(response) - used) break;
            used += (size_t)written;
        }
        if (used + 1 < sizeof(response)) response[used++] = '\n';
        Serial.write((const uint8_t *)response, used);
        return;
    }
    int trap_profile_offset = 0;
    if (strcmp(command, "TRAPS") == 0 ||
        sscanf(command, "TRAPS %d", &trap_profile_offset) == 1) {
        if (trap_profile_offset < 0 || trap_profile_offset >= 16) {
            Serial.println("@B2 ERR TRAPS invalid_offset");
            return;
        }
        uint16_t top_index[16] = {};
        uint32_t top_count[16] = {};
        for (uint16_t index = 0; index < 4096; ++index) {
            const uint32_t count = CPUTrapProfileRead(index);
            if (count <= top_count[15]) continue;
            int position = 15;
            while (position > 0 && count > top_count[position - 1]) {
                top_count[position] = top_count[position - 1];
                top_index[position] = top_index[position - 1];
                --position;
            }
            top_count[position] = count;
            top_index[position] = index;
        }
        char response[96];
        size_t used = (size_t)snprintf(response, sizeof(response),
                                      "@B2 OK TRAPS %d", trap_profile_offset);
        const int profile_end = trap_profile_offset + 3 < 16
                                    ? trap_profile_offset + 3 : 16;
        for (int position = trap_profile_offset;
             position < profile_end && top_count[position]; ++position) {
            if (used >= sizeof(response)) break;
            const int written = snprintf(response + used, sizeof(response) - used,
                                         " A%03X:%lu", top_index[position],
                                         (unsigned long)top_count[position]);
            if (written < 0 || (size_t)written >= sizeof(response) - used) break;
            used += (size_t)written;
        }
        if (used + 1 < sizeof(response)) response[used++] = '\n';
        Serial.write((const uint8_t *)response, used);
        return;
    }

    int x = 0;
    int y = 0;
    int button = 0;
    if (sscanf(command, "MOUSE MOVE %d %d", &x, &y) == 2) {
        InputAutomationMouseMove(x, y, false);
        Serial.printf("@B2 OK MOUSE MOVE %d %d\n", x, y);
        return;
    }
    if (sscanf(command, "MOUSE REL %d %d", &x, &y) == 2) {
        InputAutomationMouseMove(x, y, true);
        Serial.printf("@B2 OK MOUSE REL %d %d\n", x, y);
        return;
    }
    int click_fields = sscanf(command, "MOUSE CLICK %d %d %d", &x, &y, &button);
    if (click_fields == 2 || click_fields == 3) {
        if (button < 0 || button > 2) {
            Serial.println("@B2 ERR MOUSE invalid_button");
            return;
        }
        InputAutomationMouseMove(x, y, false);
        vTaskDelay(pdMS_TO_TICKS(15));
        InputAutomationMouseButton((uint8_t)button, true);
        vTaskDelay(pdMS_TO_TICKS(25));
        InputAutomationMouseButton((uint8_t)button, false);
        Serial.printf("@B2 OK MOUSE CLICK %d %d %d\n", x, y, button);
        return;
    }
    if (sscanf(command, "MOUSE DOWN %d", &button) == 1 ||
        sscanf(command, "MOUSE UP %d", &button) == 1) {
        if (button < 0 || button > 2) {
            Serial.println("@B2 ERR MOUSE invalid_button");
            return;
        }
        const bool pressed = strncmp(command, "MOUSE DOWN", 10) == 0;
        InputAutomationMouseButton((uint8_t)button, pressed);
        Serial.printf("@B2 OK MOUSE %s %d\n", pressed ? "DOWN" : "UP", button);
        return;
    }

    if (strncmp(command, "KEY ", 4) == 0) {
        char action[8] = {};
        char code_text[16] = {};
        if (sscanf(command + 4, "%7s %15s", action, code_text) != 2) {
            Serial.println("@B2 ERR KEY invalid_arguments");
            return;
        }
        uint8_t code = 0;
        if (!parseKeyCode(code_text, &code)) {
            Serial.println("@B2 ERR KEY invalid_keycode");
            return;
        }
        if (strcmp(action, "DOWN") == 0) {
            InputAutomationKey(code, true);
        } else if (strcmp(action, "UP") == 0) {
            InputAutomationKey(code, false);
        } else if (strcmp(action, "TAP") == 0) {
            AutomationKeyStroke stroke = {code, false};
            tapKey(stroke);
        } else {
            Serial.println("@B2 ERR KEY invalid_action");
            return;
        }
        Serial.printf("@B2 OK KEY %s 0x%02X\n", action, code);
        return;
    }

    if (strncmp(command, "TYPE ", 5) == 0) {
        const char *encoded = command + 5;
        const size_t encoded_length = strlen(encoded);
        const size_t decoded_capacity = (encoded_length * 3) / 4 + 3;
        uint8_t *decoded = (uint8_t *)malloc(decoded_capacity);
        if (decoded == NULL) {
            Serial.println("@B2 ERR TYPE out_of_memory");
            return;
        }
        size_t decoded_size = 0;
        if (!decodeBase64(encoded, decoded, decoded_capacity, &decoded_size)) {
            free(decoded);
            Serial.println("@B2 ERR TYPE invalid_base64");
            return;
        }
        for (size_t i = 0; i < decoded_size; ++i) {
            AutomationKeyStroke stroke;
            if (!asciiToKeyStroke(decoded[i], &stroke)) {
                const unsigned bad = decoded[i];
                free(decoded);
                InputAutomationReleaseAll();
                Serial.printf("@B2 ERR TYPE unsupported_byte_%02X\n", bad);
                return;
            }
            tapKey(stroke);
        }
        free(decoded);
        Serial.printf("@B2 OK TYPE %u\n", (unsigned)decoded_size);
        return;
    }

    Serial.println("@B2 ERR unknown_command");
}

static void automationTask(void *param)
{
    (void)param;
    static char line[AUTOMATION_LINE_CAPACITY];
    size_t length = 0;
    bool overflow = false;

    Serial.printf("@B2 READY %d\n", AUTOMATION_PROTOCOL_VERSION);
    while (automation_task_running) {
        while (Serial.available() > 0) {
            const int raw = Serial.read();
            if (raw < 0) break;
            const char ch = (char)raw;
            if (ch == '\n') {
                if (overflow) {
                    Serial.println("@B2 ERR line_too_long");
                } else {
                    if (length > 0 && line[length - 1] == '\r') --length;
                    line[length] = '\0';
                    if (strncmp(line, "@B2 ", 4) == 0) {
                        processCommand(line + 4);
                    }
                }
                length = 0;
                overflow = false;
            } else if (!overflow) {
                if (length + 1 < sizeof(line)) {
                    line[length++] = ch;
                } else {
                    overflow = true;
                }
            }
        }
        if (s_serial_screenshot_locked &&
            (uint32_t)(millis() - s_serial_screenshot_last_activity_ms) >=
                AUTOMATION_SERIAL_SCREENSHOT_LEASE_MS) {
            // A dead/disconnected serial client must not block the WiFi path
            // forever. Packet pulls refresh this lease; a new frame can be
            // requested safely after it expires.
            releaseScreenshot();
            s_serial_screenshot_locked = false;
            s_serial_screenshot_last_activity_ms = 0;
            xSemaphoreGive(s_screenshot_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    InputAutomationReleaseAll();
    if (s_serial_screenshot_locked) {
        releaseScreenshot();
        s_serial_screenshot_locked = false;
        s_serial_screenshot_last_activity_ms = 0;
        xSemaphoreGive(s_screenshot_mutex);
    }
    automation_task_handle = NULL;
    vTaskDelete(NULL);
}

static void automationNetworkTask(void *param)
{
    (void)param;
    while (automation_network_task_running) {
        pollAutomationHttp();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    updateNetworkState(WL_DISCONNECTED, "0.0.0.0", "");
    automation_network_task_handle = NULL;
    vTaskDelete(NULL);
}

extern "C" void AutomationPrepareNetwork(void)
{
    if (s_automation_wifi_events_registered) return;
    WiFi.onEvent(automationWiFiEvent);
    s_automation_wifi_events_registered = true;
}

extern "C" bool AutomationInit(void)
{
    if (automation_task_handle != NULL) return true;
#if defined(BOARD_M5STACK_TAB5)
    /* HWCDC defaults to a 100 ms TX-lock/ring-buffer timeout. A screenshot
     * line can otherwise be dropped when a diagnostic log briefly owns the
     * serial writer, which the host correctly reports as a missing sequence.
     * The UART-backed Waveshare Serial class has no corresponding method. */
    Serial.setTxTimeoutMs(2000);
#endif
    snprintf(s_automation_http_token, sizeof(s_automation_http_token),
             "%08lX", (unsigned long)esp_random());
    AutomationPrepareNetwork();
    s_automation_wifi_configured = BootGUI_GetWiFiSSID()[0] != '\0';
    s_automation_wifi_auto_connect = BootGUI_GetWiFiAutoConnect();
    s_screenshot_mutex = xSemaphoreCreateMutex();
    if (s_screenshot_mutex == NULL) {
        Serial.println("[AUTOMATION] ERROR: Failed to create screenshot lock");
        return false;
    }
    automation_task_running = true;
    const BaseType_t result = xTaskCreatePinnedToCore(
        automationTask, "Automation", AUTOMATION_TASK_STACK_SIZE, NULL,
        AUTOMATION_TASK_PRIORITY, &automation_task_handle, AUTOMATION_TASK_CORE);
    if (result != pdPASS) {
        automation_task_running = false;
        automation_task_handle = NULL;
        vSemaphoreDelete(s_screenshot_mutex);
        s_screenshot_mutex = NULL;
        Serial.println("[AUTOMATION] ERROR: Failed to start serial control task");
        return false;
    }
    automation_network_task_running = true;
    const BaseType_t network_result = xTaskCreatePinnedToCore(
        automationNetworkTask, "AutomationNet", AUTOMATION_NETWORK_TASK_STACK_SIZE,
        NULL, AUTOMATION_NETWORK_TASK_PRIORITY, &automation_network_task_handle,
        AUTOMATION_NETWORK_TASK_CORE);
    if (network_result != pdPASS) {
        automation_network_task_running = false;
        automation_network_task_handle = NULL;
        Serial.println("[AUTOMATION] WARNING: HTTP framebuffer task unavailable");
    }
    Serial.println("[AUTOMATION] Closed-loop control enabled (@B2 serial + HTTP)");
    return true;
}

extern "C" void AutomationExit(void)
{
    automation_task_running = false;
    automation_network_task_running = false;
    for (int i = 0; i < 100 && automation_task_handle != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    for (int i = 0; i < 100 && automation_network_task_handle != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    InputAutomationReleaseAll();
    if (s_screenshot_mutex != NULL && automation_network_task_handle == NULL &&
        automation_task_handle == NULL) {
        vSemaphoreDelete(s_screenshot_mutex);
        s_screenshot_mutex = NULL;
    }
}
