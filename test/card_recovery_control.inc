// Exercise the actual preboot controller with a replaceable card and real
// touch layout. The simulated storage interface records each erase attempt.
#include <atomic>
#include <functional>
#include "board_sd_format.h"
#include "card_erase_confirmation.h"

namespace CardRecoveryTest {
namespace Screens = PrebootScreens;
using String = std::string;
struct UiTouch { int x, y; bool down, pressed, released, injected; };
static const char *control_screen;
static std::function<void()> step;
static UiTouch touch = {};
static bool have_touch, mount_ok, swap_on_format, card_present, allow_format;
static unsigned tick, erases, checks;
static constexpr char id[] = "0123456789abcdef0123456789abcdef";
struct Restart {};
static unsigned millis() { return tick; }
static void delay(unsigned ms) { tick += ms; assert(tick < 20000); }
static unsigned esp_random() { return 0xa1b2c3d4; }
static void esp_restart() { throw Restart(); }
static bool BoardSD_Init() { return mount_ok; }
static const char *testInspectCard(BoardSdCardInfo *out) {
    ++checks;
    if (!card_present) return "No card.";
    out->capacity_bytes = 16ull << 30;
    out->format_supported = allow_format;
    strcpy(out->layout, "exFAT");
    strcpy(out->identity, "Test SD 1234");
    strcpy(out->token, id);
    return nullptr;
}
static const char *testFormatCard(const char *token) {
    assert(!strcmp(token,id));
    if (swap_on_format) return "Card changed.";
    ++erases;
    return nullptr;
}
static String controlHex(const char *s) { return s; }
static void BootGUI_ControlPoll() { step(); }
static bool pollUiTouch(UiTouch *out) {
    if (!have_touch) { *out = {}; return true; }
    *out = touch;
    have_touch = false;
    return true;
}
static void vTaskDelete(void *) {}
static constexpr int pdPASS = 1;
static int xTaskCreatePinnedToCore(void (*fn)(void *), const char *, int, void *, int, void *, int) {
    fn(nullptr); return pdPASS;
}
#define BoardSD_InspectCard testInspectCard
#define BoardSD_FormatCard testFormatCard
#include "../src/basilisk/card_recovery_ui.inc"
#undef BoardSD_InspectCard
#undef BoardSD_FormatCard

static void tapPart(Screens::CardControl which, bool down, bool injected = false)
{
    const auto r = Screens::LayoutCard(UI::Width(),UI::Height()).controls[which];
    touch = {r.x+r.w/2,r.y+r.h/2,down,down,!down,injected};
    have_touch = true;
}

static void run()
{
#if defined(BOARD_M5STACK_TAB5)
    int stage = 0;
    tick = 0;
    step = [&] {
        if (checks == 1 && stage == 0) {
            assert(!card_examined && !strcmp(card_message, "No card."));
            assert(!armCard(id));
            card_present = true;
            assert(cardCommand("RETRY") == "OK BOOT CARD RETRY");
            ++stage;
        } else if (checks == 2 && stage == 1) {
            assert(card_examined && !armCard(id)); // unsupported geometry
            mount_ok = true;
            assert(cardCommand("RETRY") == "OK BOOT CARD RETRY");
            ++stage;
        }
    };
    runCardScreen(false);
    assert(stage == 2 && !erases);
    mount_ok = false; allow_format = true; tick = 0; stage = 0;
    step = [&] {
        if (!card_examined || card_page == Screens::CardPage::Formatting || card_page == Screens::CardPage::Complete) return;
        switch (stage++) {
            case 0: assert(cardCommand("ERASE 0123456789abcdef0123456789abcdef a1b2c3d4") ==
                           "ERR BOOT card_confirmation_required"); break;
            case 1: assert(cardCommand("ARM wrong") == "ERR BOOT invalid_card_action"); break;
            case 2: assert(cardCommand("ARM 0123456789abcdef0123456789abcdef").find("a1b2c3d4") != String::npos); break;
            case 3: assert(card_blocks_injected_press.load()); assert(cardCommand("CANCEL") == "OK BOOT CARD CANCEL"); break;
            case 4: tapPart(Screens::kCardReview,true); break;
            case 5: tapPart(Screens::kCardReview,false); break;
            case 6: assert(card_page == Screens::CardPage::Confirm); break; // neutral sample after entering
            case 7: tapPart(Screens::kCardErase,true,true); break;
            case 8: tapPart(Screens::kCardErase,false,true); break;
            case 9: assert(card_page == Screens::CardPage::Confirm && !erases); tapPart(Screens::kCardErase,true); break;
            case 10: tapPart(Screens::kCardErase,false); break;
        }
    };
    try { runCardScreen(true); assert(false); } catch (const Restart &) {}
    assert(erases == 1 && card_page == Screens::CardPage::Complete);

    tick = 0; stage = 0; swap_on_format = true;
    step = [&] {
        if (card_page == Screens::CardPage::Inspect && card_examined && stage == 0) {
            assert(armCard(id));
            assert(!confirmCard(id,"wrong"));
            assert(!confirmCard(id,"a1b2c3d4")); // failed challenge was consumed
            assert(armCard(id));
            assert(cardCommand("ERASE 0123456789abcdef0123456789abcdef a1b2c3d4") == "OK BOOT CARD ERASE");
            ++stage;
        } else if (stage == 1 && card_page == Screens::CardPage::Inspect && !card_examined) {
            assert(!strcmp(card_message, "Card changed."));
            mount_ok = true;
            assert(cardCommand("RETRY") == "OK BOOT CARD RETRY");
            ++stage;
        }
    };
    runCardScreen(false);
    assert(erases == 1 && stage == 2 && !card_open);
#endif
}
}
