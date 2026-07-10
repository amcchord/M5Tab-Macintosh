/*
 * board_keyboard_waveshare.cpp - optional keyboard HAL no-op
 *
 * The Waveshare target has no board-attached keyboard. USB HID and the
 * on-screen keyboard remain available through the shared input layer.
 */

#include "board_keyboard.h"

extern "C" bool BoardKeyboard_Init(void)
{
    return true;
}

extern "C" bool BoardKeyboard_Poll(BoardKeyboardEvent *event)
{
    (void)event;
    return false;
}

extern "C" bool BoardKeyboard_IsConnected(void)
{
    return false;
}

extern "C" void BoardKeyboard_Exit(void)
{
}
