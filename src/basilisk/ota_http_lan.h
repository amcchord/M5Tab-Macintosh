// Restricted HTTP/1.x download from the numeric endpoint authorized over USB.
// Used by ESP-IDF versions without public access to the HTTP client's socket.
#pragma once

#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>

class OtaHttpLan {
public:
    enum class Result { Pending, Done, Error };

    ~OtaHttpLan() {
        if (fd >= 0) lwip_close(fd);
    }

    bool open(const sockaddr_in &peer) {
        char ip[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip))) return false;
        const int length = snprintf(buffer, sizeof(buffer),
            "GET /firmware.bin HTTP/1.0\r\nHost: %s:%u\r\nConnection: close\r\n\r\n",
            ip, unsigned(ntohs(peer.sin_port)));
        if (length <= 0 || size_t(length) >= sizeof(buffer)) return false;
        end = length;
        fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0 || lwip_fcntl(fd, F_SETFL, O_NONBLOCK) < 0) return false;
        connected = lwip_connect(fd, reinterpret_cast<const sockaddr *>(&peer), sizeof(peer)) == 0;
        return connected || errno == EINPROGRESS || wouldBlock();
    }

    Result sendRequest() {
        if (!connected) {
            fd_set writable, errors;
            FD_ZERO(&writable);
            FD_ZERO(&errors);
            FD_SET(fd, &writable);
            FD_SET(fd, &errors);
            timeval now = {};
            int ready = lwip_select(fd + 1, nullptr, &writable, &errors, &now);
            if (!ready || (ready < 0 && errno == EINTR)) return Result::Pending;
            int error = 0;
            socklen_t length = sizeof(error);
            if (ready < 0 || lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error)
                return Result::Error;
            connected = true;
        }
        const int n = lwip_send(fd, buffer + pos, end - pos, 0);
        if (n < 0 && wouldBlock()) return Result::Pending;
        if (n <= 0) return Result::Error;
        pos += n;
        if (pos < end) return Result::Pending;
        pos = end = 0;
        return Result::Done;
    }

    Result headers(uint32_t expected) {
        const size_t old_end = end;
        const int n = lwip_recv(fd, buffer + end, sizeof(buffer) - end, 0);
        if (n < 0 && wouldBlock()) return Result::Pending;
        if (n <= 0) return Result::Error;
        end += n;
        // Include the last three bytes from the preceding receive.
        for (size_t i = old_end > 3 ? old_end - 3 : 0; i + 4 <= end; ++i) {
            if (memcmp(buffer + i, "\r\n\r\n", 4)) continue;
            pos = i + 4;
            if (!validHeaders(pos, expected) || end - pos > expected) return Result::Error;
            return Result::Done;
        }
        return end < sizeof(buffer) ? Result::Pending : Result::Error;
    }

    int read(char *out, size_t want) {
        if (pos < end) {
            size_t count = end - pos;
            if (count > want) count = want;
            memcpy(out, buffer + pos, count);
            pos += count;
            return int(count);
        }
        return lwip_recv(fd, out, want, 0);
    }

private:
    int fd = -1;
    bool connected = false;
    size_t pos = 0, end = 0;
    char buffer[2048];

    static bool wouldBlock() {
        return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
    }
    bool validHeaders(size_t bytes, uint32_t expected) {
        // The header delimiter is present. Terminate its text in place while
        // retaining any firmware bytes coalesced with the final header.
        for (size_t i = 0; i < bytes; ++i) {
            const unsigned char c = buffer[i];
            if (c == '\r') {
                if (++i >= bytes || buffer[i] != '\n') return false;
            } else if ((c < 32 && c != '\t') || c == 127) {
                return false;
            }
        }
        buffer[bytes - 2] = 0;
        char *line = buffer;
        char *next = strstr(line, "\r\n");
        if (!next) return false;
        *next = 0;
        if ((strncmp(line, "HTTP/1.0 200", 12) && strncmp(line, "HTTP/1.1 200", 12)) ||
            (line[12] && line[12] != ' ')) return false;
        bool have_length = false;
        for (line = next + 2; *line; line = next + 2) {
            next = strstr(line, "\r\n");
            if (!next) return false;
            *next = 0;
            char *value = strchr(line, ':');
            if (!value || value == line) return false;
            *value++ = 0;
            for (const unsigned char *p = reinterpret_cast<unsigned char *>(line); *p; ++p) {
                if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                      (*p >= '0' && *p <= '9') || strchr("!#$%&'*+-.^_`|~", *p)))
                    return false;
            }
            while (*value == ' ' || *value == '\t') ++value;
            if (!strcasecmp(line, "Transfer-Encoding")) return false;
            if (strcasecmp(line, "Content-Length")) continue;
            if (have_length || *value < '0' || *value > '9') return false;
            uint32_t size = 0;
            while (*value >= '0' && *value <= '9') {
                // The USB-selected image is at most 6 MiB.
                size = size * 10 + unsigned(*value++ - '0');
                if (size > expected) return false;
            }
            while (*value == ' ' || *value == '\t') ++value;
            if (*value || size != expected) return false;
            have_length = true;
        }
        return have_length;
    }
};
