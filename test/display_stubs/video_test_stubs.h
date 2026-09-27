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
#define likely(x) (x)
#define D(x) ((void)0)
struct FakeSerial {
    void println(const char *) {}
    template<class... Args> void printf(const char *, Args...) {}
};
inline FakeSerial Serial;
inline void *ps_malloc(size_t size) { return malloc(size); }
enum video_depth { VDEPTH_1BIT, VDEPTH_2BIT, VDEPTH_4BIT, VDEPTH_8BIT };
struct video_mode { int x, y, resolution_id, user_data; video_depth depth; uint32 bytes_per_row; };
inline uint32 TrivialBytesPerRow(int width, video_depth depth) { return width / (8 >> depth); }
struct monitor_desc {
    video_mode mode;
    monitor_desc(const vector<video_mode> &modes, video_depth, uint32) : mode(modes.back()) {}
    virtual ~monitor_desc() = default;
    const video_mode &get_current_mode() { return mode; }
    void set_mac_frame_base(uint32) {}
};
inline vector<monitor_desc *> VideoMonitors;
inline uint8 *MacFrameBaseHost;
inline uint32 MacFrameBaseMac = 0x10000000, MacFrameSize;
inline int MacFrameLayout;
#define FLAYOUT_DIRECT 0
inline bool AutomationSerialCaptureActive() { return false; }
inline void TouchOverlay_BeginFrame() {}
inline void TouchOverlay_CompositeTile(int,int,int,int,uint16 *) {}
struct esp_task_wdt_config_t { int timeout_ms; int idle_core_mask; bool trigger_panic; };
inline void esp_task_wdt_reconfigure(esp_task_wdt_config_t *) {}
void VideoSignalFrameReady();
void VideoExit();
