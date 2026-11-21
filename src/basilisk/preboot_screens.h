/*
 *  preboot_screens.h - layout and painting of each preboot screen
 *
 *  BasiliskII ESP32 Port
 *
 *  Pure functions of a view model: no WiFi, SD or touch code lives here, so
 *  the same layout drives both painting and hit testing in boot_gui.cpp and
 *  the screens can be rendered on a host for tests.
 */
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "preboot_ui.h"

namespace PrebootScreens {

using PrebootUI::Rect;

/* ---- Scrolling lists --------------------------------------------------- */

constexpr int kRowHeight = 22;

struct ListGeometry {
    Rect frame;       /* black list frame, scroll bar inside on the right */
    Rect rows;        /* white row area */
    Rect scroll;      /* scroll bar */
    int visible;      /* whole rows that fit */
};
ListGeometry ListAt(const Rect &frame);
/* Row index under a point, or -1. */
int ListRowAt(const ListGeometry &list, int first, int count, int x, int y);
/* -1/+1 for the up/down arrow, -2/+2 for the track above/below the thumb,
 * 0 elsewhere. Only meaningful when the list scrolls. */
int ListScrollPartAt(const ListGeometry &list, int first, int count, int x, int y);
/* Clamp `first` so that `row` is visible. */
int ListReveal(const ListGeometry &list, int first, int count, int row);
int ListClampFirst(const ListGeometry &list, int first, int count);

/* ---- Settings ---------------------------------------------------------- */

enum SettingsControl {
    kNoControl = -1,
    kDiskList, kCDList, kFolderList,
    kRamFirst, kRamLast = kRamFirst + 3,   /* one radio per kBootRamChoicesMB entry */
    kSoundBox, kBootCDBox, kRotateBox,
    kWiFiButton, kUsbButton, kStartButton,
    kSettingsControlCount
};

struct MediaColumn {
    const char *title;
    const std::vector<std::string> *paths;   /* full SD paths */
    bool include_none;                        /* row 0 is "None" */
    int selected;                             /* row index, including "None" */
    int first;                                /* first visible row */
    int pressed_scroll;                       /* -1 up arrow, +1 down, 0 none */
    PrebootUI::Icon icon;
};

struct SettingsView {
    const char *clock;
    MediaColumn columns[3];
    int ram_mb;
    bool sound, boot_from_cd, rotate180;
    bool usb_available, usb_waits_for_update;
    bool needs_setup;
    int wifi_bars;                 /* 0..4 */
    const char *wifi_status;       /* "Connected", "Not connected", ... */
    const char *wifi_detail;       /* SSID and address, or a hint */
    int pressed;                   /* SettingsControl being touched */
};

struct SettingsLayout {
    Rect window;
    Rect column_label[3];
    ListGeometry lists[3];
    Rect memory_box, options_box, network_box;
    Rect controls[kSettingsControlCount];   /* touch rectangles */
    Rect wifi_icon_area;
};
SettingsLayout LayoutSettings(int width, int height, bool usb_available);
void PaintSettings(const SettingsLayout &layout, const SettingsView &view);
/* Control under a point (lists included), or kNoControl. */
int SettingsControlAt(const SettingsLayout &layout, int x, int y);

/* ---- SD card setup ----------------------------------------------------- */

enum StarterControl { kStarter8, kStarter7, kStarterSettings, kStarterWiFi, kStarterAction, kStarterCount };

struct StarterLayout {
    Rect window, body, status, progress, network;
    Rect controls[kStarterCount];
};
struct StarterView {
    const char *clock, *status, *network, *action;
    int pressed, percent;   /* -1 if total size is unknown */
    bool system7, working, action_enabled;
};
StarterLayout LayoutStarter(int width, int height);
void PaintStarter(const StarterLayout &layout, const StarterView &view);
int StarterControlAt(const StarterLayout &layout, int x, int y);

/* ---- Wi-Fi ------------------------------------------------------------- */

enum WiFiControl {
    kWiFiNoControl = -1,
    kNetworkList, kPasswordField, kScanButton, kBackButton, kJoinButton,
    kWiFiControlCount
};

struct NetworkRow {
    std::string ssid;
    int bars;
    bool secure;
};

struct WiFiView {
    const char *clock;
    const std::vector<NetworkRow> *networks;
    int selected;                /* -1 none */
    int first;
    int pressed_scroll;
    bool scanning;
    const char *status;          /* "Connected to Home (192.168.1.20)" */
    int password_length;         /* shown as bullets */
    bool can_join;
    int pressed;                 /* WiFiControl being touched */
};

struct WiFiLayout {
    Rect window;
    ListGeometry list;
    Rect status_line, password_label;
    Rect controls[kWiFiControlCount];
};
WiFiLayout LayoutWiFi(int width, int height);
void PaintWiFi(const WiFiLayout &layout, const WiFiView &view);
int WiFiControlAt(const WiFiLayout &layout, int x, int y);

/* ---- Password keyboard ------------------------------------------------- */

enum KeyCode {
    kKeyNone = -1,
    kKeyShift = -2, kKeyDelete = -3, kKeyOK = -4, kKeyCancel = -5,
};

struct KeyboardView {
    const char *clock;
    const char *network;         /* SSID being joined */
    const char *text;            /* typed password, shown so it can be checked */
    bool shift;
    int pressed_key;             /* a KeyCode or a character, kKeyNone */
};

struct KeyboardLayout {
    Rect window, prompt, field;
    struct Key { Rect r; int normal; int shifted; const char *label; };
    std::vector<Key> keys;
    Rect cancel, ok;
};
KeyboardLayout LayoutKeyboard(int width, int height);
void PaintKeyboard(const KeyboardLayout &layout, const KeyboardView &view);
/* Key code under a point (Cancel/OK buttons included), or kKeyNone. */
int KeyAt(const KeyboardLayout &layout, int x, int y);
/* Character a key produces with the current shift state. */
int KeyCharacter(const KeyboardLayout::Key &key, bool shift);

/* ---- Alerts ------------------------------------------------------------ */

struct AlertView {
    const char *clock;           /* nullptr: no menu bar (error / power-off) */
    PrebootUI::Icon icon;
    const char *heading;         /* first line, may be nullptr */
    const char *body;            /* word-wrapped paragraphs separated by '\n' */
    const char *note;            /* gray paragraph, may be nullptr */
    const char *status;          /* status line above the button, may be nullptr */
    const char *button;          /* default button label, nullptr for none */
    bool button_pressed;
    bool dark_backdrop;          /* black screen instead of the desktop */
};
struct AlertLayout { Rect window, button; };
AlertLayout LayoutAlert(int width, int height, const AlertView &view);
void PaintAlert(const AlertLayout &layout, const AlertView &view);

/* ---- Splash ------------------------------------------------------------ */

void PaintSplash(void);

/* Word-wrap `text` into `r`; returns the height used. */
int WrapText(const Rect &r, const char *text, uint8_t color, bool draw = true);

} // namespace PrebootScreens
