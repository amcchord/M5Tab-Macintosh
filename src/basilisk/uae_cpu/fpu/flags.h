/*
 *  fpu/flags.h - Floating-point condition code flags
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

#ifndef FPU_FLAGS_H
#define FPU_FLAGS_H

/* NOTE: this file shall be included only from fpu/fpu_*.cpp */
#undef  PUBLIC
#define PUBLIC  extern

#undef  PRIVATE
#define PRIVATE static

#undef  FFPU
#define FFPU    /**/

#undef  FPU
#define FPU     fpu.

/* ESP32 uses lazy flag evaluation (FPU_IEEE without x86 assembly) */
#define FPU_USE_LAZY_FLAGS

/* Native flag values match m68k format */
#define NATIVE_FFLAG_NEGATIVE   0x08000000
#define NATIVE_FFLAG_ZERO       0x04000000
#define NATIVE_FFLAG_INFINITY   0x02000000
#define NATIVE_FFLAG_NAN        0x01000000

#ifdef FPU_IMPLEMENTATION

/* -------------------------------------------------------------------------- */
/* --- Lazy Flags Evaluation                                              --- */
/* -------------------------------------------------------------------------- */

/* Initialization - nothing to do */
PRIVATE inline void FFPU fpu_init_native_fflags(void)
    { }

/* Native to m68k floating-point condition codes
 * Computed on-demand from FPU result register
 */
PRIVATE inline uae_u32 FFPU get_fpccr(void)
{
    uae_u32 fpccr = 0;
    if (isnan(FPU result)) {
        fpccr |= FPSR_CCB_NAN;
    }
    else if (FPU result == 0.0) {
        fpccr |= FPSR_CCB_ZERO;
    }
    else if (FPU result < 0.0) {
        fpccr |= FPSR_CCB_NEGATIVE;
    }
    if (isinf(FPU result)) {
        fpccr |= FPSR_CCB_INFINITY;
    }
    return fpccr;
}

/* M68k to native floating-point condition codes */
PRIVATE inline void FFPU set_fpccr(uae_u32 new_fpcond)
{
    if (new_fpcond & FPSR_CCB_NAN) {
        make_nan(FPU result);
    }
    else if (new_fpcond & FPSR_CCB_ZERO) {
        FPU result = 0.0;
    }
    else if (new_fpcond & FPSR_CCB_NEGATIVE) {
        FPU result = -1.0;
    }
    else {
        FPU result = +1.0;
    }
}

/* Make FPSR according to the value passed in argument */
PRIVATE inline void FFPU make_fpsr(fpu_register const & r)
    { FPU result = r; }

#endif /* FPU_IMPLEMENTATION */

/* -------------------------------------------------------------------------- */
/* --- Common methods                                                     --- */
/* -------------------------------------------------------------------------- */

/* Return the address of the floating-point condition codes register */
static inline uae_u32 * const FFPU address_of_fpccr(void)
    { return ((uae_u32 *)& FPU fpsr.condition_codes); }

#endif /* FPU_FLAGS_H */
