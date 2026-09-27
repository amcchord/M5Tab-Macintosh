#pragma once
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "touch_overlay.h"
#include "chicago_font_data.h"
struct FakeSerial {
    void println(const char *) {}
    template<class... Args> void printf(const char *, Args...) {}
};
inline FakeSerial Serial;
inline void InputKeyDown(uint8_t) {}
inline void InputKeyUp(uint8_t) {}
inline void ADBSetRelMouseMode(bool) {}
inline void ADBMouseMoved(int, int) {}
inline void ADBMouseDown(int) {}
inline void ADBMouseUp(int) {}
