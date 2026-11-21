#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Callers serialize access to the session shared by USB and the network task.
class WebSession {
public:
    enum Access { Closed, View, Control };

    void open(const uint8_t (&random)[16], Access mode, int64_t now_us) {
        for (size_t i = 0; i < sizeof(random); ++i)
            snprintf(token + 2 * i, 3, "%02x", random[i]);
        access = mode;
        expires_us = now_us + 30LL * 60 * 1000000;
    }
    void close() { access = Closed; token[0] = 0; }
    bool active(int64_t now_us) const { return access != Closed && now_us < expires_us; }
    Access authenticate(const char *candidate, int64_t now_us) const {
        if (!active(now_us) || strlen(candidate) != sizeof(token) - 1) return Closed;
        uint8_t diff = 0;
        for (size_t i = 0; i < sizeof(token) - 1; ++i)
            diff |= (uint8_t)(candidate[i] ^ token[i]);
        return diff == 0 ? access : Closed;
    }
    char token[33] = {};
private:
    Access access = Closed;
    int64_t expires_us = 0;
};
