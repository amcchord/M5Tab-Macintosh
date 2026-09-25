#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
struct TestSerial {
    template<typename... Args> void printf(const char *, Args...) {}
};
inline TestSerial Serial;
