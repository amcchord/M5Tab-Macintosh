/*
 *  preboot_screens.cpp - layout and painting of each preboot screen
 *
 *  BasiliskII ESP32 Port
 *
 *  Every screen is a full-screen Mac OS 8 scene: the desktop, a menu bar and
 *  one Platinum window. Layouts are in logical pixels (half the panel), adapt
 *  to the taller Waveshare panel, and keep touch targets at least 22 logical
 *  pixels high.
 */

#include "preboot_screens.h"
#include "boot_control_values.h"

#include <stdio.h>
#include <string.h>

#include "asset_happy_mac.h"   /* generated from assets/happymacIconNew.png */

namespace PrebootScreens {

using namespace PrebootUI;

static constexpr int kMargin = 12;        /* window content inset */
static constexpr int kButtonHeight = 24;  /* push buttons, a little taller for touch */

static Rect mainWindow(int width, int height)
{
    return {8, kMenuBarHeight + 7, width - 16, height - kMenuBarHeight - 16};
}

static void backdrop(const char *clock)
{
    Desktop({0, kMenuBarHeight, Width(), Height() - kMenuBarHeight});
    MenuBar(clock);
}

static Rect buttonRowRect(const Rect &content, int x, int w)
{
    return {x, content.y + content.h - kMargin - kButtonHeight, w, kButtonHeight};
}

/* ---- Text helpers ------------------------------------------------------ */

int WrapText(const Rect &r, const char *text, uint8_t color, bool draw)
{
    if (!text) return 0;
    int lines = 0;
    char line[128];
    const char *p = text;
    while (*p) {
        // One paragraph at a time; blank paragraphs add spacing.
        const char *end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        size_t used = 0;
        const char *word = p;
        if (word == end) ++lines;
        while (word < end) {
            const char *word_end = word;
            while (word_end < end && *word_end != ' ') ++word_end;
            char candidate[128];
            const size_t wlen = (size_t)(word_end - word);
            if (used + wlen + 2 >= sizeof(candidate)) break;
            memcpy(candidate, line, used);
            size_t clen = used;
            if (clen) candidate[clen++] = ' ';
            memcpy(candidate + clen, word, wlen);
            clen += wlen;
            candidate[clen] = 0;
            if (used && TextWidth(candidate) > r.w) {
                line[used] = 0;
                if (draw) Text(r.x, r.y + 12 + lines * kLineHeight, line, color);
                ++lines;
                memcpy(line, word, wlen);
                used = wlen;
            } else {
                memcpy(line, candidate, clen);
                used = clen;
            }
            word = word_end;
            while (word < end && *word == ' ') ++word;
        }
        if (used) {
            line[used] = 0;
            if (draw) Text(r.x, r.y + 12 + lines * kLineHeight, line, color);
            ++lines;
        }
        p = *end ? end + 1 : end;
    }
    return lines * kLineHeight;
}

static const char *displayName(const std::string &path)
{
    const char *s = path.c_str();
    const char *slash = strrchr(s, '/');
    return slash && slash[1] ? slash + 1 : s;
}

/* ---- Lists ------------------------------------------------------------- */

ListGeometry ListAt(const Rect &frame)
{
    ListGeometry g;
    g.frame = frame;
    g.scroll = {frame.x + frame.w - kScrollBarWidth, frame.y, kScrollBarWidth, frame.h};
    g.rows = {frame.x + 1, frame.y + 1, frame.w - kScrollBarWidth - 1, frame.h - 2};
    g.visible = g.rows.h / kRowHeight;
    return g;
}

int ListClampFirst(const ListGeometry &list, int first, int count)
{
    const int max_first = count > list.visible ? count - list.visible : 0;
    if (first > max_first) first = max_first;
    return first < 0 ? 0 : first;
}

int ListReveal(const ListGeometry &list, int first, int count, int row)
{
    if (row < first) first = row;
    if (row >= first + list.visible) first = row - list.visible + 1;
    return ListClampFirst(list, first, count);
}

int ListRowAt(const ListGeometry &list, int first, int count, int x, int y)
{
    if (!list.rows.contains(x, y)) return -1;
    const int row = first + (y - list.rows.y) / kRowHeight;
    return row < count && (y - list.rows.y) / kRowHeight < list.visible ? row : -1;
}

int ListScrollPartAt(const ListGeometry &list, int first, int count, int x, int y)
{
    if (!list.scroll.contains(x, y) || count <= list.visible) return 0;
    if (y < list.scroll.y + 16) return -1;
    if (y >= list.scroll.y + list.scroll.h - 16) return 1;
    const int track_y = list.scroll.y + 16, span = list.scroll.h - 32 - 17;
    const int max_first = count - list.visible;
    const int thumb_y = track_y + (span > 0 ? (first * span + max_first / 2) / max_first : 0);
    return y < thumb_y ? -2 : (y >= thumb_y + 17 ? 2 : 0);
}

template <typename DrawRow>
static void paintList(const ListGeometry &list, int count, int first, int selected,
                      int pressed_scroll, DrawRow drawRow)
{
    Rect inner = ListFrame(list.frame);
    (void)inner;
    for (int i = 0; i < list.visible && first + i < count; ++i) {
        const Rect row = {list.rows.x, list.rows.y + i * kRowHeight, list.rows.w, kRowHeight};
        const bool is_selected = first + i == selected;
        if (is_selected) Fill(row, kBlack);
        drawRow(first + i, row, is_selected);
    }
    ScrollBar(list.scroll, count, list.visible, first, pressed_scroll);
}

/* ---- Settings ---------------------------------------------------------- */

static_assert(kRamLast - kRamFirst + 1 == kBootRamChoiceCount, "one Memory radio per RAM choice");
static const char *const kOptionLabels[3] = {"Sound", "Boot from CD", "Rotate 180"};

SettingsLayout LayoutSettings(int width, int height, bool usb_available)
{
    SettingsLayout L;
    for (Rect &r : L.controls) r = {0, 0, 0, 0};
    L.window = mainWindow(width, height);
    const Rect c = WindowContent(L.window);
    const int left = c.x + kMargin, usable = c.w - 2 * kMargin;
    const int rows = 4 + (height > 360 ? (height - 360) / kRowHeight : 0);
    const int column_w = (usable - 2 * kMargin) / 3;

    int y = c.y + 8;
    for (int i = 0; i < 3; ++i) {
        const int x = left + i * (column_w + kMargin);
        L.column_label[i] = {x, y, column_w, 16};
        L.lists[i] = ListAt({x, y + 18, column_w, rows * kRowHeight + 2});
        L.controls[kDiskList + i] = L.lists[i].frame;
    }
    y = L.lists[0].frame.bottom() + kMargin;

    L.memory_box = {left, y, 290, 50};
    L.options_box = {left + 300, y, usable - 300, 50};
    for (int i = 0; i < kBootRamChoiceCount; ++i)
        L.controls[kRamFirst + i] = {left + 14 + i * 68 - 4, y + 14, 66, 28};
    int total = 0;
    for (const char *label : kOptionLabels) total += 18 + TextWidth(label);
    const int gap = (L.options_box.w - 28 - total) / 2;
    int x = L.options_box.x + 14;
    for (int i = 0; i < 3; ++i) {
        const int w = 18 + TextWidth(kOptionLabels[i]);
        L.controls[kSoundBox + i] = {x - 4, y + 14, w + 8, 28};
        x += w + gap;
    }
    y += 50 + 10;

    L.network_box = {left, y, usable, 56};
    L.wifi_icon_area = {left + 14, y + 20, 16, 12};

    L.controls[kWiFiButton] = buttonRowRect(c, left, 96);
    if (usb_available) L.controls[kUsbButton] = buttonRowRect(c, left + 108, 108);
    L.controls[kStartButton] = buttonRowRect(c, left + usable - 104, 104);
    return L;
}

void PaintSettings(const SettingsLayout &L, const SettingsView &v)
{
    backdrop(v.clock);
    Window(L.window, "Startup Settings");

    for (int i = 0; i < 3; ++i) {
        const MediaColumn &col = v.columns[i];
        TextIn(L.column_label[i], col.title, kBlack);
        const int count = (int)col.paths->size() + (col.include_none ? 1 : 0);
        paintList(L.lists[i], count, col.first, col.selected, col.pressed_scroll,
                  [&](int row, const Rect &r, bool selected) {
            const uint8_t fg = selected ? kWhite : kBlack;
            if (col.include_none && row == 0) {
                TextIn({r.x + 22, r.y, r.w - 26, r.h}, "None", selected ? kWhite : kGray73);
                return;
            }
            const std::string &path = (*col.paths)[row - (col.include_none ? 1 : 0)];
            DrawIcon(col.icon, r.x + 3, r.y + 3, selected);
            TextIn({r.x + 22, r.y, r.w - 26, r.h}, displayName(path), fg);
        });
    }

    GroupBox(L.memory_box, "Memory");
    for (int i = 0; i < kBootRamChoiceCount; ++i) {
        const Rect &hit = L.controls[kRamFirst + i];
        char label[8];
        snprintf(label, sizeof(label), "%d MB", kBootRamChoicesMB[i]);
        LabeledRadio(hit.x + 4, hit.y + 6, label, v.ram_mb == kBootRamChoicesMB[i],
                     v.pressed == kRamFirst + i);
    }
    GroupBox(L.options_box, "Options");
    const bool option_values[3] = {v.sound, v.boot_from_cd, v.rotate180};
    for (int i = 0; i < 3; ++i) {
        const Rect &hit = L.controls[kSoundBox + i];
        LabeledCheckbox(hit.x + 4, hit.y + 6, kOptionLabels[i], option_values[i],
                        v.pressed == kSoundBox + i);
    }

    GroupBox(L.network_box, "Network");
    SignalBars(L.wifi_icon_area.x, L.wifi_icon_area.y, v.wifi_bars);
    const int text_x = L.wifi_icon_area.x + 26;
    const int text_w = L.network_box.right() - 14 - text_x;
    TextIn({text_x, L.network_box.y + 18, text_w, 16}, v.wifi_status, kBlack);
    TextIn({text_x, L.network_box.y + 35, text_w, 16}, v.wifi_detail, kGray73);

    Button(L.controls[kWiFiButton], "Wi-Fi...",
           v.pressed == kWiFiButton ? kButtonPressed : kButtonNormal);
    if (v.usb_available)
        Button(L.controls[kUsbButton], "USB Disk...",
               v.pressed == kUsbButton ? kButtonPressed : kButtonNormal);
    Button(L.controls[kStartButton], v.needs_setup ? "Set Up..." : "Start Up",
           v.pressed == kStartButton ? kButtonPressed : kButtonNormal, true);
    ScreenCorners();
}

int SettingsControlAt(const SettingsLayout &L, int x, int y)
{
    for (int i = 0; i < kSettingsControlCount; ++i) {
        // Buttons accept touches on their default ring and a little beyond.
        Rect r = L.controls[i];
        if (i >= kWiFiButton) r = r.inset(-4);
        if (r.w > 0 && r.contains(x, y)) return i;
    }
    return kNoControl;
}

/* ---- SD card setup ----------------------------------------------------- */

StarterLayout LayoutStarter(int width, int height)
{
    StarterLayout L = {};
    const int w = width > 484 ? 460 : width - 24, h = 248;
    L.window = {(width - w)/2, (height + kMenuBarHeight - h)/2, w, h};
    const Rect c = WindowContent(L.window);
    const int x = c.x + kMargin, y = c.y + kMargin, inner = c.w - 2*kMargin;
    L.body = {x, y, inner, 38};
    L.controls[kStarter8] = {x, y + 37, 124, 26};
    L.controls[kStarter7] = {x + 136, y + 37, 110, 26};
    L.network = {x, y + 70, inner, 16};
    L.status = {x, y + 95, inner, 44};
    L.progress = {x, y + 145, inner, 13};
    L.controls[kStarterSettings] = buttonRowRect(c, x, 84);
    L.controls[kStarterWiFi] = buttonRowRect(c, x + 96, 78);
    L.controls[kStarterAction] = buttonRowRect(c, x + inner - 108, 108);
    return L;
}

int StarterControlAt(const StarterLayout &L, int x, int y)
{
    for (int i = 0; i < kStarterCount; ++i)
        if (L.controls[i].contains(x, y)) return i;
    return kNoControl;
}

void PaintStarter(const StarterLayout &L, const StarterView &v)
{
    backdrop(v.clock);
    Window(L.window, "Set Up SD Card");
    WrapText(L.body, "Download the Macintosh ROM and a startup disk to this card. Allow 1.2 GiB of free space.", kBlack);
    const Rect &eight = L.controls[kStarter8], &seven = L.controls[kStarter7];
    LabeledRadio(eight.x, eight.y+7, "Mac OS 8.1", !v.system7, v.pressed == kStarter8);
    LabeledRadio(seven.x, seven.y+7, "System 7", v.system7, v.pressed == kStarter7);
    TextIn(L.network, v.network, kGray63);
    WrapText(L.status, v.status, kBlack);
    if (v.working && v.percent >= 0) {
        FrameRect(L.progress, kBlack);
        const Rect r = L.progress.inset(1);
        Fill(r, kWhite);
        const int percent = v.percent < 100 ? v.percent : 100;
        Fill({r.x, r.y, r.w * percent / 100, r.h}, kAccent);
    }
    const char *labels[] = {"Settings...", "Wi-Fi...", v.action};
    for (int i = kStarterSettings; i <= kStarterAction; ++i) {
        const bool disabled = i == kStarterAction ? !v.action_enabled : v.working;
        Button(L.controls[i], labels[i-kStarterSettings], disabled ? kButtonDisabled :
               v.pressed == i ? kButtonPressed : kButtonNormal, i == kStarterAction && !v.working);
    }
    ScreenCorners();
}

/* ---- Wi-Fi ------------------------------------------------------------- */

WiFiLayout LayoutWiFi(int width, int height)
{
    WiFiLayout L;
    L.window = mainWindow(width, height);
    const Rect c = WindowContent(L.window);
    const int left = c.x + kMargin, usable = c.w - 2 * kMargin;
    const int rows = 6 + (height > 360 ? (height - 360) / kRowHeight : 0);
    L.list = ListAt({left, c.y + 26, usable, rows * kRowHeight + 2});
    L.controls[kNetworkList] = L.list.frame;
    int y = L.list.frame.bottom() + 9;
    L.status_line = {left, y, usable, 16};
    y += 24;
    L.password_label = {left, y, 80, 22};
    L.controls[kPasswordField] = {left + 84, y, 300, 22};
    L.controls[kScanButton] = buttonRowRect(c, left, 84);
    L.controls[kJoinButton] = buttonRowRect(c, left + usable - 90, 90);
    L.controls[kBackButton] = buttonRowRect(c, left + usable - 90 - 14 - 84, 84);
    return L;
}

void PaintWiFi(const WiFiLayout &L, const WiFiView &v)
{
    backdrop(v.clock);
    Window(L.window, "Wi-Fi Networks");
    const Rect label = {L.list.frame.x, L.list.frame.y - 18, L.list.frame.w, 16};
    TextIn(label, "Choose a network:", kBlack);
    if (v.scanning) TextIn(label, "Scanning...", kGray73, kRight);

    const int count = (int)v.networks->size();
    paintList(L.list, count, v.first, v.selected, v.pressed_scroll,
              [&](int row, const Rect &r, bool selected) {
        const NetworkRow &net = (*v.networks)[row];
        if (net.secure) DrawIcon(kIconLock, r.x + 6, r.y + 5, selected);
        TextIn({r.x + 22, r.y, r.w - 52, r.h}, net.ssid.c_str(), selected ? kWhite : kBlack);
        SignalBars(r.right() - 24, r.y + 5, net.bars, selected);
    });
    if (count == 0 && !v.scanning)
        TextIn(L.list.rows, "No networks found. Click Scan to look again.", kGray73, kCenter);

    TextIn(L.status_line, v.status, kBlack);
    TextIn(L.password_label, "Password:", kBlack);
    char bullets[40];
    int n = v.password_length < 32 ? v.password_length : 32;
    for (int i = 0; i < n; ++i) bullets[i] = '*';
    bullets[n] = 0;
    TextField(L.controls[kPasswordField], n ? bullets : "Tap to enter the password", n == 0);

    Button(L.controls[kScanButton], "Scan",
           v.scanning ? kButtonDisabled : v.pressed == kScanButton ? kButtonPressed : kButtonNormal);
    Button(L.controls[kBackButton], "Back", v.pressed == kBackButton ? kButtonPressed : kButtonNormal);
    Button(L.controls[kJoinButton], "Join",
           !v.can_join ? kButtonDisabled : v.pressed == kJoinButton ? kButtonPressed : kButtonNormal,
           true);
    ScreenCorners();
}

int WiFiControlAt(const WiFiLayout &L, int x, int y)
{
    for (int i = 0; i < kWiFiControlCount; ++i) {
        Rect r = L.controls[i];
        if (i >= kScanButton) r = r.inset(-4);
        if (r.contains(x, y)) return i;
    }
    return kWiFiNoControl;
}

/* ---- Password keyboard ------------------------------------------------- */

KeyboardLayout LayoutKeyboard(int width, int height)
{
    KeyboardLayout L;
    L.window = mainWindow(width, height);
    const Rect c = WindowContent(L.window);
    const int left = c.x + kMargin, usable = c.w - 2 * kMargin;
    L.prompt = {left, c.y + 8, usable, 16};
    L.field = {left, c.y + 28, usable, 24};
    L.ok = buttonRowRect(c, left + usable - 90, 90);
    L.cancel = buttonRowRect(c, left + usable - 90 - 14 - 90, 90);

    const int gap = 4;
    const int unit = (usable - 9 * gap) / 10;
    const int x0 = left + (usable - (10 * unit + 9 * gap)) / 2;
    const int top = L.field.bottom() + 11, bottom = L.ok.y - 10;
    const int row_h = (bottom - top - 4 * 5) / 5;
    struct Spec { int normal, shifted, units; const char *label; };
    static const char *const rows[4][2] = {
        {"1234567890", "!@#$%^&*()"},
        {"qwertyuiop", "QWERTYUIOP"},
        {"asdfghjkl-", "ASDFGHJKL_"},
        {"zxcvbnm,./", "ZXCVBNM<>?"},
    };
    for (int r = 0; r < 4; ++r)
        for (int i = 0; i < 10; ++i) {
            KeyboardLayout::Key key;
            key.r = {x0 + i * (unit + gap), top + r * (row_h + 5), unit, row_h};
            key.normal = rows[r][0][i];
            key.shifted = rows[r][1][i];
            key.label = nullptr;
            L.keys.push_back(key);
        }
    static const Spec last[] = {
        {kKeyShift, kKeyShift, 2, "shift"}, {';', ':', 1, nullptr}, {'\'', '"', 1, nullptr},
        {' ', ' ', 3, ""}, {'=', '+', 1, nullptr}, {kKeyDelete, kKeyDelete, 2, "delete"},
    };
    int x = x0;
    for (const Spec &s : last) {
        KeyboardLayout::Key key;
        key.r = {x, top + 4 * (row_h + 5), s.units * unit + (s.units - 1) * gap, row_h};
        key.normal = s.normal;
        key.shifted = s.shifted;
        key.label = s.label;
        L.keys.push_back(key);
        x += key.r.w + gap;
    }
    return L;
}

int KeyCharacter(const KeyboardLayout::Key &key, bool shift)
{
    return shift ? key.shifted : key.normal;
}

void PaintKeyboard(const KeyboardLayout &L, const KeyboardView &v)
{
    backdrop(v.clock);
    Window(L.window, "Wi-Fi Password");
    char prompt[96];
    if (v.network && v.network[0])
        snprintf(prompt, sizeof(prompt), "Enter the password for \"%s\":", v.network);
    else
        snprintf(prompt, sizeof(prompt), "Enter the Wi-Fi password:");
    TextIn(L.prompt, prompt, kBlack);
    TextField(L.field, v.text, false, true);
    for (const KeyboardLayout::Key &key : L.keys) {
        const int code = KeyCharacter(key, v.shift);
        char label[2] = {(char)code, 0};
        const bool pressed = v.pressed_key != kKeyNone && v.pressed_key == key.normal;
        KeyCap(key.r, key.label ? key.label : label, pressed,
               key.normal == kKeyShift && v.shift);
    }
    Button(L.cancel, "Cancel", v.pressed_key == kKeyCancel ? kButtonPressed : kButtonNormal);
    Button(L.ok, "OK", v.pressed_key == kKeyOK ? kButtonPressed : kButtonNormal, true);
    ScreenCorners();
}

int KeyAt(const KeyboardLayout &L, int x, int y)
{
    for (const KeyboardLayout::Key &key : L.keys)
        if (key.r.inset(-2).contains(x, y)) return key.normal;
    if (L.ok.inset(-4).contains(x, y)) return kKeyOK;
    if (L.cancel.inset(-4).contains(x, y)) return kKeyCancel;
    return kKeyNone;
}

/* ---- Alerts ------------------------------------------------------------ */

static constexpr int kAlertWidth = 440;
static constexpr int kAlertTextInset = 62;

AlertLayout LayoutAlert(int width, int height, const AlertView &v)
{
    const int w = kAlertWidth < width - 40 ? kAlertWidth : width - 40;
    const int text_w = w - 12 - kAlertTextInset - 16;
    int text_h = 0;
    if (v.heading) text_h += WrapText({0, 0, text_w, 0}, v.heading, 0, false) + 6;
    if (v.body) text_h += WrapText({0, 0, text_w, 0}, v.body, 0, false);
    if (v.note) text_h += 8 + WrapText({0, 0, text_w, 0}, v.note, 0, false);
    if (text_h < 36) text_h = 36;
    int content_h = 14 + text_h + 10;
    if (v.status) content_h += 22;
    content_h += v.button ? kButtonHeight + 16 : 4;
    const int h = content_h + kTitleBarHeight + 7;
    const int top = v.clock ? kMenuBarHeight : 0;
    AlertLayout L;
    L.window = {(width - w) / 2, top + (height - top - h) / 3, w, h};
    const Rect c = WindowContent(L.window, kAlertWindow);
    L.button = v.button ? Rect{c.right() - 16 - 92, c.bottom() - 14 - kButtonHeight, 92, kButtonHeight}
                        : Rect{0, 0, 0, 0};
    return L;
}

void PaintAlert(const AlertLayout &L, const AlertView &v)
{
    if (v.dark_backdrop) Fill({0, 0, Width(), Height()}, kBlack);
    else backdrop(v.clock);
    const Rect c = Window(L.window, nullptr, kAlertWindow);
    DrawIcon(v.icon, c.x + 16, c.y + 12);
    Rect text = {c.x + kAlertTextInset, c.y + 12, c.w - kAlertTextInset - 16, 0};
    if (v.heading) text.y += WrapText(text, v.heading, kBlack) + 6;
    if (v.body) text.y += WrapText(text, v.body, kBlack);
    if (v.note) text.y += 8 + WrapText({text.x, text.y + 8, text.w, 0}, v.note, kGray73);
    if (v.status) {
        const int y = (v.button ? L.button.y - 26 : c.bottom() - 26);
        TextIn({text.x, y, text.w, 16}, v.status, kBlack);
    }
    if (v.button)
        Button(L.button, v.button, v.button_pressed ? kButtonPressed : kButtonNormal, true);
    if (!v.dark_backdrop) ScreenCorners();
}

/* ---- Splash ------------------------------------------------------------ */

void PaintSplash(void)
{
    SplashBackground();
#if HAPPY_MAC_HAS_MASK
    Image565((Width() - HAPPY_MAC_W) / 2, (Height() - HAPPY_MAC_H) / 2, HAPPY_MAC_W,
             HAPPY_MAC_H, HAPPY_MAC_PIXELS, HAPPY_MAC_MASK, HAPPY_MAC_MASK_ROW_BYTES);
#else
    Image565((Width() - HAPPY_MAC_W) / 2, (Height() - HAPPY_MAC_H) / 2, HAPPY_MAC_W,
             HAPPY_MAC_H, HAPPY_MAC_PIXELS);
#endif
}

} // namespace PrebootScreens
