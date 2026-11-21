// Install only the selected members of the project's existing starter ZIP.
// The byte ranges and both digests pin the compressed and installed bytes.
#include "starter_download.h"
#include "starter_speed.h"
#include "board_config.h"
#include <Arduino.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_vfs_fat.h>
#include <mbedtls/sha256.h>
#include <miniz.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>

namespace {
constexpr char archiveUrl[] = "https://www.mcchord.net/static/sdCard.zip";
constexpr char stageDir[] = BOARD_SD_MOUNT_POINT "/.m5-starter-v1";
constexpr size_t chunk = 32 * 1024;
struct Asset {
    const char *name;
    uint32_t offset, compressed, size;
    const char *compressedHash, *hash;
};
constexpr Asset rom = {"Q650.ROM",
                       493591308,
                       490974,
                       1048576,
                       "cc72342554afdc9918cde3a224b0376f02e663c0f74afcfec8c4eb7a8da1f2d3",
                       "05ad753fb594e656cf078023ec189e09e2a7655a780de993b75b8c51ed6b09ca"};
constexpr Asset os7 = {"Macintosh7.dsk",
                       378048183,
                       115542761,
                       1048576000,
                       "feae46aa28a2a90d8ffc64979aa4b75f04e67d8908c80bce5b2288f0107cb70e",
                       "37aca18d2ddeadd29851c7fd49b73475da37b3578514066fcf9b745840fdc2f5"};
constexpr Asset os8 = {"Macintosh8.dsk",
                       175043602,
                       203003478,
                       1048576000,
                       "a04e49ed856e74a2a2a769f5a8aa0f19e58b545bab8be0730d838ffe4474a075",
                       "4de7a954ca15d6c8f4257097e5bb39983ab03afae228c03781dee327d55efcee"};

portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
StarterDownloadInfo info = {};
StarterNetworkSpeed network_speed;
bool interrupted = false;
void progress(const char *action, const Asset &asset, uint32_t completed, uint32_t total) {
    char line[sizeof(info.message)];
    snprintf(line, sizeof(line), "%s %s", action, asset.name);
    portENTER_CRITICAL(&lock);
    snprintf(info.message, sizeof(info.message), "%s", line);
    info.completed = completed;
    info.total = total;
    portEXIT_CRITICAL(&lock);
}
bool cancelled() { return __atomic_load_n(&interrupted, __ATOMIC_ACQUIRE); }

struct FileHandle {
    int fd;
    explicit FileHandle(int value) : fd(value) {}
    ~FileHandle() {
        if (fd >= 0)
            close(fd);
    }
    bool syncClose() {
        int result = fsync(fd), closed = close(fd);
        fd = -1;
        return result == 0 && closed == 0;
    }
};

bool writeAll(int fd, const unsigned char *data, size_t n) {
    while (n) {
        ssize_t wrote = write(fd, data, n);
        if (wrote <= 0)
            return false;
        data += wrote;
        n -= wrote;
    }
    return true;
}

struct Scratch {
    // P4 SDMMC splits misaligned PSRAM I/O into individual 512-byte
    // transfers. Both streaming buffers start on an external cache line.
    alignas(CONFIG_CACHE_L2_CACHE_LINE_SIZE) unsigned char input[chunk];
    alignas(CONFIG_CACHE_L2_CACHE_LINE_SIZE) unsigned char output[TINFL_LZ_DICT_SIZE];
    tinfl_decompressor inflater;

    static Scratch *allocate() {
        return static_cast<Scratch *>(heap_caps_aligned_alloc(
            alignof(Scratch), sizeof(Scratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
};

enum class Digest { Match, Different, Error };
Digest checkFile(const char *path, const char *expected, uint32_t size, Scratch &scratch, const Asset &a) {
    FileHandle file(open(path, O_RDONLY));
    struct stat st;
    if (file.fd < 0 || fstat(file.fd, &st))
        return Digest::Error;
    if (st.st_size != size)
        return Digest::Different;
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    int rc = mbedtls_sha256_starts(&hash, 0);
    uint32_t pos = 0;
    while (!rc && pos < size && !cancelled()) {
        ssize_t n = read(file.fd, scratch.input, std::min<uint32_t>(chunk, size - pos));
        if (n <= 0) {
            rc = -1;
            break;
        }
        rc = mbedtls_sha256_update(&hash, scratch.input, n);
        pos += n;
        progress("Verifying", a, pos, size);
        if (!(pos % (1024 * 1024)))
            vTaskDelay(1);
    }
    unsigned char digest[32];
    if (!rc)
        rc = mbedtls_sha256_finish(&hash, digest);
    mbedtls_sha256_free(&hash);
    if (rc || cancelled())
        return Digest::Error;
    char hex[65];
    for (size_t i = 0; i < 32; ++i)
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return !strcmp(hex, expected) ? Digest::Match : Digest::Different;
}

struct Range {
    char expected[80];
    bool seen, matched;
};
esp_err_t onHeader(esp_http_client_event_t *event) {
    if (event->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key, "Content-Range")) {
        Range *range = static_cast<Range *>(event->user_data);
        range->matched = !range->seen && !strcmp(event->header_value, range->expected);
        range->seen = true;
    }
    return ESP_OK;
}

constexpr uint32_t networkTimeoutMs = 15000;
struct DownloadResult {
    const char *error = nullptr;
    bool retry = false;
};
constexpr DownloadResult connectionLost = {"Download interrupted. Reconnect Wi-Fi and retry to resume.", true};

struct HttpClient {
    esp_http_client_handle_t handle;
    ~HttpClient() {
        if (handle)
            esp_http_client_cleanup(handle);
    }
};

DownloadResult receiveFile(esp_http_client_handle_t client, const Asset &a, const char *path,
                           uint32_t &pos, Scratch &scratch) {
    FileHandle out(open(path, O_WRONLY | O_CREAT | O_APPEND, 0644));
    if (out.fd < 0 || lseek(out.fd, 0, SEEK_END) != pos)
        return {"Could not resume the download on this card."};
    uint32_t checkpoint = pos, lastByte = millis();
    portENTER_CRITICAL(&lock);
    network_speed.start(lastByte);
    portEXIT_CRITICAL(&lock);
    DownloadResult result;
    // Drain available TLS bytes without waiting to fill 32 KiB inside a blocking read.
    // Buffer short reads for an aligned card write, including the last bytes before a disconnect.
    esp_http_client_set_timeout_ms(client, 0);
    while (pos < a.compressed && !cancelled()) {
        size_t filled = 0, wanted = std::min<uint32_t>(chunk, a.compressed - pos);
        while (filled < wanted && !cancelled()) {
            int n = esp_http_client_read(client, (char *)scratch.input + filled, wanted - filled);
            uint32_t now = millis();
            if (n == -ESP_ERR_HTTP_EAGAIN && now - lastByte < networkTimeoutMs) {
                vTaskDelay(1);
                continue;
            }
            if (n <= 0) {
                result = connectionLost;
                break;
            }
            filled += n;
            lastByte = now;
            portENTER_CRITICAL(&lock);
            network_speed.receivedBytes(n, now);
            portEXIT_CRITICAL(&lock);
        }
        if (filled && !writeAll(out.fd, scratch.input, filled)) {
            result = {"Could not write the download. Check card space."};
            break;
        }
        pos += filled;
        progress("Downloading", a, pos, a.compressed);
        if (pos - checkpoint >= 2 * 1024 * 1024) {
            if (fsync(out.fd)) {
                result = {"Could not save download progress to the card."};
                break;
            }
            checkpoint = pos;
        }
        if (result.error)
            break;
    }
    portENTER_CRITICAL(&lock);
    network_speed.stop();
    portEXIT_CRITICAL(&lock);
    if (!out.syncClose())
        return {"Could not finish writing the download."};
    return result;
}

DownloadResult downloadAttempt(const Asset &a, const char *path, uint32_t &pos, Scratch &scratch) {
    Range range = {};
    char header[72];
    uint32_t first = a.offset + pos, last = a.offset + a.compressed - 1;
    snprintf(header, sizeof(header), "bytes=%lu-%lu", (unsigned long)first, (unsigned long)last);
    snprintf(range.expected, sizeof(range.expected), "bytes %lu-%lu/494083964", (unsigned long)first,
             (unsigned long)last);
    esp_http_client_config_t cfg = {};
    cfg.url = archiveUrl;
    cfg.is_async = true;
    cfg.timeout_ms = 200;
    cfg.disable_auto_redirect = true;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.user_data = &range;
    cfg.event_handler = onHeader;
    cfg.buffer_size = 4096;
    HttpClient client{esp_http_client_init(&cfg)};
    if (!client.handle || esp_http_client_set_header(client.handle, "Range", header) != ESP_OK)
        return {"Could not prepare the download connection."};
    progress("Connecting to download", a, pos, a.compressed);
    uint32_t started = millis();
    esp_err_t opened;
    do {
        if (cancelled())
            return {};
        opened = esp_http_client_open(client.handle, 0);
        if (opened == ESP_ERR_HTTP_EAGAIN)
            vTaskDelay(1);
    } while (opened == ESP_ERR_HTTP_EAGAIN && millis() - started < networkTimeoutMs);
    if (opened != ESP_OK) {
        int tlsFlags = 0;
        esp_http_client_get_and_clear_last_tls_error(client.handle, nullptr, &tlsFlags);
        if (tlsFlags)
            return {"The starter server's TLS certificate could not be verified."};
        if (opened == ESP_ERR_HTTP_EAGAIN || opened == ESP_ERR_HTTP_CONNECT ||
            opened == ESP_ERR_HTTP_WRITE_DATA)
            return connectionLost;
        return {"Could not open the download connection."};
    }

    started = millis();
    int64_t length;
    do {
        if (cancelled())
            return {};
        length = esp_http_client_fetch_headers(client.handle);
        if (length == -ESP_ERR_HTTP_EAGAIN)
            vTaskDelay(1);
    } while (length == -ESP_ERR_HTTP_EAGAIN && millis() - started < networkTimeoutMs);
    if (length < 0)
        return connectionLost;
    if (esp_http_client_get_status_code(client.handle) != 206 || !range.matched ||
        length != a.compressed - pos)
        return {"The starter archive changed or the server declined the requested range."};
    if (cancelled())
        return {};
    progress("Downloading", a, pos, a.compressed);
    return receiveFile(client.handle, a, path, pos, scratch);
}

const char *download(const Asset &a, const char *path, uint32_t pos, Scratch &scratch) {
    constexpr unsigned maxRetries = 3;
    for (unsigned retries = 0; pos < a.compressed && !cancelled(); ++retries) {
        DownloadResult result = downloadAttempt(a, path, pos, scratch);
        if (!result.retry || retries == maxRetries || cancelled())
            return result.error;
        char action[64];
        snprintf(action, sizeof(action), "Wi-Fi interrupted. Retrying (%u/%u)", retries + 1, maxRetries);
        progress(action, a, pos, a.compressed);
        const uint32_t start = millis(), delayMs = 1000u << retries;
        while (!cancelled() && millis() - start < delayMs)
            vTaskDelay(pdMS_TO_TICKS(50));
    }
    return nullptr;
}

const char *expand(const Asset &a, const char *packed, const char *temp, Scratch &s) {
    FileHandle in(open(packed, O_RDONLY)), out(open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0644));
    if (in.fd < 0 || out.fd < 0)
        return "Could not open the installation files.";
    tinfl_init(&s.inflater);
    uint32_t loaded = 0, decoded = 0;
    size_t avail = 0, at = 0, ring = 0;
    progress("Installing", a, 0, a.size);
    while (!cancelled()) {
        if (!avail && loaded < a.compressed) {
            ssize_t n = read(in.fd, s.input, std::min<uint32_t>(chunk, a.compressed - loaded));
            if (n <= 0)
                return "Could not read the staged download.";
            loaded += n;
            avail = n;
            at = 0;
        }
        size_t consumed = avail, produced = TINFL_LZ_DICT_SIZE - ring;
        tinfl_status status =
            tinfl_decompress(&s.inflater, s.input + at, &consumed, s.output, s.output + ring, &produced,
                             loaded < a.compressed ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        if (status < 0 || produced > a.size - decoded ||
            (!produced && !consumed && status != TINFL_STATUS_DONE))
            return "Invalid compressed starter file.";
        at += consumed;
        avail -= consumed;
        decoded += produced;
        ring += produced;
        const bool done = status == TINFL_STATUS_DONE;
        if (done && (loaded != a.compressed || avail || decoded != a.size))
            return "Starter file size does not match.";
        // Keep partial output in the inflater's existing dictionary until
        // it can be written as one aligned card transfer.
        if (ring == sizeof(s.output) || (done && ring)) {
            if (!writeAll(out.fd, s.output, ring))
                return "Could not write the disk image. Check card space.";
            ring = 0;
            progress("Installing", a, decoded, a.size);
            if (!(decoded % (1024 * 1024)))
                vTaskDelay(1);
        }
        if (done)
            return out.syncClose() ? nullptr : "Could not finish writing the disk image.";
    }
    return nullptr;
}

const char *install(const Asset &a, Scratch &s) {
    char target[80], packed[96], temp[96];
    snprintf(target, sizeof(target), "%s/%s", BOARD_SD_MOUNT_POINT, a.name);
    snprintf(packed, sizeof(packed), "%s/%s.deflate", stageDir, a.name);
    snprintf(temp, sizeof(temp), "%s/%s.new", stageDir, a.name);
    struct stat st;
    if (!stat(target, &st)) {
        Digest result = checkFile(target, a.hash, a.size, s, a);
        if (result == Digest::Match) {
            // The card may have lost power just after the verified rename.
            unlink(packed);
            unlink(temp);
            return nullptr;
        }
        return "An existing file has different contents. Keep it or move it before retrying.";
    }
    if (errno != ENOENT)
        return "Could not inspect files on the card.";
    uint32_t pos = 0;
    if (!stat(packed, &st)) {
        if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > a.compressed)
            return "Invalid staged download.";
        pos = st.st_size;
    } else if (errno != ENOENT)
        return "Could not inspect the staged download.";
    // Only our reserved output file is replaceable. Keep compressed bytes on retry.
    if (unlink(temp) && errno != ENOENT)
        return "Could not reset the staged image.";
    uint64_t total, free;
    if (esp_vfs_fat_info(BOARD_SD_MOUNT_POINT, &total, &free) != ESP_OK ||
        free < uint64_t(a.size) + a.compressed - pos + 1024 * 1024)
        return "Insufficient space on the card for this installation.";
    const char *error = download(a, packed, pos, s);
    if (error || cancelled())
        return error;
    Digest packedStatus = checkFile(packed, a.compressedHash, a.compressed, s, a);
    if (packedStatus != Digest::Match) {
        if (packedStatus == Digest::Different)
            unlink(packed);
        return "Compressed file verification failed. Retry the download.";
    }
    error = expand(a, packed, temp, s);
    if (error || cancelled())
        return error;
    if (checkFile(temp, a.hash, a.size, s, a) != Digest::Match)
        return "Installed file failed its card readback. Retry to install again.";
    if (cancelled())
        return nullptr;
    if (!stat(target, &st) || errno != ENOENT || rename(temp, target))
        return "Could not publish the verified file on the card.";
    unlink(packed);
    return nullptr;
}

const char *prepareStage() {
    // A versioned, private directory is reserved exclusively for resumable work.
    char marker[96];
    snprintf(marker, sizeof(marker), "%s/installer-v1", stageDir);
    if (mkdir(stageDir, 0755) != 0) {
        if (errno != EEXIST)
            return "Could not create the starter staging folder.";
        struct stat st;
        if (!stat(marker, &st) && S_ISREG(st.st_mode) && !st.st_size)
            return nullptr;
        DIR *dir = opendir(stageDir);
        if (!dir)
            return "Could not inspect the starter staging folder.";
        bool empty = true;
        while (dirent *entry = readdir(dir)) {
            if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
                empty = false;
                break;
            }
        }
        closedir(dir);
        if (!empty)
            return "The starter staging folder contains other files.";
    }
    FileHandle f(open(marker, O_CREAT | O_EXCL | O_WRONLY, 0644));
    if (f.fd >= 0 && f.syncClose())
        return nullptr;
    return "Could not open the starter staging folder.";
}

void worker(void *choice) {
    const Asset &os = choice == &os7 ? os7 : os8;
    auto *s = Scratch::allocate();
    const char *error = s ? prepareStage() : "Insufficient memory for installation.";
    if (!error)
        error = install(rom, *s);
    if (!error && !cancelled())
        error = install(os, *s);
    free(s);
    portENTER_CRITICAL(&lock);
    const bool stopped = cancelled();
    info.state = stopped ? StarterState::Cancelled : error ? StarterState::Failed : StarterState::Done;
    snprintf(info.message, sizeof(info.message), "%s",
             stopped ? "Installation cancelled."
             : error ? error
                     : "Startup files verified.");
    portEXIT_CRITICAL(&lock);
    vTaskDelete(nullptr);
}
} // namespace

StarterDownloadInfo StarterDownloadStatus() {
    portENTER_CRITICAL(&lock);
    StarterDownloadInfo result = info;
    result.network_bytes_per_second = network_speed.bytesPerSecond(millis());
    portEXIT_CRITICAL(&lock);
    return result;
}
void StarterDownloadCancel() { __atomic_store_n(&interrupted, true, __ATOMIC_RELEASE); }
bool StarterDownloadStart(bool system7) {
    portENTER_CRITICAL(&lock);
    if (info.state == StarterState::Working) {
        portEXIT_CRITICAL(&lock);
        return false;
    }
    __atomic_store_n(&interrupted, false, __ATOMIC_RELEASE);
    info = {StarterState::Working, 0, 0, "Preparing the card."};
    portEXIT_CRITICAL(&lock);
    if (xTaskCreatePinnedToCore(worker, "starter", 8192, const_cast<Asset *>(system7 ? &os7 : &os8), 2,
                                nullptr, 0) == pdPASS)
        return true;
    portENTER_CRITICAL(&lock);
    info.state = StarterState::Failed;
    snprintf(info.message, sizeof(info.message), "Insufficient memory for installation.");
    portEXIT_CRITICAL(&lock);
    return false;
}
