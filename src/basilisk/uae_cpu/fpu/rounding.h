/*
 *  fpu/rounding.h - FPU rounding mode and precision handling
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *  ESP32 port
 *
 *  MC68881/68040 fpu emulation
 *  
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef FPU_ROUNDING_H
#define FPU_ROUNDING_H

/* NOTE: this file shall be included from fpu/fpu_*.cpp */
#undef  PUBLIC
#define PUBLIC  extern

#undef  PRIVATE
#define PRIVATE static

#undef  FFPU
#define FFPU    /**/

#undef  FPU
#define FPU     fpu.

/* ESP32 uses generic rounding mode and precision handling */
#define FPU_USE_GENERIC_ROUNDING_MODE
#define FPU_USE_GENERIC_ROUNDING_PRECISION

/* -------------------------------------------------------------------------- */
/* --- Generic rounding mode and precision                                --- */
/* -------------------------------------------------------------------------- */

/* Set host control word for rounding mode and precision - nothing to do on ESP32 */
PRIVATE inline void set_host_control_word(void)
    { }

/* -------------------------------------------------------------------------- */
/* --- Rounding mode                                                      --- */
/* -------------------------------------------------------------------------- */

/* Return the current rounding mode in m68k format */
static inline uae_u32 FFPU get_rounding_mode(void)
    { return FPU fpcr.rounding_mode; }

/* Set rounding mode */
static inline void FFPU set_rounding_mode(uae_u32 new_rounding_mode)
    { FPU fpcr.rounding_mode = new_rounding_mode; }

/* -------------------------------------------------------------------------- */
/* --- Rounding precision                                                 --- */
/* -------------------------------------------------------------------------- */

/* Return the current rounding precision in m68k format */
static inline uae_u32 FFPU get_rounding_precision(void)
    { return FPU fpcr.rounding_precision; }

/* Set rounding precision */
static inline void FFPU set_rounding_precision(uae_u32 new_rounding_precision)
    { FPU fpcr.rounding_precision = new_rounding_precision; }

#endif /* FPU_ROUNDING_H */
