#include <array>
#include <atomic>
#include <iostream>
#include <limits>
#include <random>
#include "esp_cache.h"
#include "board_display_surface.h"
#include "panel_surface.h"
#define VIDEO_HOST_TEST
#include "../src/basilisk/video_esp32.cpp"

// Guest stores record 32-pixel spans: one tile at every depth.
static constexpr int kSpanTiles = 1;
static int collect() { return collectWriteDirtyTiles(current_bytes_per_row, current_pixels_per_byte, video_dirty_shift); }


static std::mt19937 rng(582913);

static void test_ranges()
{
    for (uint32 ppb : {1u, 2u, 4u, 8u}) for (uint32 height : {360u, 400u})
        for (uint32 padding : {0u, 4u}) {
            const uint32 stride = 640 / ppb + padding, capacity = 640 * height;
            for (int trial = 0; trial < 1000; ++trial) {
                uint32 offset = rng() % capacity, length = rng() % 12000;
                if (trial == 0) { offset = 0; length = UINT32_MAX; }
                std::array<bool, 160> actual{}, expected{};
                markDirtyByteRange(offset, length, capacity, stride, ppb, 640, height,
                    40, 40, [&](uint32 tile) { assert(tile < 160); actual[tile] = true; });
                const uint64_t end = std::min(uint64_t(capacity), uint64_t(offset) + length);
                for (uint64_t b = offset; b < end && b / stride < height; ++b)
                    for (uint32 p = 0; p < ppb; ++p) {
                        const uint32 x = (b % stride) * ppb + p;
                        if (x < 640) expected[(b / stride / 40) * 16 + x / 40] = true;
                    }
                assert(actual == expected);
            }
        }
}

static void test_rotation()
{
    for (int pw : {720, 800}) for (bool flip : {false, true}) {
        std::vector<uint16> actual(pw * 1280, 0xdead), expected = actual;
        for (int trial = 0; trial < 100; ++trial) {
            int w = 1 + rng() % 140, h = 1 + rng() % 100;
            int x = rng() % (1281 - w), y = rng() % (pw + 1 - h);
            std::vector<uint16> pixels(w * h);
            for (auto &v : pixels) v = rng();
            assert(writePanelTile(actual.data(), pw, 1280, flip, x, y, w, h, pixels.data()));
            for (int r = 0; r < h; ++r) for (int c = 0; c < w; ++c) {
                int px = flip ? y + r : pw - 1 - y - r;
                int py = flip ? 1279 - x - c : x + c;
                expected[py * pw + px] = pixels[r * w + c];
            }
            assert(actual == expected);
        }
        assert(!writePanelTile(actual.data(), pw, 1280, flip, 1270, 0, 80, 80, actual.data()));
        assert(!writePanelTile(actual.data(), pw, 1280, flip, -1, 0, 1, 1, actual.data()));
        assert(actual == expected);
    }
}

// The fused indexed path must equal unpacking and pixel doubling followed by
// writePanelTile, at every packed depth and source alignment.
static void test_indexed_rotation()
{
    std::vector<uint32_t> pairs(256);
    std::vector<uint16> palette(256);
    for (int i = 0; i < 256; ++i) { palette[i] = rng(); pairs[i] = palette[i] * 0x00010001u; }
    for (int bits : {1, 2, 4, 8}) for (int pw : {720, 800}) for (bool flip : {false, true}) {
        std::vector<uint16> actual(pw * 1280, 0xdead), expected = actual;
        for (int trial = 0; trial < 60; ++trial) {
            const int sw = 1 + rng() % 70, sh = 1 + rng() % 50, first = rng() % 40;
            const int stride = ((first + sw) * bits + 7) / 8 + rng() % 5;
            const int w = sw * 2, h = sh * 2;
            const int x = rng() % (1281 - w), y = 2 * (rng() % ((pw - h) / 2 + 1));
            std::vector<uint8> src(stride * sh);
            for (auto &v : src) v = rng();
            std::vector<uint16> doubled(w * h);
            for (int r = 0; r < h; ++r) for (int c = 0; c < w; ++c) {
                const int bit = (first + c / 2) * bits;
                const int index = (src[(r / 2) * stride + bit / 8] >> (8 - bits - bit % 8)) & ((1 << bits) - 1);
                doubled[r * w + c] = palette[index];
            }
            assert(writePanelTile(expected.data(), pw, 1280, flip, x, y, w, h, doubled.data()));
            assert(writePanelIndexedTile2x(actual.data(), pw, 1280, flip, x, y, w, h,
                                           src.data(), stride, bits, first, pairs.data()));
            assert(actual == expected);
        }
        assert(!writePanelIndexedTile2x(actual.data(), pw, 1280, flip, 1270, 0, 80, 80,
                                        nullptr, 40, bits, 0, pairs.data()));
        assert(!writePanelIndexedTile2x(actual.data(), pw, 1280, flip, 0, 1, 80, 80,
                                        (const uint8 *)actual.data(), 40, bits, 0, pairs.data()));
        assert(!writePanelIndexedTile2x(actual.data(), pw, 1280, flip, 0, 0, 80, 80,
                                        (const uint8 *)actual.data(), 40, 3, 0, pairs.data()));
        assert(actual == expected);
    }
}

static void assert_panel(const std::vector<uint16> &panel, const std::vector<uint8> &guest,
                         video_depth depth, bool flip)
{
    const int bits = 1 << depth, ppb = 8 / bits, stride = MAC_SCREEN_WIDTH / ppb;
    for (int y = 0; y < MAC_SCREEN_HEIGHT; ++y) for (int x = 0; x < MAC_SCREEN_WIDTH; ++x) {
        int index = (guest[y * stride + x / ppb] >> (8 - bits * (x % ppb + 1))) & ((1 << bits) - 1);
        for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
            const int lx = x * 2 + dx, ly = y * 2 + dy;
            const int px = flip ? ly : DISPLAY_HEIGHT - 1 - ly;
            const int py = flip ? DISPLAY_WIDTH - 1 - lx : lx;
            assert(panel[py * DISPLAY_HEIGHT + px] == palette_rgb565[index]);
        }
    }
}

static void test_pipeline()
{
    std::vector<uint16> panel(DISPLAY_WIDTH * DISPLAY_HEIGHT);
    std::vector<uint8> guest(MAC_SCREEN_WIDTH * MAC_SCREEN_HEIGHT);
    assert(!BoardDisplay_BeginTiles());
    assert(!BoardDisplay_AttachSurface(panel.data(), 1, 1));
    assert(BoardDisplay_AttachSurface(panel.data(), DISPLAY_HEIGHT, DISPLAY_WIDTH));
    auto &gfx = BoardDisplay_Gfx();
    BoardDisplay_Present();

    // A draw during cache writeback must still trigger another flush.
    gfx.drawPixel(0, 0, 0x1234);
    const int before = cache_calls;
    cache_hook = [&] { gfx.drawPixel(1, 0, 0x5678); };
    BoardDisplay_Present(); BoardDisplay_Present(); BoardDisplay_Present();
    assert(cache_calls == before + 2);
    gfx.drawPixel(0, 0, 0);
    cache_result = -1; BoardDisplay_Present();
    cache_result = ESP_OK; BoardDisplay_Present();
    assert(cache_calls == before + 4);

    mac_frame_buffer = guest.data(); frame_buffer_size = guest.size();
    preserve_splash_until_first_write = false;
    uint16 local_palette[256] = {};
    for (bool flip : {false, true}) for (auto depth : {VDEPTH_8BIT, VDEPTH_1BIT, VDEPTH_2BIT, VDEPTH_4BIT}) {
        gfx.setFlip180(flip);
        for (auto &v : guest) v = rng();
        initDefaultPalette(depth);
        assert(palette_changed);
        renderPendingFrame(local_palette);
        assert_panel(panel, guest, depth, flip);
        assert(collect() == 0);
        // A long partial write crosses many tiles and several tile rows.
        std::fill(guest.begin() + 39, guest.begin() + 16000, 0);
        VideoMarkDirtyRange(39, 16000 - 39);
        renderPendingFrame(local_palette);
        assert_panel(panel, guest, depth, flip);
        // Palette-only changes must redraw all pixels with no framebuffer writes.
        uint8 rgb[768]; for (auto &v : rgb) v = rng();
        vector<video_mode> modes(1);
        ESP32_monitor_desc monitor(modes, depth, 0);
        monitor.set_palette(rgb, 256);
        renderPendingFrame(local_palette);
        assert_panel(panel, guest, depth, flip);
    }
    VideoMarkTilesDirtyRect(INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX);
    assert(collect() == 0);
    VideoMarkTilesDirtyRect(-10, -10, 11, 11);
    assert(collect() == 1);
    initDefaultPalette(VDEPTH_8BIT); renderPendingFrame(local_palette);
    // A failed batch must retain old damage and merge the next write.
    guest[0] ^= 0xff; VideoMarkDirtyOffset(0);
    cache_result = -1; renderPendingFrame(local_palette);
    assert(collect() == kSpanTiles);
    guest[80] ^= 0xff; VideoMarkDirtyOffset(80);
    cache_result = ESP_OK; renderPendingFrame(local_palette);
    assert(collect() == 0);
    assert_panel(panel, guest, VDEPTH_8BIT, true);
    // Writes to the same tile after collection cannot be collapsed away.
    VideoMarkDirtyOffset(0); assert(collect() == kSpanTiles);
    renderPendingFrame(local_palette);
    VideoMarkDirtyOffset(0); assert(collect() == kSpanTiles);
    // A write during publication remains pending for the next batch.
    cache_hook = [&] { guest[0] ^= 0xff; VideoMarkDirtyOffset(0); };
    renderPendingFrame(local_palette);
    assert(collect() == kSpanTiles);
    renderPendingFrame(local_palette);
    assert_panel(panel, guest, VDEPTH_8BIT, true);

    // No guest writes at all: splash still has a real timeout.
    preserve_splash_until_first_write = true; preserve_splash_armed_ms = 0;
    test_ms = 100; initDefaultPalette(VDEPTH_8BIT);
    std::fill(guest.begin(), guest.end(), 0);
    renderPendingFrame(local_palette); assert(preserve_splash_until_first_write);
    test_ms = PRESERVE_SPLASH_MAX_MS;
    renderPendingFrame(local_palette); assert(!preserve_splash_until_first_write);
    assert_panel(panel, guest, VDEPTH_8BIT, true);
    // Packed monochrome stipples contain real content even if all bytes match.
    preserve_splash_until_first_write = true; preserve_splash_armed_ms = test_ms;
    initDefaultPalette(VDEPTH_1BIT); std::fill(guest.begin(), guest.end(), 0x55);
    VideoMarkDirtyRange(0, current_bytes_per_row * MAC_SCREEN_HEIGHT);
    renderPendingFrame(local_palette); assert(!preserve_splash_until_first_write);
    assert_panel(panel, guest, VDEPTH_1BIT, true);

    mac_frame_buffer = nullptr; frame_buffer_size = 0;
}

static void test_lifecycle()
{
    fail_task_create = true;
    assert(!VideoInit(false));
    assert(!mac_frame_buffer && !MacFrameBaseHost && MacFrameSize == 0 && !video_task_stopped);
    fail_task_create = false;
    fail_semaphore_after = 0; // completion semaphore allocation
    assert(!VideoInit(false));
    assert(!mac_frame_buffer && !video_task_stopped);
    assert(VideoInit(false));
    assert(!VideoInit(false)); // never replace an active renderer's framebuffer
    // Hold a capture lease while teardown waits for the worker and then capture.
    xSemaphoreTake(video_capture_mutex, portMAX_DELAY);
    std::atomic<bool> exited{false};
    std::thread exit([&] { VideoExit(); exited = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    assert(!exited && mac_frame_buffer);
    VideoSignalFrameReady(); // detached handle is safe during teardown
    xSemaphoreGive(video_capture_mutex);
    exit.join(); joinTasks();
    assert(exited && !mac_frame_buffer && !MacFrameBaseHost && MacFrameSize == 0);
    uint8 pixel; uint16 palette[256], width, height;
    assert(!VideoCaptureFrame(&pixel, 1, palette, &width, &height));
    // Non-emulator clients may still call the capture API after exit.
    std::vector<uint8> pixels(MAC_SCREEN_WIDTH * MAC_SCREEN_HEIGHT);
    assert(!VideoCaptureFrame(pixels.data(), pixels.size(), palette, &width, &height));
}

int main()
{
    test_ranges(); test_rotation(); test_indexed_rotation(); test_pipeline(); test_lifecycle();
    puts("PASS: randomized damage, panel rotation, production tile/palette pipeline, retries, splash and lifecycle");
}
