/*
 *  input.h - Input handling (keyboard/mouse/touch) for ESP32
 *
 *  BasiliskII ESP32 Port
 *
 *  This module handles:
 *  - USB HID keyboard input (translated to Mac ADB keycodes)
 *  - Touch panel input (as mouse cursor)
 *  - USB HID mouse input (optional)
 */

#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  Initialize input handling
 *  Sets up USB host for HID devices and touch panel
 *  Returns true on success
 */
bool InputInit(void);

/*
 *  Shutdown input handling
 *  Releases USB host resources
 */
void InputExit(void);

/*
 *  Poll for input events
 *  Should be called from the main loop or a dedicated task
 *  Forwards events to ADB subsystem (ADBKeyDown, ADBKeyUp, ADBMouseMoved, etc.)
 */
void InputPoll(void);

/*
 *  Set the Mac screen dimensions for touch coordinate mapping
 *  The touch panel coordinates will be scaled to these dimensions
 */
void InputSetScreenSize(int width, int height);

/*
 *  Enable/disable touch input
 */
void InputSetTouchEnabled(bool enabled);

/*
 *  Enable/disable USB keyboard input
 */
void InputSetKeyboardEnabled(bool enabled);

/*
 *  Check if a USB keyboard or the official Tab5 Keyboard is connected
 */
bool InputIsKeyboardConnected(void);

/*
 *  Check if USB mouse is connected
 */
bool InputIsMouseConnected(void);

/*
 * Shared ADB key broker for input producers such as the touchscreen overlay.
 * Multiple physical sources may claim the same key; the broker emits ADB
 * down/up only for the first claim and final release.
 */
void InputKeyDown(uint8_t mac_keycode);
void InputKeyUp(uint8_t mac_keycode);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_H */
