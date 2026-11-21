/*
 * usb_msc.h - Pre-boot "USB Disk" mode.
 *
 * The USB host gets raw sectors from the mounted SD card. Startup Settings
 * must keep filesystem operations idle until USB Disk mode has exited.
 *
 * The OTG controller is shared with the HID host stack used during
 * emulation, but the two are mutually exclusive in time: USB Disk mode
 * only runs in pre-boot, HID host only once the emulator is running.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Called once at startup (or the first time we need it) to check that
 *        required build configuration is present. Returns false if the
 *        required TinyUSB MSC Kconfig options are missing, in which case the
 *        boot GUI should hide the "USB Disk" button.
 */
bool UsbMsc_IsSupported(void);

/**
 * @brief Enter USB Disk mode.
 *
 * Starts TinyUSB with the mounted card as backing storage and waits for
 * @ref UsbMsc_RequestExit. A successful exit restarts the Tab5 into Startup
 * Settings. Waveshare detaches the drive and returns to its caller.
 *
 * @param[out] out_error_msg  On failure, set to a short human-readable
 *                            description (static string). May be NULL.
 * @return true on clean enter+run+exit, false if the USB stack refused to
 *         start.
 */
bool UsbMsc_Enter(const char **out_error_msg);

/**
 * @brief Ask the @ref UsbMsc_Enter loop to tear down and return. Safe to
 *        call from any task.
 */
void UsbMsc_RequestExit(void);

/**
 * @brief Query whether the host PC currently has the disk mounted. Useful
 *        for showing a "Safe to eject" hint in the UI.
 */
bool UsbMsc_HostMounted(void);

#ifdef __cplusplus
}
#endif
