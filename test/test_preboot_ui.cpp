// Render every preboot screen through the production panel path and check
// that the panel holds the canvas scaled in both rotations. With
// PREBOOT_UI_OUT set, each screen is also written there as a binary PPM.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "esp_cache.h"
#include "board_display.h"
#include "board_display_surface.h"
#include "panel_surface.h"
#include "preboot_screens.h"

using namespace PrebootScreens;
namespace UI = PrebootUI;

static std::vector<uint16_t> panel;

static void expectScaled(const char *name)
{
    std::vector<uint8_t> pixels(UI::Width() * UI::Height());
    uint16_t palette[256], w = 0, h = 0;
    assert(UI::CaptureFrame(pixels.data(), pixels.size(), palette, &w, &h));
    assert(w == BoardDisplay_Width() / UI::kScale && h == BoardDisplay_Height() / UI::kScale);
    auto &gfx = BoardDisplay_Gfx();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint16_t want = palette[pixels[y * w + x]];
            for (int dy = 0; dy < UI::kScale; ++dy)
                for (int dx = 0; dx < UI::kScale; ++dx)
                    if (readPanelPixel(panel.data(), gfx.panelW(), gfx.panelH(), gfx.flip180(),
                                       UI::kScale * x + dx, UI::kScale * y + dy) != want) {
                        std::fprintf(stderr, "%s: panel (%d,%d) is not logical (%d,%d)\n",
                                     name, UI::kScale * x + dx, UI::kScale * y + dy, x, y);
                        std::abort();
                    }
        }
    uint32_t checked, mismatched;
    int first_x, first_y;
    assert(UI::VerifyPanel(&checked, &mismatched, &first_x, &first_y));
    assert(checked == panel.size() && mismatched == 0);
    if (const char *dir = std::getenv("PREBOOT_UI_OUT")) {
        std::string path = std::string(dir) + "/" + name + ".ppm";
        FILE *f = std::fopen(path.c_str(), "wb");
        assert(f);
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int i = 0; i < w * h; ++i) {
            const uint16_t c = palette[pixels[i]];
            const unsigned char rgb[3] = {
                (unsigned char)((((c >> 11) & 31) * 255 + 15) / 31),
                (unsigned char)((((c >> 5) & 63) * 255 + 31) / 63),
                (unsigned char)(((c & 31) * 255 + 15) / 31)};
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
    }
}

int main()
{
    panel.assign(BOARD_DISPLAY_WIDTH * BOARD_DISPLAY_HEIGHT, 0xdead);
    assert(BoardDisplay_AttachSurface(panel.data(), BOARD_DISPLAY_HEIGHT, BOARD_DISPLAY_WIDTH));
    assert(UI::Begin());
    const int W = UI::Width(), H = UI::Height();

    std::vector<std::string> disks = {"/Macintosh7.dsk", "/Macintosh8.dsk"};
    std::vector<std::string> cds = {"/System753.iso", "/A very long CD-ROM image name.iso",
                                    "/Utilities.iso", "/Games.iso", "/Extra.iso"};
    std::vector<std::string> folders = {"/Shared"};

    for (bool flip : {true, false}) {
        BoardDisplay_Gfx().setFlip180(flip);
        UI::InvalidateAll();
        const char *suffix = flip ? "" : "-unflipped";
        auto shot = [&](const char *name) {
            UI::Present();
            expectScaled((std::string(name) + suffix).c_str());
        };

        PaintSplash();
        shot("splash");

        SettingsView sv = {};
        sv.clock = "11:40 PM";
        sv.columns[0] = {"Hard Disk:", &disks, false, 1, 0, 0, UI::kIconHardDisk};
        sv.columns[1] = {"CD-ROM:", &cds, true, 0, 0, 0, UI::kIconCD};
        sv.columns[2] = {"Shared Folder:", &folders, true, 1, 0, 0, UI::kIconFolder};
        sv.ram_mb = 24;
        sv.sound = true;
        sv.rotate180 = true;
        sv.usb_available = true;
        sv.wifi_bars = 3;
        sv.wifi_status = "Wi-Fi: Connected to Home Network";
        sv.wifi_detail = "Address 192.168.1.20";
        sv.pressed = kNoControl;
        const SettingsLayout sl = LayoutSettings(W, H, true);
        PaintSettings(sl, sv);
        shot("settings");

        // Every control must be reachable at its own center.
        for (int i = 0; i < kSettingsControlCount; ++i) {
            const UI::Rect &r = sl.controls[i];
            assert(SettingsControlAt(sl, r.x + r.w / 2, r.y + r.h / 2) == i);
            assert(r.right() <= W && r.bottom() <= H);
        }
        sv.pressed = kStartButton;
        sv.columns[1].pressed_scroll = 1;
        sv.columns[1].first = 1;
        PaintSettings(sl, sv);
        shot("settings-pressed");

        const auto setup = LayoutStarter(W, H);
        StarterView starter = {"11:40 PM", "Choose Wi-Fi to connect to a network.", "Wi-Fi disconnected",
                               "Download", kNoControl, -1, false, false, false};
        PaintStarter(setup, starter);
        shot("starter");
        for (int i=0; i<kStarterCount; ++i) {
            const auto &r = setup.controls[i];
            assert(StarterControlAt(setup,r.x+r.w/2,r.y+r.h/2)==i);
            assert(r.right()<=W && r.bottom()<=H);
        }
        starter.status="Downloading the startup disk.  37%";
        starter.network="Wi-Fi connected";
        starter.action="Cancel"; starter.working=true; starter.action_enabled=true; starter.percent=37;
        PaintStarter(setup, starter);
        shot("starter-download");

        std::vector<NetworkRow> nets = {
            {"Home Network", 4, true}, {"Coffee Shop", 2, false}, {"Neighbor 5G", 1, true},
            {"Printer-Setup", 3, false}, {"Guest", 2, true}, {"Office", 4, true},
            {"Very long network name that will not fit", 1, true}};
        WiFiView wv = {};
        wv.clock = "11:40 PM";
        wv.networks = &nets;
        wv.selected = 0;
        wv.status = "Status: Connected to Home Network (192.168.1.20)";
        wv.password_length = 8;
        wv.can_join = true;
        wv.pressed = kWiFiNoControl;
        const WiFiLayout wl = LayoutWiFi(W, H);
        PaintWiFi(wl, wv);
        shot("wifi");
        for (int i = 0; i < kWiFiControlCount; ++i) {
            const UI::Rect &r = wl.controls[i];
            assert(WiFiControlAt(wl, r.x + r.w / 2, r.y + r.h / 2) == i);
        }
        std::vector<NetworkRow> none;
        wv.networks = &none;
        wv.selected = -1;
        wv.scanning = true;
        wv.can_join = false;
        wv.password_length = 0;
        wv.status = "Status: Not connected";
        PaintWiFi(wl, wv);
        shot("wifi-scanning");

        KeyboardView kv = {"11:40 PM", "Home Network", "hunter2", false, 'g'};
        const KeyboardLayout kl = LayoutKeyboard(W, H);
        PaintKeyboard(kl, kv);
        shot("keyboard");
        for (const auto &key : kl.keys) assert(KeyAt(kl, key.r.x + key.r.w / 2, key.r.y + key.r.h / 2) == key.normal);
        assert(KeyAt(kl, kl.ok.x + 5, kl.ok.y + 5) == kKeyOK);
        kv.shift = true;
        kv.pressed_key = kKeyNone;
        PaintKeyboard(kl, kv);
        shot("keyboard-shift");

        AlertView usb = {"11:40 PM", UI::kIconCard, "Your SD card is now available as a USB drive.",
                         "Copy or remove disk images on your computer, then tap Done to return "
                         "to the startup settings.",
                         "Use a USB-A to USB-C cable in the USB-A port (not USB-C).",
                         "Waiting for the computer...", "Done", false, false};
        PaintAlert(LayoutAlert(W, H, usb), usb);
        shot("usb");

        AlertView fw = {"11:40 PM", UI::kIconCaution, "Updating the Wi-Fi co-processor.",
                        "This happens once after a firmware update. The device restarts "
                        "automatically when it is done.",
                        "Do not unplug or switch off.", nullptr, nullptr, false, false};
        PaintAlert(LayoutAlert(W, H, fw), fw);
        shot("wifi-firmware");

        AlertView err = {nullptr, UI::kIconStop, "Basilisk II can't start.",
                         "SD card or ROM file not found", nullptr, nullptr, nullptr, false, true};
        PaintAlert(LayoutAlert(W, H, err), err);
        shot("error");

        AlertView off = {nullptr, UI::kIconNote, nullptr,
                         "It is now safe to switch off your Macintosh.", nullptr, nullptr, nullptr,
                         false, true};
        PaintAlert(LayoutAlert(W, H, off), off);
        shot("power-off");
    }

    // A single changed pixel exercises odd dirty bounds and the last panel pixel.
    for (bool flip : {false, true}) {
        BoardDisplay_Gfx().setFlip180(flip);
        UI::Fill(0, 0, W, H, UI::kBlack);
        UI::InvalidateAll();
        UI::Present();
        UI::Pixel(W - 1, H - 1, UI::kRed);
        UI::Present();
        expectScaled("last-pixel");
        UI::Pixel(3, 5, UI::kBlue);
        cache_result = -1;
        UI::Present();
        std::vector<uint8_t> shown(W * H);
        uint16_t palette[256], width, height;
        assert(UI::CaptureFrame(shown.data(), shown.size(), palette, &width, &height));
        assert(shown[5 * W + 3] == UI::kBlack);
        cache_result = ESP_OK;
        UI::Present();
        expectScaled("single-pixel-retry");
        assert(UI::CaptureFrame(shown.data(), shown.size(), palette, &width, &height));
        assert(shown[5 * W + 3] == UI::kBlue && shown[W * H - 1] == UI::kRed);
    }

    // Long names are shortened rather than overflowing.
    char fitted[32];
    UI::FitText("A very long CD-ROM image name.iso", 60, fitted, sizeof(fitted));
    assert(UI::TextWidth(fitted) <= 60 && std::strstr(fitted, "..."));
    UI::Release();
    assert(!UI::Active());
    std::puts("preboot ui ok");
    return 0;
}
