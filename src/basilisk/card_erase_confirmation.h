// A serial erase consumes a separately issued challenge for one inspected card.
// Touch confirmation is bound to the same card and uses an independent tap.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

struct CardEraseConfirmation {
    char token[33] = {};
    char challenge[9] = {};

    void clear() { token[0] = challenge[0] = 0; }

    bool arm(const char *expected, const char *actual, bool supported, uint32_t nonce)
    {
        clear();
        if (!supported || strlen(actual) != 32 || strcmp(expected, actual)) return false;
        memcpy(token, actual, sizeof(token));
        snprintf(challenge, sizeof(challenge), "%08lx", (unsigned long)nonce);
        return true;
    }

    bool consume(const char *actual, const char *provided, const char *response)
    {
        const bool accepted = token[0] && !strcmp(actual, token) && !strcmp(provided, token) &&
                              !strcmp(response, challenge);
        clear();
        return accepted;
    }
};
