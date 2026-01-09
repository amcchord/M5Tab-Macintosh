/*
 *  fpu/types.h - basic types for FPU registers
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

#ifndef FPU_TYPES_H
#define FPU_TYPES_H

#include "sysdeps.h"

/* ESP32-P4 configuration:
 * - sizeof(float) = 4 bytes
 * - sizeof(double) = 8 bytes
 * - sizeof(long double) = 8 bytes (same as double on ESP32)
 * - Hardware FPU supports both single and double precision
 */

/* Do not use long doubles on ESP32 (they're just doubles anyway) */
#undef USE_LONG_DOUBLE
#undef USE_QUAD_DOUBLE

/* FPU_IEEE is defined in platformio.ini build flags */
#if defined(FPU_IEEE)

/* 4-byte floats */
typedef float uae_f32;

/* 8-byte floats */
typedef double uae_f64;

/* FPU registers use double precision on ESP32
 * This is sufficient for most Mac software - the 68040's 80-bit extended
 * precision is approximated with 64-bit doubles (same approach as other
 * non-x86 BasiliskII ports)
 */
typedef uae_f64 fpu_register;

/* All floating-point types */
typedef fpu_register fpu_extended;
typedef uae_f64      fpu_double;
typedef uae_f32      fpu_single;

#endif /* FPU_IEEE */

#endif /* FPU_TYPES_H */
