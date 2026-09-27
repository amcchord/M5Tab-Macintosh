#include <cstring>
#include <atomic>
#include "video_test_stubs.h"
static int mac_screen_width=640,mac_screen_height=360;
static bool keys[128] = {};
static void ADBKeyDown(int code) { assert(!keys[code]); keys[code]=true; }
static void ADBKeyUp(int code) { assert(keys[code]); keys[code]=false; }
static void ADBSetRelMouseMode(bool) {}
static void ADBMouseMoved(int,int) {}
static void ADBMouseDown(int) {}
static void ADBMouseUp(int) {}
#include "../src/basilisk/input_shared_state.inc"

int main()
{
    resetInputClaims();
    InputKeyDown(0x36);
    InputAutomationKey(0x36,true); InputAutomationKey(0x36,true);
    InputAutomationReleaseAll();
    assert(keys[0x36] && controlKeyClaimed() && adb_key_claims[0x36]==1);
    InputKeyUp(0x36); assert(!keys[0x36] && !controlKeyClaimed());
    // Race a physical keyboard with serial press/release and idle cleanup.
    std::atomic<bool> start{false};
    std::thread physical([&]{
        while(!start) std::this_thread::yield();
        for(int i=0;i<20000;++i){ InputKeyDown(0x36); std::this_thread::yield(); InputKeyUp(0x36); }
    });
    std::thread serial([&]{
        start=true;
        for(int i=0;i<20000;++i){ InputAutomationKey(0x36,true); std::this_thread::yield(); InputAutomationKey(0x36,false); }
    });
    std::thread cleanup([&]{
        while(!start) std::this_thread::yield();
        for(int i=0;i<20000;++i) InputAutomationReleaseAll();
    });
    physical.join();serial.join();cleanup.join();
    assert(!keys[0x36] && adb_key_claims[0x36]==0 && !automation_keys_down[0x36]);
    puts("PASS: physical/automation modifier ownership and 60000 concurrent input operations");
}
