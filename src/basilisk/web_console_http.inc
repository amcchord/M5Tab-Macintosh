// The browser and the USB screenshot fallback share one Wi-Fi listener.
// This handler also runs against an in-memory client in the native test.
static void httpEmpty(WiFiClient &client, const char *status)
{
    client.printf("HTTP/1.1 %s\r\nContent-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", status);
}

// The total byte and time budget covers the whole request, including headers.
static bool httpLine(WiFiClient &client, char *line, size_t cap, size_t &remaining, uint32_t deadline)
{
    size_t pos = 0;
    while (remaining && (int32_t)(millis() - deadline) < 0) {
        const int c = client.read();
        if (c < 0) {
            if (!client.connected()) return false;
            vTaskDelay(1);
            continue;
        }
        --remaining;
        if (c == '\n') { line[pos] = 0; return true; }
        if (c == '\r') continue;
        if (pos + 1 >= cap || c == 0) return false;
        line[pos++] = (char)c;
    }
    return false;
}

static void handleWebRequest(WiFiClient &client, const char *method, const char *path,
                             size_t &remaining, uint32_t deadline)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_network_state_lock);
    const bool active = s_web_session.active(now);
    portEXIT_CRITICAL(&s_network_state_lock);
    if (!active) { httpEmpty(client, "403 Forbidden"); return; }
    char token[33] = {};
    int text_length = -1;
    const bool text_request = !strcmp(method, "POST") && !strcmp(path, "/desk/text");
    char line[768];
    for (;;) {
        if (!httpLine(client, line, sizeof(line), remaining, deadline)) { httpEmpty(client, "400 Bad Request"); return; }
        if (!line[0]) break;
        static const char field[] = "X-M5Tab-Session:";
        if (!strncasecmp(line, field, sizeof(field) - 1)) {
            const char *value = line + sizeof(field) - 1;
            while (*value == ' ' || *value == '\t') ++value;
            if (token[0] || strlen(value) != sizeof(token) - 1) { httpEmpty(client, "403 Forbidden"); return; }
            memcpy(token, value, sizeof(token));
        } else if (text_request && !strncasecmp(line, "Content-Length:", 15)) {
            const char *value = line + 15;
            while (*value == ' ' || *value == '\t') ++value;
            char trailing;
            unsigned length;
            if (text_length >= 0 || sscanf(value, "%2u%c", &length, &trailing) != 1 || !length || length > 64) {
                httpEmpty(client, "400 Bad Request");
                return;
            }
            text_length = length;
        } else if (text_request && !strncasecmp(line, "Transfer-Encoding:", 18)) {
            httpEmpty(client, "400 Bad Request");
            return;
        }
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/desk")) {
        client.printf("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
                      "Referrer-Policy: no-referrer\r\nX-Frame-Options: DENY\r\n"
                      "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                      "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n", (unsigned)(sizeof(kWebDashboard)-1));
        writeHttpBody(client, (const uint8_t *)kWebDashboard, sizeof(kWebDashboard)-1);
        return;
    }
    portENTER_CRITICAL(&s_network_state_lock);
    const WebSession::Access mode = s_web_session.authenticate(token, esp_timer_get_time());
    const uint64_t count = s_cpu_sample_count, ms = s_cpu_sample_ms;
    portEXIT_CRITICAL(&s_network_state_lock);
    if (mode == WebSession::Closed || !s_emulator_ready) { httpEmpty(client, "403 Forbidden"); return; }
    if (!strcmp(method, "GET") && !strcmp(path, "/desk/frame")) {
        sendHttpFrame(client);
        return;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/desk/status")) {
        char json[320];
        const int len = snprintf(json, sizeof(json),
            "{\"board\":\"%s\",\"version\":\"%s\",\"control\":%s,\"ota_slots\":%u,\"uptime_sec\":%llu,"
            "\"internal\":%u,\"psram\":%u,\"sample_ms\":%llu,\"instructions\":%llu}",
            BOARD_NAME, M5TAB_FIRMWARE_VERSION, mode == WebSession::Control ? "true" : "false",
            FirmwareUpdateHasDualSlots() ? 2u : 1u, (unsigned long long)(now / 1000000), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned long long)ms, (unsigned long long)count);
        if (len < 0 || (size_t)len >= sizeof(json)) { httpEmpty(client, "500 Internal Server Error"); return; }
        client.printf("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n%s", len, json);
        return;
    }
    if (strcmp(method, "POST")) { httpEmpty(client, "404 Not Found"); return; }
    if (mode != WebSession::Control) { httpEmpty(client, "403 Forbidden"); return; }
    if (text_request) {
        if (text_length < 1) { httpEmpty(client, "400 Bad Request"); return; }
        AutomationKeyStroke strokes[64];
        for (int i = 0; i < text_length;) {
            if ((int32_t)(millis() - deadline) >= 0) { httpEmpty(client, "408 Request Timeout"); return; }
            const int c = client.read();
            if (c < 0) {
                if (!client.connected()) return;
                vTaskDelay(1);
                continue;
            }
            if ((c < 32 && c != '\n' && c != '\t') || !asciiToKeyStroke((uint8_t)c, &strokes[i])) {
                httpEmpty(client, "400 Bad Request");
                return;
            }
            ++i;
        }
        // Validate the whole body before typing. A revoked session stops between
        // complete taps; each one owns its key and modifier claims independently.
        for (int i = 0; i < text_length; ++i) {
            portENTER_CRITICAL(&s_network_state_lock);
            const bool authorized = s_web_session.authenticate(token, esp_timer_get_time()) == WebSession::Control;
            portEXIT_CRITICAL(&s_network_state_lock);
            if (!authorized || !s_emulator_ready) { httpEmpty(client, "403 Forbidden"); return; }
            const AutomationKeyStroke stroke = strokes[i];
            if (stroke.shift) InputKeyDown(0x38);
            InputKeyDown(stroke.code);
            vTaskDelay(pdMS_TO_TICKS(AUTOMATION_TYPE_KEY_HOLD_MS));
            InputKeyUp(stroke.code);
            if (stroke.shift) InputKeyUp(0x38);
            vTaskDelay(pdMS_TO_TICKS(AUTOMATION_TYPE_KEY_GAP_MS));
        }
        httpEmpty(client, "204 No Content");
        return;
    }
    int x, y, key;
    char tail;
    const bool twice = !strncmp(path, "/desk/double-click/", 19);
    if (sscanf(path, twice ? "/desk/double-click/%4d/%4d%c" : "/desk/click/%4d/%4d%c", &x, &y, &tail) == 2 &&
        x >= 0 && y >= 0 && x < BOARD_MAC_SCREEN_WIDTH && y < BOARD_MAC_SCREEN_HEIGHT) {
        if (!InputAutomationTryClick(x, y, twice ? 2 : 1)) { httpEmpty(client, "409 Conflict"); return; }
    } else if (sscanf(path, "/desk/key/%3d%c", &key, &tail) == 1 && key >= 0 && key < 0x80) {
        InputKeyDown((uint8_t)key);
        vTaskDelay(pdMS_TO_TICKS(AUTOMATION_TYPE_KEY_HOLD_MS));
        InputKeyUp((uint8_t)key);
    } else { httpEmpty(client, "400 Bad Request"); return; }
    httpEmpty(client, "204 No Content");
}
