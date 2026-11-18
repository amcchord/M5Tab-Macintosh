#pragma once
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "board_display.h"
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using std::vector;
#define UNUSED(x) ((void)(x))
#define DRAM_ATTR
#define SDATA_ATTR(name)
#define likely(x) (x)
#define D(x) ((void)0)
struct FakeSerial {
    void println(const char *) {}
    template<class... Args> void printf(const char *, Args...) {}
};
inline FakeSerial Serial;
inline void *ps_malloc(size_t size) { return malloc(size); }
#include "sysdeps.h"
#include "video.h"
inline uint8 *MacFrameBaseHost;
inline uint32 MacFrameBaseMac = 0x10000000, MacFrameSize;
inline int MacFrameLayout;
#define FLAYOUT_DIRECT 0
inline bool AutomationSerialCaptureActive() { return false; }
inline void TouchOverlay_BeginFrame() {}
inline bool test_overlay_covered = false;
inline int test_overlay_composites = 0;
inline void TouchOverlay_CompositeTile(int,int,int,int,uint16 *) { ++test_overlay_composites; }
inline bool TouchOverlay_TileCovered(int,int,int,int) { return test_overlay_covered; }
struct esp_task_wdt_config_t { int timeout_ms; int idle_core_mask; bool trigger_panic; };
inline void esp_task_wdt_reconfigure(esp_task_wdt_config_t *) {}
void VideoSignalFrameReady();
void VideoExit();
