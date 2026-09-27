/*
 * board_config.h - per-board constants
 *
 * Selects the right pin map, Mac screen geometry, and physical display size
 * at compile time based on the BOARD_* macro defined by the active
 * PlatformIO environment in platformio.ini.
 *
 * Exactly one of BOARD_M5STACK_TAB5 or BOARD_WAVESHARE_P4_101 must be set.
 */
#pragma once

#if !defined(BOARD_M5STACK_TAB5) && !defined(BOARD_WAVESHARE_P4_101)
#error "No board target defined. Set BOARD_M5STACK_TAB5 or BOARD_WAVESHARE_P4_101 via platformio.ini build_flags."
#endif

#if defined(BOARD_M5STACK_TAB5) && defined(BOARD_WAVESHARE_P4_101)
#error "Multiple board targets defined. Pick exactly one."
#endif

/* ------------------------------------------------------------------------- */
/* M5Stack Tab5                                                              */
/* ------------------------------------------------------------------------- */
#if defined(BOARD_M5STACK_TAB5)

#define BOARD_NAME                  "M5Stack Tab5"

/* Physical display (landscape after rotation) */
#define BOARD_DISPLAY_WIDTH         1280
#define BOARD_DISPLAY_HEIGHT        720

/* Mac emulated framebuffer - 640x360 doubles to 1280x720 */
#define BOARD_MAC_SCREEN_WIDTH      640
#define BOARD_MAC_SCREEN_HEIGHT     360
#define BOARD_PIXEL_SCALE           2

/* Tile grid used by video_esp32.cpp for dirty tracking. 32 pixels wide so
 * each 32-byte damage span of an 8-bit frame buffer maps to exactly one tile. */
#define BOARD_TILE_WIDTH            32
#define BOARD_TILE_HEIGHT           40
#define BOARD_TILES_X               (BOARD_MAC_SCREEN_WIDTH  / BOARD_TILE_WIDTH)  /* 20 */
#define BOARD_TILES_Y               (BOARD_MAC_SCREEN_HEIGHT / BOARD_TILE_HEIGHT) /* 9  */

/* SD card on SPI (Tab5 layout documented in boardConfig.md).
 *
 * BOARD_SD_MOUNT_POINT is the *VFS prefix* where the card is registered,
 * which matches Arduino SD's default mountpoint (/sd). It is NOT what
 * appears in calls like SD_FS.open("/Q650.ROM") - Arduino's File class
 * automatically prepends its mountpoint when opening files, so app
 * code keeps using SD-root-relative paths like "/Q650.ROM". The macro
 * is only consumed by code that has to talk to POSIX directly (stat,
 * mkdir, etc., e.g. ExtFS host-folder validation in prefs_esp32.cpp).
 *
 * Previous value of "/" was wrong - the VFS does not actually have a
 * mount registered at "/" on Tab5, so POSIX stat("/Shared") returned
 * ENOENT even when the card had a /Shared folder, which is what made
 * ExtFS silently disable. */
#define BOARD_SD_SPI_SCK            43
#define BOARD_SD_SPI_MOSI           44
#define BOARD_SD_SPI_MISO           39
#define BOARD_SD_SPI_CS             42
#define BOARD_SD_MOUNT_POINT        "/sd"

/* ------------------------------------------------------------------------- */
/* Waveshare ESP32-P4-WIFI6-Touch-LCD-10.1                                   */
/* ------------------------------------------------------------------------- */
#elif defined(BOARD_WAVESHARE_P4_101)

#define BOARD_NAME                  "Waveshare P4 10.1"

/* Physical display after 90-degree rotation of the 800x1280 portrait panel */
#define BOARD_DISPLAY_WIDTH         1280
#define BOARD_DISPLAY_HEIGHT        800

/* Mac emulated framebuffer - 640x400 doubles to 1280x800 exactly */
#define BOARD_MAC_SCREEN_WIDTH      640
#define BOARD_MAC_SCREEN_HEIGHT     400
#define BOARD_PIXEL_SCALE           2

/* Tile grid - 20x10 = 200 tiles (see the Tab5 note on tile width) */
#define BOARD_TILE_WIDTH            32
#define BOARD_TILE_HEIGHT           40
#define BOARD_TILES_X               (BOARD_MAC_SCREEN_WIDTH  / BOARD_TILE_WIDTH)  /* 20 */
#define BOARD_TILES_Y               (BOARD_MAC_SCREEN_HEIGHT / BOARD_TILE_HEIGHT) /* 10 */

/* SD card - mounted by the Waveshare BSP at /sd (CONFIG_BSP_SD_MOUNT_POINT).
 * We do not use SPI mode here - the BSP configures SDMMC 4-bit slot 0. */
#define BOARD_SD_MOUNT_POINT        "/sd"

#endif

/* Sanity: tile grid must divide the Mac framebuffer exactly. */
#if (BOARD_MAC_SCREEN_WIDTH  % BOARD_TILE_WIDTH)  != 0
#error "BOARD_TILE_WIDTH does not divide BOARD_MAC_SCREEN_WIDTH"
#endif
#if (BOARD_MAC_SCREEN_HEIGHT % BOARD_TILE_HEIGHT) != 0
#error "BOARD_TILE_HEIGHT does not divide BOARD_MAC_SCREEN_HEIGHT"
#endif
