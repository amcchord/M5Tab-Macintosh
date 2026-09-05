/*
 * usb_msc.cpp - Pre-boot "USB Disk" mode implementation.
 *
 * See usb_msc.h for the public contract. We use Arduino-ESP32's USBMSC
 * class (a thin wrapper over arduino_tinyusb) with SD_FS.readRAW /
 * writeRAW as the raw-sector backing store. Compared to re-initializing
 * the IDF SDMMC driver ourselves this has two wins:
 *
 *   1. It coexists cleanly with Arduino's own TinyUSB bundle - no
 *      duplicate-symbol linkage with esp_tinyusb.
 *   2. The SD card stays mounted through Arduino SD / SD_MMC, so there's
 *      no unmount/remount dance and existing SD state (open file
 *      handles, seek positions) is preserved.
 *
 * Platform notes:
 *   - Waveshare (ARDUINO_USB_MODE=0): USB-OTG is otherwise idle during
 *     pre-boot; USB.begin() enumerates us as an MSC-only device. Done
 *     detaches the MSC class cleanly and returns to the boot menu.
 *   - Tab5 (BOARD_M5STACK_TAB5): Tab5's USB-C jack is hard-wired to the
 *     ESP32-P4 USB-Serial-JTAG controller (FS_PHY1) - there is no way
 *     to surface TinyUSB MSC on it without burning EFUSE_USB_PHY_SEL.
 *     The USB-A jack, however, is wired to USB-OTG HS (HS_PHY, GPIO
 *     49/50), which is exactly where Arduino-ESP32's USB.begin() lands.
 *     So we run MSC over USB-A with a USB-A-to-USB-C cable. Tab5's own
 *     5V output to the USB-A port is gated through the PI4IOE #1 pin 3
 *     load switch; we turn it off (M5.Power.setExtOutput(false, ext_USB))
 *     so the attached host can supply VBUS, then re-enable it on exit.
 *     HWCDC/Serial lives on USB-Serial-JTAG - a different hardware
 *     controller - so it stays up the whole time. We still ESP.restart()
 *     on exit because Arduino-ESP32 doesn't expose USB.end()/usb_del_phy,
 *     and the emulator's esp_usb_host needs a clean PHY to claim.
 */

#include <Arduino.h>
#include "usb_msc.h"
#include "board_sd.h"
#include "board_config.h"

#include "sdkconfig.h"

#if defined(BOARD_M5STACK_TAB5)
#include <M5Unified.h>
#endif

// Arduino's USBMSC class is compiled in the core whenever the SoC has a
// USB-OTG peripheral and TinyUSB MSC is enabled via sdkconfig. On both
// Tab5 and Waveshare we hit this branch; per-board behaviour is gated at
// runtime inside UsbMsc_Enter (see BOARD_M5STACK_TAB5 sections).
#if defined(SOC_USB_OTG_SUPPORTED) && CONFIG_TINYUSB_MSC_ENABLED
#define USB_MSC_AVAILABLE 1
#else
#define USB_MSC_AVAILABLE 0
#endif

#if USB_MSC_AVAILABLE

#include <USB.h>
#include <USBMSC.h>

// FatFs low-level disk API. Arduino's SD/SD_MMC wrappers only ever ask it
// for one sector at a time; we need the count argument. See the comment on
// FsPdrvAccess below.
#include "diskio.h"

static USBMSC            s_msc;
static volatile bool     s_exit_requested = false;
static volatile bool     s_host_mounted   = false;
static bool              s_msc_up         = false;
static bool              s_usb_started    = false;

/* ------------------------------------------------------------------------- */
/* MSC <-> SD read/write callbacks                                           */
/* ------------------------------------------------------------------------- */

/*
 * Arduino's SD (SPI) and SD_MMC (SDMMC) expose only single-sector
 * readRAW()/writeRAW(), which hardcode a FatFs count of 1. Driving MSC
 * through those turned every CONFIG_TINYUSB_MSC_BUFSIZE-sized USB transfer
 * into BUFSIZE/512 separate single-block SD commands. Because SD cards have
 * 64KB-4MB internal erase blocks, each single-block write (CMD24) costs a
 * full erase-block read-modify-write, so copies capped out around 250 KB/s
 * -- the USB link was never the bottleneck; the OTG port is High-Speed and
 * sat idle.
 *
 * Both classes register a FatFs diskio driver whose read/write DO take a
 * sector count, so one disk_write() call issues a proper multi-block
 * transfer (CMD25) instead. The only thing Arduino withholds is the drive
 * number, which lives in a protected member. FsPdrvAccess reaches it the
 * standard way -- through a derived class -- and works unchanged for both
 * boards' filesystem types. Nothing is ever constructed; the cast exists
 * only to satisfy the protected-member access rule.
 */
template <typename FsT>
struct FsPdrvAccess : public FsT {
    static uint8_t get(FsT &fs) { return static_cast<FsPdrvAccess &>(fs)._pdrv; }
};

static inline uint8_t sd_pdrv(void)
{
    return FsPdrvAccess<decltype(SD_FS)>::get(SD_FS);
}

static int32_t msc_on_read(uint32_t lba, uint32_t offset, void *buffer,
                           uint32_t bufsize)
{
    // offset is always 0 and bufsize always a whole number of sectors,
    // because we advertise the card's own sector size to the MSC class.
    (void)offset;
    uint32_t sec_size = SD_FS.sectorSize();
    if (!sec_size) return -1;
    uint32_t nsec = bufsize / sec_size;
    if (!nsec) return 0;
    if (disk_read(sd_pdrv(), static_cast<BYTE *>(buffer), lba, nsec) != RES_OK) {
        return -1;
    }
    return nsec * sec_size;
}

static int32_t msc_on_write(uint32_t lba, uint32_t offset, uint8_t *buffer,
                            uint32_t bufsize)
{
    (void)offset;
    uint32_t sec_size = SD_FS.sectorSize();
    if (!sec_size) return -1;
    uint32_t nsec = bufsize / sec_size;
    if (!nsec) return 0;
    if (disk_write(sd_pdrv(), buffer, lba, nsec) != RES_OK) {
        return -1;
    }
    return nsec * sec_size;
}

static bool msc_on_start_stop(uint8_t /*power_condition*/, bool start,
                              bool load_eject)
{
    // A host-initiated eject signals that data has been flushed. Use that
    // as a proxy for "host disconnected the disk".
    if (load_eject && !start) {
        s_host_mounted = false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* USB bus events - used to update host_mounted for the UI indicator.        */
/* ------------------------------------------------------------------------- */

static void usb_event_cb(void *arg, esp_event_base_t base, int32_t id,
                         void *data)
{
    (void)arg; (void)data;
    if (base != ARDUINO_USB_EVENTS) return;
    switch (id) {
        case ARDUINO_USB_STARTED_EVENT:
            s_host_mounted = true;
            Serial.println("[USB_MSC] Host attached");
            break;
        case ARDUINO_USB_STOPPED_EVENT:
            s_host_mounted = false;
            Serial.println("[USB_MSC] Host detached");
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

extern "C" bool UsbMsc_IsSupported(void) { return true; }

extern "C" bool UsbMsc_Enter(const char **out_error_msg)
{
    Serial.println("[USB_MSC] Entering USB Disk mode...");
    s_exit_requested = false;
    s_host_mounted   = false;

    uint32_t sec_size = SD_FS.sectorSize();
    uint32_t n_sec    = SD_FS.numSectors();
    if (!sec_size || !n_sec) {
        if (out_error_msg) *out_error_msg = "SD card not available";
        return false;
    }
    Serial.printf("[USB_MSC] SD geometry: %u sectors x %u bytes\n",
                  (unsigned)n_sec, (unsigned)sec_size);

#if defined(BOARD_M5STACK_TAB5)
    // Tab5 path: USB-A jack is wired to OTG-HS (where USB.begin() lands).
    // Turn off Tab5's own 5V output to the USB-A load switch so the
    // attached host can supply VBUS. HWCDC/Serial is on a separate
    // USB-Serial-JTAG PHY and keeps running throughout.
    Serial.println("[USB_MSC] Tab5: gating USB-A 5V so host can supply VBUS.");
    M5.Power.setExtOutput(false, m5::ext_port_mask_t::ext_USB);
#endif

    s_msc.vendorID("M5Tab");
    s_msc.productID("Macintosh-SD");
    s_msc.productRevision("1.0");
    s_msc.onRead(msc_on_read);
    s_msc.onWrite(msc_on_write);
    s_msc.onStartStop(msc_on_start_stop);
    s_msc.mediaPresent(true);
    s_msc.isWritable(true);

    if (!s_msc.begin(n_sec, sec_size)) {
        if (out_error_msg) *out_error_msg = "USBMSC.begin() failed";
        return false;
    }
    s_msc_up = true;

    if (!s_usb_started) {
        USB.productName("Macintosh-SD");
        USB.manufacturerName("M5Tab");
        USB.onEvent(usb_event_cb);
        if (!USB.begin()) {
            if (out_error_msg) *out_error_msg = "USB.begin() failed";
            s_msc.end();
            s_msc_up = false;
            return false;
        }
        s_usb_started = true;
    }

    // Park until the boot GUI asks us to leave. The host OS sees the
    // microSD as a normal USB drive in the meantime.
    while (!s_exit_requested) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

#if defined(BOARD_M5STACK_TAB5)
    // Tab5: eject, then reboot. Arduino-ESP32 doesn't expose USB.end()
    // or usb_del_phy, and esp_usb_host (used by input_esp32.cpp for
    // keyboards) needs an unclaimed PHY. Restart is the cleanest fix.
    s_msc.mediaPresent(false);
    s_msc.end();
    s_msc_up       = false;
    s_host_mounted = false;
    // Tiny pause so the host sees the ejection before we disappear.
    delay(250);
    // Restore USB-A 5V output so keyboards/mice work after reboot.
    M5.Power.setExtOutput(true, m5::ext_port_mask_t::ext_USB);
    Serial.println("[USB_MSC] USB Disk mode exited; restarting to release PHY.");
    Serial.flush();
    delay(50);
    ESP.restart();
    // not reached
    return true;
#else
    // Waveshare: detach the MSC class and return. USB stays begun
    // because Arduino doesn't expose USB.end(); next UsbMsc_Enter() will
    // just re-attach MSC and the host will see the disk reappear.
    s_msc.mediaPresent(false);
    s_msc.end();
    s_msc_up        = false;
    s_host_mounted  = false;

    Serial.println("[USB_MSC] USB Disk mode exited cleanly");
    return true;
#endif
}

extern "C" void UsbMsc_RequestExit(void)
{
    s_exit_requested = true;
}

extern "C" bool UsbMsc_HostMounted(void)
{
    return s_host_mounted;
}

#else  /* USB_MSC_AVAILABLE */

// This build target doesn't have USBMSC available (TinyUSB MSC not
// enabled in sdkconfig, or SoC without USB-OTG). Stub out the entry
// points so the module still links; the boot GUI checks
// UsbMsc_IsSupported() and hides the button when false.

extern "C" bool UsbMsc_IsSupported(void)           { return false; }
extern "C" bool UsbMsc_HostMounted(void)           { return false; }
extern "C" void UsbMsc_RequestExit(void)           {}
extern "C" bool UsbMsc_Enter(const char **out_error_msg)
{
    if (out_error_msg) {
        *out_error_msg = "USB Disk mode not available in this build";
    }
    return false;
}

#endif  /* USB_MSC_AVAILABLE */
