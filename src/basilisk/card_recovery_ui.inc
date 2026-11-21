// Startup Settings owns SD access while this screen is open, including during
// recovery before the first mount. Only the formatting worker touches storage
// from the moment the confirmation is consumed until it publishes completion.
static BoardSdCardInfo recovery_card = {};
static CardEraseConfirmation erase_confirmation;
static Screens::CardPage card_page = Screens::CardPage::Inspect;
static bool card_requested, card_open, card_mounted, card_leave, card_retry, card_dirty;
static bool card_examined, card_needs_release;
static std::atomic<bool> card_format_finished{false};
static std::atomic<bool> card_blocks_injected_press{false};
static char card_format_token[33], card_message[160];
static const char *card_format_error;

static void cardNotice(const char *text)
{
    snprintf(card_message, sizeof(card_message), "%s", text);
    card_dirty = true;
}

static void cardPage(Screens::CardPage page)
{
    card_page = page;
    card_needs_release = page == Screens::CardPage::Confirm;
    control_screen = page == Screens::CardPage::Confirm ? "card-confirm" :
                     page == Screens::CardPage::Formatting ? "card-format" : "card";
    card_blocks_injected_press.store(page == Screens::CardPage::Confirm);
    card_dirty = true;
}

static void inspectCard()
{
    erase_confirmation.clear();
    recovery_card = {};
#if defined(BOARD_M5STACK_TAB5)
    const char *error = BoardSD_InspectCard(&recovery_card);
    card_examined = !error;
    if (error) card_mounted = false; // inspection can fail while restoring a previous mount
    cardNotice(error ? error : card_mounted ? "This card is mounted." :
               "The firmware could not mount this card. Retry after reinserting it, or format it as FAT32.");
#else
    card_examined = false;
    cardNotice("Insert a FAT32 microSD card and choose Retry Mount.");
#endif
}

static bool armCard(const char *token)
{
    if (card_page != Screens::CardPage::Inspect || !card_examined ||
        !erase_confirmation.arm(token, recovery_card.token, recovery_card.format_supported, esp_random()))
        return false;
    cardPage(Screens::CardPage::Confirm);
    return true;
}

static void cancelCardConfirmation()
{
    erase_confirmation.clear();
    cardPage(Screens::CardPage::Inspect);
}

static void formatCardTask(void *)
{
#if defined(BOARD_M5STACK_TAB5)
    card_format_error = BoardSD_FormatCard(card_format_token);
#else
    card_format_error = "Formatting is unavailable on this board.";
#endif
    card_format_finished.store(true);
    vTaskDelete(nullptr);
}

static bool confirmCard(const char *token, const char *challenge)
{
    if (card_page != Screens::CardPage::Confirm) return false;
    if (!erase_confirmation.consume(recovery_card.token, token, challenge)) {
        cancelCardConfirmation();
        cardNotice("Confirmation did not match. Choose Format to review this card again.");
        return false;
    }
    snprintf(card_format_token, sizeof(card_format_token), "%s", recovery_card.token);
    cardPage(Screens::CardPage::Formatting);
    card_format_finished.store(false);
    card_format_error = nullptr;
    cardNotice("Formatting as FAT32. Keep the card inserted and the device powered on.");
    return true;
}

static Screens::CardView currentCardView(int pressed, char *detail, size_t detail_n,
                                         char *identity, size_t identity_n)
{
    if (card_examined) {
        snprintf(detail, detail_n, "%llu MiB microSD card   %s",
                 (unsigned long long)(recovery_card.capacity_bytes / (1024 * 1024)), recovery_card.layout);
        if (recovery_card.volume_bytes) {
            const size_t used = strlen(detail);
            snprintf(detail + used, detail_n-used, "   %llu MiB volume",
                     (unsigned long long)(recovery_card.volume_bytes / (1024 * 1024)));
        }
        snprintf(identity, identity_n, "Card ID %s", recovery_card.identity);
    } else {
        snprintf(detail, detail_n, "%s", card_mounted ? "Mounted microSD card" : "No mounted microSD card");
        identity[0] = 0;
    }
    return {card_page, detail, identity,
            card_page == Screens::CardPage::Confirm ?
                "Erase every file and partition on this card and create a new FAT32 volume? This cannot be undone." :
                card_message, card_mounted, card_examined && recovery_card.format_supported, pressed};
}

static bool cardHitRect(const char *name, UI::Rect *out)
{
    static const char *const names[] = {"back", "retry", "format", "erase"};
    char detail[80], identity[96];
    const auto view = currentCardView(Screens::kNoControl, detail, sizeof(detail), identity, sizeof(identity));
    const auto layout = Screens::LayoutCard(UI::Width(), UI::Height());
    for (int i = 0; i < Screens::kCardCount; ++i) {
        const auto &r = layout.controls[i];
        if (!strcmp(name, names[i]) &&
            Screens::CardControlAt(layout, view, r.x+r.w/2, r.y+r.h/2) == i) {
            *out = r;
            return true;
        }
    }
    return false;
}

static String cardCommand(const char *action)
{
    if (!strcmp(action, "STATUS")) {
        const char *phase = card_page == Screens::CardPage::Confirm ? "confirm" :
                            card_page == Screens::CardPage::Formatting ? "formatting" :
                            card_page == Screens::CardPage::Complete ? "done" : "inspect";
        char status[288], message[67];
        snprintf(message, sizeof(message), "%s", card_message);
        snprintf(status, sizeof(status), "OK BOOT CARD STATUS state=%s mounted=%u format=%u bytes=%llu volume_bytes=%llu token=%s "
                 "clock_khz=%lu bus_width=%u cluster_bytes=%lu partition_lba=%lu partition_type=%u",
                 phase, card_mounted, card_examined && recovery_card.format_supported,
                 (unsigned long long)recovery_card.capacity_bytes, (unsigned long long)recovery_card.volume_bytes,
                 card_examined && recovery_card.token[0] ? recovery_card.token : "-",
                 (unsigned long)recovery_card.clock_khz, recovery_card.bus_width,
                 (unsigned long)recovery_card.cluster_bytes, (unsigned long)recovery_card.partition_lba,
                 recovery_card.partition_type);
        return String(status) + " layout=" + controlHex(recovery_card.layout) + " identity=" +
               controlHex(recovery_card.identity) + " driver=" + controlHex(recovery_card.driver) +
               " message=" + controlHex(message);
    }
    if (card_page == Screens::CardPage::Formatting || card_page == Screens::CardPage::Complete)
        return "ERR BOOT card_busy";
    if (!strcmp(action, "CANCEL") && card_page == Screens::CardPage::Confirm) {
        cancelCardConfirmation();
        return "OK BOOT CARD CANCEL";
    }
    if (!strcmp(action, "RETRY") && card_page == Screens::CardPage::Inspect) {
        card_retry = true;
        return "OK BOOT CARD RETRY";
    }
    if (!strcmp(action, "BACK") && card_page == Screens::CardPage::Inspect && card_mounted) {
        card_leave = true;
        return "OK BOOT CARD BACK";
    }
    if (!strncmp(action, "ARM ", 4) && armCard(action+4))
        return String("OK BOOT CARD ARMED ") + erase_confirmation.token + " " + erase_confirmation.challenge;
    if (!strncmp(action, "ERASE ", 6)) {
        char token[33], challenge[9], extra;
        if (sscanf(action+6, "%32[0-9a-fA-F] %8[0-9a-fA-F]%c", token, challenge, &extra) == 2 &&
            confirmCard(token, challenge)) return "OK BOOT CARD ERASE";
        return "ERR BOOT card_confirmation_required";
    }
    return "ERR BOOT invalid_card_action";
}

static void runCardScreen(bool initially_mounted)
{
    card_open = true;
    card_mounted = initially_mounted;
    card_examined = card_leave = card_retry = false;
    erase_confirmation.clear();
    recovery_card = {};
    cardPage(Screens::CardPage::Inspect);
    cardNotice("Checking the microSD card...");
    const auto layout = Screens::LayoutCard(UI::Width(), UI::Height());
    bool first = true, worker_started = false;
    uint32_t restart_at = 0;
    int pressed = Screens::kNoControl;
    while (!card_leave) {
        BootGUI_ControlPoll();
        char detail[80], identity[96];
        const auto view = currentCardView(pressed, detail, sizeof(detail), identity, sizeof(identity));
        if (card_dirty) {
            Screens::PaintCard(layout, view);
            UI::Present();
            card_dirty = false;
        }
        if (first) { inspectCard(); first = false; continue; }
        if (card_page == Screens::CardPage::Formatting) {
            if (!worker_started) {
                worker_started = xTaskCreatePinnedToCore(formatCardTask, "sd_format", 8192, nullptr, 2,
                                                         nullptr, 0) == pdPASS;
                if (worker_started) card_mounted = false;
                else {
                    cancelCardConfirmation();
                    cardNotice("Insufficient memory. Choose Format to try again.");
                }
            } else if (card_format_finished.load()) {
                worker_started = false;
                if (!card_format_error) {
                    card_mounted = true;
                    snprintf(recovery_card.layout, sizeof(recovery_card.layout), "FAT32");
                    recovery_card.volume_bytes = 0; // the new partition has not been inspected by this screen
                    recovery_card.format_supported = false;
                    recovery_card.token[0] = 0;
                    cardPage(Screens::CardPage::Complete);
                    cardNotice("FAT32 card ready. Restarting into setup...");
                    restart_at = millis() + 1500;
                } else {
                    card_mounted = false;
                    card_examined = false;
                    recovery_card = {};
                    cancelCardConfirmation();
                    cardNotice(card_format_error);
                }
            }
            delay(5);
            continue;
        }
        if (card_page == Screens::CardPage::Complete) {
            if ((int32_t)(millis()-restart_at) >= 0) esp_restart();
            delay(5);
            continue;
        }
        if (card_retry) {
            card_retry = false;
            if (!card_mounted && BoardSD_Init()) {
                if (initially_mounted) esp_restart(); // reload settings after a card change
                break;
            }
            inspectCard();
            continue;
        }
        UiTouch t;
        if (pollUiTouch(&t)) {
            if (card_needs_release) {
                // Discard any queued press from the previous screen or USB.
                // Erase requires a fresh physical press and release here.
                if (!t.down && !t.pressed && !t.released && !t.injected) card_needs_release = false;
                pressed = Screens::kNoControl;
                delay(5);
                continue;
            }
            const int hit = Screens::CardControlAt(layout, view, t.x, t.y);
            const int old_pressed = pressed;
            if (t.pressed && t.down)
                pressed = (card_page == Screens::CardPage::Confirm && t.injected) ? Screens::kNoControl : hit;
            if (t.released) {
                if (pressed == hit) {
                    if (hit == Screens::kCardBack) {
                        if (card_page == Screens::CardPage::Confirm) cancelCardConfirmation();
                        else card_leave = true;
                    } else if (hit == Screens::kCardReview) armCard(recovery_card.token);
                    else if (hit == Screens::kCardRetry) card_retry = true;
                    else if (hit == Screens::kCardErase && !t.injected) {
                        char response[sizeof(erase_confirmation.challenge)];
                        memcpy(response, erase_confirmation.challenge, sizeof(response));
                        confirmCard(recovery_card.token, response);
                    }
                }
                pressed = Screens::kNoControl;
            }
            if (old_pressed != pressed) card_dirty = true;
        }
        delay(5);
    }
    cancelCardConfirmation();
    card_open = false;
    control_screen = "none";
}
