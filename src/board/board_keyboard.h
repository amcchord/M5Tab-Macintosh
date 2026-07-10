/*
 * board_keyboard.h - optional board-attached keyboard interface
 *
 * The Tab5 keyboard is an I2C peripheral that reports raw key-matrix edges.
 * Other board targets provide a no-op implementation so the input layer can
 * probe and poll this interface without board-specific preprocessor branches.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BoardKeyboardEvent {
    uint8_t row;
    uint8_t col;
    bool pressed;
} BoardKeyboardEvent;

/**
 * @brief Initialize the optional board keyboard transport.
 *
 * Absence of a keyboard is not an error. The implementation continues to
 * probe for a hot-plugged device from BoardKeyboard_Poll().
 */
bool BoardKeyboard_Init(void);

/**
 * @brief Poll transport state and return at most one raw matrix event.
 *
 * Calling this function also performs low-rate attach probes and connection
 * health checks. Returns false when no event is currently available.
 */
bool BoardKeyboard_Poll(BoardKeyboardEvent *event);

/** @brief Return whether a validated board keyboard is currently attached. */
bool BoardKeyboard_IsConnected(void);

/** @brief Stop polling and release transport resources. */
void BoardKeyboard_Exit(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
