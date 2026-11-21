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

// Replay the same sequence of Finder selection endpoints used on the board.
// The guest bitmap receives an XOR erase and draw before each presentation.
struct Point { int x, y; };

static void xorLine(uint8 *guest, int stride, int bits, int y, int left, int right)
{
    for (int x = left; x <= right; ++x) {
        const int shift = 8 - bits - (x * bits % 8);
        guest[y * stride + x * bits / 8] ^= ((1u << bits) - 1u) << shift;
    }
    VideoMarkDirtyRange(y * stride + left * bits / 8, right * bits / 8 - left * bits / 8 + 1);
}

static void selection(uint8 *guest, int stride, int bits, Point anchor, Point end)
{
    xorLine(guest, stride, bits, anchor.y, anchor.x, end.x);
    for (int y = anchor.y + 1; y < end.y; ++y) {
        xorLine(guest, stride, bits, y, anchor.x, anchor.x);
        xorLine(guest, stride, bits, y, end.x, end.x);
    }
    xorLine(guest, stride, bits, end.y, anchor.x, end.x);
}

int main(int argc, char **argv)
{
    std::vector<Point> trace;
    if (argc > 1) {
        FILE *file = argc == 2 ? fopen(argv[1], "r") : nullptr;
        if (!file) return 2;
        Point p;
        int read;
        while ((read = fscanf(file, "%d %d", &p.x, &p.y)) == 2) {
            if (p.x < 0 || p.y < 0 || p.x >= MAC_SCREEN_WIDTH || p.y >= MAC_SCREEN_HEIGHT ||
                (!trace.empty() && (p.x <= trace.front().x || p.y <= trace.front().y))) {
                fclose(file);
                return 2;
            }
            trace.push_back(p);
        }
        fclose(file);
        if (read != EOF || trace.size() < 3) return 2;
    }
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
        const std::vector<const char *> workloads = trace.empty() ?
            std::vector<const char *>{"outline", "dense"} : std::vector<const char *>{"finder"};
        for (const char *name : workloads) {
            const bool finder = !trace.empty();
            std::fill(guest.begin(), guest.end(), 0);
            force_full_update = palette_changed = true;
            if (finder) selection(guest.data(), stride, bits, trace.front(), trace[1]);
            renderPendingFrame(palette);
            const uint32 before_bytes = perf_panel_bytes;
            const auto started = std::chrono::steady_clock::now();
            const int frames = finder ? static_cast<int>(trace.size()) - 2 : 1000;
            for (int frame = 0; frame < frames; ++frame) {
                if (finder) {
                    selection(guest.data(), stride, bits, trace.front(), trace[frame+1]);
                    selection(guest.data(), stride, bits, trace.front(), trace[frame+2]);
                } else if (!strcmp(name, "dense")) {
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
                   bits, name, usec / frames,
                   (perf_panel_bytes - before_bytes) / double(frames),
                   (unsigned long long)hash);
        }
    }
}
