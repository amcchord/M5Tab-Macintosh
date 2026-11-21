#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../src/basilisk/web_session.h"
#include "../src/basilisk/web_dashboard.h"

static uint32_t clk;
static int64_t esp_timer_get_time() { return (int64_t)clk * 1000; }
static uint32_t millis() { return clk; }
static void (*afterDelay)() = nullptr;
static void vTaskDelay(int ms) { clk += ms; if (afterDelay) afterDelay(); }
#define portENTER_CRITICAL(x) (void)0
#define portEXIT_CRITICAL(x) (void)0
#define pdMS_TO_TICKS(x) (x)
#define AUTOMATION_TYPE_KEY_HOLD_MS 10
#define BOARD_MAC_SCREEN_WIDTH 640
#define BOARD_MAC_SCREEN_HEIGHT 360
#define BOARD_NAME "Tab5"
#define M5TAB_FIRMWARE_VERSION "test"
static bool s_emulator_ready = true;
static WebSession s_web_session;
static uint64_t s_cpu_sample_count = 10000000000ULL, s_cpu_sample_ms = 5011;
static int MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_SPIRAM = 2;
static int heap_caps_get_free_size(int) { return 2048; }
static unsigned clicks, frames;
static std::vector<uint8_t> downs, ups;
static bool InputAutomationTryClick(int x, int y, unsigned count) {
    assert(x==639 && y==359);
    clicks += count;
    return true;
}
static void InputKeyDown(uint8_t key) { downs.push_back(key); }
static void InputKeyUp(uint8_t key) { ups.push_back(key); }
#define AUTOMATION_TYPE_KEY_GAP_MS 15
struct AutomationKeyStroke { uint8_t code; bool shift; };
static bool asciiToKeyStroke(uint8_t c, AutomationKeyStroke *stroke) {
    if (c != 'a' && c != 'A') return false;
    *stroke = {0, c=='A'};
    return true;
}

struct WiFiClient {
    std::string input, output;
    size_t pos = 0;
    bool connected() { return pos < input.size(); }
    int read() { return connected() ? input[pos++] : -1; }
    void printf(const char *format, ...) {
        char buf[2048];
        va_list args; va_start(args, format); vsnprintf(buf,sizeof(buf),format,args); va_end(args);
        output += buf;
    }
};
static void sendHttpFrame(WiFiClient &) { ++frames; }
static void writeHttpBody(WiFiClient &c, const uint8_t *data, size_t n) { c.output.append((const char*)data,n); }
#include "../src/basilisk/web_console_http.inc"

static std::string serve(const char *method, const char *path, const std::string &headers = "", const std::string &body = "") {
    WiFiClient c{headers+"\r\n"+body};
    size_t remaining = 4096;
    handleWebRequest(c, method, path, remaining, clk+3000);
    return c.output;
}

int main() {
    uint8_t random[16]={1};
    assert(serve("GET","/desk").find("403 Forbidden")!=std::string::npos);
    s_web_session.open(random, WebSession::View, esp_timer_get_time());
    std::string auth = "x-m5tab-session: " + std::string(s_web_session.token) + "\r\n";
    auto page = serve("GET","/desk");
    assert(page.find("<canvas")!=std::string::npos && page.find(s_web_session.token)==std::string::npos);
    assert(serve("POST","/desk/key/36").find("403 Forbidden")!=std::string::npos);
    assert(serve("POST","/desk/key/36",auth).find("403 Forbidden")!=std::string::npos);
    assert(serve("GET","/desk/status",auth).find("\"instructions\":10000000000")!=std::string::npos);
    serve("GET","/desk/frame",auth); assert(frames==1);
    assert(serve("GET","/desk/status",auth+auth).find("403 Forbidden")!=std::string::npos);
    ++random[0]; s_web_session.open(random,WebSession::Control,esp_timer_get_time());
    assert(serve("GET","/desk/status",auth).find("403 Forbidden")!=std::string::npos);
    auth = "X-M5Tab-Session: " + std::string(s_web_session.token) + "\r\n";
    assert(serve("GET","/desk/key/36",auth).find("404 Not Found")!=std::string::npos);
    assert(serve("POST","/desk/key/36x",auth).find("400 Bad Request")!=std::string::npos);
    assert(serve("POST","/desk/click/9999/359",auth).find("400 Bad Request")!=std::string::npos);
    assert(serve("POST","/desk/click/639/359",auth).find("204 No Content")!=std::string::npos);
    assert(serve("POST","/desk/key/36",auth).find("204 No Content")!=std::string::npos);
    assert(downs==std::vector<uint8_t>{36} && ups==downs && clicks==1);
    assert(serve("POST","/desk/double-click/639/359",auth).find("204 No Content")!=std::string::npos);
    assert(clicks==3);
    const auto length3 = auth+"Content-Length: 3\r\n";
    assert(serve("POST","/desk/text",length3,"Aaa").find("204 No Content")!=std::string::npos);
    assert((downs==std::vector<uint8_t>{36,0x38,0,0,0}));
    assert((ups==std::vector<uint8_t>{36,0,0x38,0,0}));
    auto count = downs.size();
    assert(serve("POST","/desk/text",length3,"aa!").find("400 Bad Request")!=std::string::npos);
    assert(serve("POST","/desk/text",auth+"Content-Length: 65\r\n").find("400 Bad Request")!=std::string::npos);
    assert(serve("POST","/desk/text",length3+"Transfer-Encoding: chunked\r\n","aaa").find("400 Bad Request")!=std::string::npos);
    assert(serve("POST","/desk/text",auth).find("400 Bad Request")!=std::string::npos);
    assert(downs.size()==count);
    // Revoke while Shift and A are held. Complete that tap, then stop.
    afterDelay = [] { s_web_session.close(); afterDelay=nullptr; };
    assert(serve("POST","/desk/text",length3,"AAA").find("403 Forbidden")!=std::string::npos);
    assert(downs.size()==count+2 && ups.size()==downs.size());
    assert(ups.back()==0x38 && ups[count]==0);
    s_web_session.open(random,WebSession::Control,esp_timer_get_time());
    s_emulator_ready=false;
    assert(serve("GET","/desk/status",auth).find("403 Forbidden")!=std::string::npos);
    s_emulator_ready=true;
    clk+=30*60*1000;
    assert(serve("POST","/desk/key/36",auth).find("403 Forbidden")!=std::string::npos);
    s_web_session.open(random,WebSession::Control,esp_timer_get_time());
    s_web_session.close();
    assert(serve("GET","/desk/status",auth).find("403 Forbidden")!=std::string::npos);
    assert(ups.size()==downs.size() && clicks==3);
}
