#include <array>
#include <algorithm>
#include <cstring>
#define OVERLAY_HOST_TEST
#include "../src/basilisk/touch_overlay.cpp"

static int notifications = 0;
static TouchOverlayMode expected_mode;
extern "C" void VideoMarkTilesDirtyRect(int, int, int w, int h)
{
    assert(w > 0 && h > 0);
    // Damage notification must only become observable after the new snapshot.
    assert(TouchOverlay_GetMode() == expected_mode);
    ++notifications;
}

int main()
{
    TouchOverlay_Init(1280, 720, 640, 360);
    assert(!TouchOverlay_IsVisible());
    expected_mode = TOUCH_OVERLAY_KEYBOARD;
    {
        OverlayInputGuard guard;
        toggle_mode(expected_mode);
    }
    assert(notifications == 1);
    TouchOverlay_BeginFrame();
    assert(s_frame.mode == TOUCH_OVERLAY_KEYBOARD);
    const auto keyboard = s_frame;
    expected_mode = TOUCH_OVERLAY_GAMING;
    {
        OverlayInputGuard guard;
        toggle_mode(expected_mode);
    }
    assert(notifications == 2 && s_frame.mode == TOUCH_OVERLAY_KEYBOARD);
    // All tiles of the old batch still see exactly the same keyboard.
    assert(memcmp(&keyboard, &s_frame, sizeof(keyboard)) == 0);
    TouchOverlay_BeginFrame();
    assert(s_frame.mode == TOUCH_OVERLAY_GAMING && s_frame.count == s_game_count);
    {
        OverlayInputGuard guard;
        press_key_index(0);
    }
    assert(!s_frame.keys[0].held && s_published.keys[0].held);
    TouchOverlay_BeginFrame();
    assert(s_frame.keys[0].held);
    // Guard the compositing buffer while exercising every panel tile.
    std::array<uint16_t, 80 * 80 + 2> tile;
    for (int y = 0; y < 720; y += 80) for (int x = 0; x < 1280; x += 80) {
        tile.fill(0x1234); tile.front() = 0xdead; tile.back() = 0xbeef;
        TouchOverlay_CompositeTile(x, y, 80, 80, tile.data() + 1);
        assert(tile.front() == 0xdead && tile.back() == 0xbeef);
    }
    expected_mode = TOUCH_OVERLAY_NONE;
    TouchOverlay_Shutdown();
    assert(!TouchOverlay_IsVisible());
    TouchOverlay_BeginFrame();
    assert(s_frame.mode == TOUCH_OVERLAY_NONE);
    puts("PASS: overlay publication ordering, immutable batches, key state and tile bounds");
}
