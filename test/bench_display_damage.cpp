// Host timing of the production dirty collector and rotated panel writer.
// Build and compare revisions as described in EMULATOR_PERFORMANCE.md.
#include <chrono>
#include <cstdio>
#include <vector>
#include "board_display_surface.h"
#include "panel_surface.h"
#define VIDEO_HOST_TEST
#include "video_esp32.cpp"
#include "video.cpp"

static void drawOutline(uint8 *guest, int stride, int bits, int frame)
{
    const int x = 2 + frame % 22;
    const int y = 2 + frame % 5;
    for (int row = y; row < y + 30; ++row) {
        const int first = row * stride + x * bits / 8;
        const int last = row * stride + (x + 219) * bits / 8;
        if (row == y || row == y + 29) {
            for (int i = first; i <= last; ++i) guest[i] ^= 0xff;
            VideoMarkDirtyRange(first, last - first + 1);
        } else {
            guest[first] ^= 0xff;
            guest[last] ^= 0xff;
            VideoMarkDirtyOffset(first);
            VideoMarkDirtyOffset(last);
        }
    }
}

int main()
{
    std::vector<uint16> panel(DISPLAY_WIDTH * DISPLAY_HEIGHT);
    std::vector<uint8> guest(MAC_SCREEN_WIDTH * MAC_SCREEN_HEIGHT);
    mac_frame_buffer = guest.data();
    frame_buffer_size = guest.size();
    if (!BoardDisplay_AttachSurface(panel.data(), DISPLAY_HEIGHT, DISPLAY_WIDTH)) return 1;
    uint16 palette[256];
    for (int i = 0; i < 256; ++i) palette_rgb565[i] = (i * 257) ^ 0x5555;

    for (int bits : {1, 8}) {
        const int stride = MAC_SCREEN_WIDTH * bits / 8;
        updateVideoStateCache(bits == 1 ? VDEPTH_1BIT : VDEPTH_8BIT, stride);
        for (bool dense : {false, true}) {
            std::fill(guest.begin(), guest.end(), 0);
            force_full_update = palette_changed = true;
            renderPendingFrame(palette);
            const uint32 before_bytes = perf_panel_bytes;
            const auto started = std::chrono::steady_clock::now();
            constexpr int frames = 1000;
            for (int frame = 0; frame < frames; ++frame) {
                if (dense) {
                    const int length = stride * MAC_SCREEN_HEIGHT;
                    memset(guest.data(), frame & 0xff, length);
                    VideoMarkDirtyRange(0, length);
                } else {
                    drawOutline(guest.data(), stride, bits, frame);
                }
                renderPendingFrame(palette);
            }
            const auto elapsed = std::chrono::steady_clock::now() - started;
            const double usec = std::chrono::duration<double, std::micro>(elapsed).count();
            uint64_t hash = 0;
            for (auto pixel : panel) hash = hash * 31 + pixel;
            printf("%dbpp %s %.2f us/frame, %.1f bytes/frame, hash %016llx\n",
                   bits, dense ? "dense" : "outline", usec / frames,
                   (perf_panel_bytes - before_bytes) / double(frames),
                   (unsigned long long)hash);
        }
    }
}
