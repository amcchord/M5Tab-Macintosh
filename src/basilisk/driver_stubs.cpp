/*
 *  driver_stubs.cpp - Stub implementations for disabled drivers
 *
 *  BasiliskII ESP32 Port
 *  
 *  These stubs provide minimal implementations for drivers that are
 *  disabled on ESP32 but are referenced by the main emulation code.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "macos_util.h"
#include "scsi.h"
#include "serial.h"
// Note: ether.h not needed - real ethernet implementation in ether.cpp/ether_esp32.cpp
// Note: audio.h not needed - real audio implementation in audio.cpp/audio_esp32.cpp
#include "user_strings.h"

/*
 * Global tick inhibit flag (referenced by emul_op.cpp)
 */
bool tick_inhibit = false;

/*
 * SCSI driver stubs
 */

int16 SCSIReset(void) { return noErr; }
int16 SCSIGet(void) { return noErr; }
int16 SCSISelect(int id) { (void)id; return noErr; }
int16 SCSICmd(int len, uint8 *cmd) { (void)len; (void)cmd; return noErr; }
int16 SCSIRead(uint32 tib) { (void)tib; return noErr; }
int16 SCSIWrite(uint32 tib) { (void)tib; return noErr; }
int16 SCSIComplete(uint32 stat, uint32 msg, uint32 ticks) { (void)stat; (void)msg; (void)ticks; return noErr; }
uint16 SCSIStat(void) { return 0; }  // Return 0 = bus free
int16 SCSIMsgIn(void) { return 0; }
int16 SCSIMsgOut(void) { return noErr; }
int16 SCSIMgrBusy(void) { return 0; }  // Return 0 = not busy

/*
 * Serial driver stubs
 */

// Dummy serial port object
class DummySERDPort : public SERDPort {
public:
    DummySERDPort() : SERDPort() {}
    virtual ~DummySERDPort() {}
    
    virtual int16 open(uint16 config) { (void)config; return noErr; }
    virtual int16 prime_in(uint32 pb, uint32 dce) { (void)pb; (void)dce; return noErr; }
    virtual int16 prime_out(uint32 pb, uint32 dce) { (void)pb; (void)dce; return noErr; }
    virtual int16 control(uint32 pb, uint32 dce, uint16 code) { (void)pb; (void)dce; (void)code; return noErr; }
    virtual int16 status(uint32 pb, uint32 dce, uint16 code) { (void)pb; (void)dce; (void)code; return noErr; }
    virtual int16 close(void) { return noErr; }
};

// Create dummy port instances
static DummySERDPort dummy_port_a;
static DummySERDPort dummy_port_b;

// The serial port instance (referenced by serial_dummy.cpp)
SERDPort *the_serd_port[2] = { &dummy_port_a, &dummy_port_b };

void SerialInit(void) {}
void SerialExit(void) {}
int16 SerialOpen(uint32 pb, uint32 dce, int port) { (void)pb; (void)dce; (void)port; return noErr; }
int16 SerialPrime(uint32 pb, uint32 dce, int port) { (void)pb; (void)dce; (void)port; return noErr; }
int16 SerialControl(uint32 pb, uint32 dce, int port) { (void)pb; (void)dce; (void)port; return noErr; }
int16 SerialStatus(uint32 pb, uint32 dce, int port) { (void)pb; (void)dce; (void)port; return noErr; }
int16 SerialClose(uint32 pb, uint32 dce, int port) { (void)pb; (void)dce; (void)port; return noErr; }
void SerialInterrupt(void) {}

/*
 * Ethernet driver - real implementation in ether.cpp and ether_esp32.cpp
 * (stubs removed to avoid duplicate symbols)
 */

/*
 * Audio driver - real implementation in audio.cpp and audio_esp32.cpp
 * (stubs removed to avoid duplicate symbols)
 */

/*
 * Timer functions - ESP32 implementation
 */

// Get current time in microseconds
void timer_current_time(uint64 &time) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time = (uint64)tv.tv_sec * 1000000 + tv.tv_usec;
}

// Return microsecond counter (split into hi/lo 32-bit parts)
void Microseconds(uint32 &hi, uint32 &lo) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64 us = (uint64)tv.tv_sec * 1000000 + tv.tv_usec;
    hi = (uint32)(us >> 32);
    lo = (uint32)(us & 0xFFFFFFFF);
}

// Add two times
void timer_add_time(uint64 &res, uint64 a, uint64 b) {
    res = a + b;
}

// Subtract two times
void timer_sub_time(uint64 &res, uint64 a, uint64 b) {
    res = a - b;
}

// Compare two times
int timer_cmp_time(uint64 a, uint64 b) {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

// Convert Mac time to host time (microseconds).
// Time Manager encoding:
//   mactime > 0 : milliseconds
//   mactime < 0 : negative microseconds
void timer_mac2host_time(uint64 &res, int32 mactime) {
    if (mactime > 0) {
        // Positive values are milliseconds.
        res = (uint64)mactime * 1000ULL;
    } else {
        // Negative values encode microseconds (with inverted sign).
        res = (uint64)(-mactime);
    }
}

// Convert host time (microseconds) to Time Manager encoding:
//   return > 0 for milliseconds when value exceeds 31-bit microsecond range
//   return < 0 for microseconds otherwise
int32 timer_host2mac_time(uint64 hosttime) {
    if (hosttime > 0x7fffffffULL) {
        return (int32)(hosttime / 1000ULL);  // Milliseconds
    } else {
        return -(int32)hosttime;             // Negative microseconds
    }
}

/*
 * Clipboard driver stubs
 */

void ClipInit(void) {}
void ClipExit(void) {}
void GetScrap(void **handle, uint32 type, int32 offset) { 
    (void)handle; (void)type; (void)offset;
}
void PutScrap(uint32 type, void *data, int32 length) {
    (void)type; (void)data; (void)length;
}

/*
 * SCSI Init/Exit stubs
 */

void SCSIInit(void) {}
void SCSIExit(void) {}

/*
 * User string lookup
 */

const char *GetString(int num) {
    // Look up a localized string from the common + platform tables defined
    // in user_strings.cpp and user_strings_esp32.cpp. Both tables are NULL-
    // terminated with an entry of {-1, NULL}. This is the same convention
    // the upstream Unix/BeOS ports use; our original stub returned "" for
    // every id which broke anything that depends on a non-empty string
    // (e.g. ExtFS uses STR_EXTFS_VOLUME_NAME to label the mounted volume).
    for (int i = 0; common_strings[i].num >= 0; i++) {
        if (common_strings[i].num == num) {
            return common_strings[i].str;
        }
    }
    for (int i = 0; platform_strings[i].num >= 0; i++) {
        if (platform_strings[i].num == num) {
            return platform_strings[i].str;
        }
    }
    static const char *empty = "";
    return empty;
}
