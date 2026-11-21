// Link the production MSC lifecycle against an in-memory USB bus and card.
#include <cassert>
#include <cstdint>
#include "diskio.h"

struct Console {
    template<typename... Args> void printf(const char *, Args...) {}
    void println(const char *) {}
    void flush() {}
} Serial;
struct Card {
    unsigned sectorSize() const { return 512; }
    unsigned numSectors() const { return 1024; }
protected:
    uint8_t _pdrv = 0;
} SD_FS;

static bool class_ok = true, bus_ok = true, media_present, power = true;
static unsigned waits, restarts;
using esp_event_base_t = int;
enum { ARDUINO_USB_EVENTS, ARDUINO_USB_STARTED_EVENT, ARDUINO_USB_STOPPED_EVENT };
namespace m5 { enum class ext_port_mask_t { ext_USB }; }
struct {
    struct {
        void setExtOutput(bool enabled, m5::ext_port_mask_t) { power = enabled; }
    } Power;
} M5;
struct USBMSC {
    void vendorID(const char *) {}
    void productID(const char *) {}
    void productRevision(const char *) {}
    template<typename T> void onRead(T) {}
    template<typename T> void onWrite(T) {}
    template<typename T> void onStartStop(T) {}
    void mediaPresent(bool state) { media_present = state; }
    void isWritable(bool) {}
    bool begin(unsigned, unsigned) { return class_ok; }
    void end() { media_present = false; }
};
struct {
    void productName(const char *) {}
    void manufacturerName(const char *) {}
    template<typename T> void onEvent(T) {}
    bool begin() { return bus_ok; }
} USB;
void delay(unsigned) {}
#define pdMS_TO_TICKS(x) (x)
void vTaskDelay(unsigned);
DRESULT disk_read(BYTE, BYTE *, uint32_t, unsigned) { return RES_ERROR; }
DRESULT disk_write(BYTE, const BYTE *, uint32_t, unsigned) { return RES_ERROR; }
void BootGUI_Restart() { assert(power && !media_present); ++restarts; }

#include "../src/basilisk/usb_msc.cpp"

void vTaskDelay(unsigned) {
    assert(media_present);
#if defined(BOARD_M5STACK_TAB5)
    assert(!power);
#endif
    if (++waits == 3) UsbMsc_RequestExit();
}
int main() {
    assert(UsbMsc_IsSupported());
    const char *error = nullptr;
    class_ok = false;
    assert(!UsbMsc_Enter(&error) && error && power && !media_present);
    class_ok = true; bus_ok = false; error = nullptr;
    assert(!UsbMsc_Enter(&error) && error && power && !media_present);
    bus_ok = true;
    for (int i = 0; i < 2; ++i) {
        error = nullptr; waits = 0;
        assert(UsbMsc_Enter(&error) && !error && waits == 3 && !media_present && power);
    }
#if defined(BOARD_M5STACK_TAB5)
    assert(restarts == 2);
#else
    assert(!restarts);
#endif
}
