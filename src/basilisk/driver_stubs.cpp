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
#include "cdrom.h"
#include "scsi.h"
#include "serial.h"
#include "ether.h"
#include "audio.h"
#include "user_strings.h"

/*
 * Global tick inhibit flag (referenced by emul_op.cpp)
 */
bool tick_inhibit = false;

/*
 * CD-ROM driver stubs
 */

// CD-ROM icon data (258 bytes) - blank icon for ESP32
const uint8 CDROMIcon[258] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // Mask
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // Extra 2 bytes
    0x00, 0x00
};

// CD-ROM icon address in Mac memory
uint32 CDROMIconAddr = 0;

void CDROMInit(void) {}
void CDROMExit(void) {}
void CDROMRemount(void) {}
int16 CDROMOpen(uint32 pb, uint32 dce) { (void)pb; (void)dce; return noErr; }
int16 CDROMPrime(uint32 pb, uint32 dce) { (void)pb; (void)dce; return noErr; }
int16 CDROMControl(uint32 pb, uint32 dce) { (void)pb; (void)dce; return noErr; }
int16 CDROMStatus(uint32 pb, uint32 dce) { (void)pb; (void)dce; return noErr; }
void CDROMInterrupt(void) {}

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
 * Ethernet driver stubs
 */

void EtherInit(void) {}
void EtherExit(void) {}
void EtherReset(void) {}
void EtherInterrupt(void) {}
int16 EtherOpen(uint32 pb, uint32 dce) { (void)pb; (void)dce; return noErr; }
int16 EtherControl(uint32 pb, uint32 dce) { (void)pb; (void)dce; return noErr; }
void EtherReadPacket(uint32 &src, uint32 &dest, uint32 &len, uint32 &remaining) {
    (void)src; (void)dest; (void)len; (void)remaining;
}

/*
 * Audio driver stubs
 */

#include <vector>

uint32 audio_component_flags = 0;
bool audio_open = false;
std::vector<uint32> audio_sample_rates;
std::vector<uint16> audio_sample_sizes;
std::vector<uint8> audio_channel_counts;

// Audio status structure - defined in audio.h as struct audio_status
struct audio_status AudioStatus = { 0, 0, 0, 0, 0 };

void AudioInit(void) {
    audio_open = false;
    audio_sample_rates.clear();
    audio_sample_rates.push_back(22050 << 16);  // 16.16 fixed point
    audio_sample_rates.push_back(44100 << 16);
    audio_sample_sizes.clear();
    audio_sample_sizes.push_back(8);
    audio_sample_sizes.push_back(16);
    audio_channel_counts.clear();
    audio_channel_counts.push_back(1);
    audio_channel_counts.push_back(2);
    AudioStatus.sample_rate = 44100 << 16;
    AudioStatus.sample_size = 16;
    AudioStatus.channels = 2;
    AudioStatus.mixer = 0;
    AudioStatus.num_sources = 0;
}
void AudioExit(void) { audio_open = false; }
void AudioReset(void) { audio_open = false; }
void AudioInterrupt(void) {}
int32 AudioDispatch(uint32 params, uint32 ti) { (void)params; (void)ti; return noErr; }
int16 SoundInOpen(uint32 pb, uint32 dce) { (void)pb; (void)dce; return noErr; }
int16 SoundInPrime(uint32 pb, uint32 dce) { (void)pb; (void)dce; return noErr; }
int16 SoundInControl(uint32 pb, uint32 dce) { (void)pb; (void)dce; return noErr; }
int16 SoundInStatus(uint32 pb, uint32 dce) { (void)pb; (void)dce; return noErr; }
int16 SoundInClose(uint32 pb, uint32 dce) { (void)pb; (void)dce; return noErr; }

/*
 * Timer functions - ESP32 implementation
 */

// Get current time in microseconds
void timer_current_time(uint64 &time) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time = (uint64)tv.tv_sec * 1000000 + tv.tv_usec;
}

// Return current date/time as Mac seconds since 1904
uint32 TimerDateTime(void) {
    // Mac epoch is Jan 1, 1904
    // Unix epoch is Jan 1, 1970
    // Difference is 2082844800 seconds
    time_t t = time(NULL);
    return (uint32)(t + 2082844800UL);
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

// Convert Mac time to host time (microseconds)
void timer_mac2host_time(uint64 &res, int32 mactime) {
    if (mactime > 0) {
        // Positive: already in microseconds
        res = (uint64)mactime;
    } else {
        // Negative: milliseconds (negate to get positive)
        res = (uint64)(-mactime) * 1000;
    }
}

// Convert host time to Mac time
int32 timer_host2mac_time(uint64 hosttime) {
    if (hosttime < 0x7fffffff) {
        return (int32)hosttime;  // Microseconds
    } else {
        return (int32)(-(hosttime / 1000));  // Milliseconds (negative)
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
    // Return empty string for unknown string IDs
    static const char *empty = "";
    (void)num;
    return empty;
}
