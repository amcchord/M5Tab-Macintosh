/*
 *  fpu/impl.h - FPU inline implementations
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

#ifndef FPU_IMPL_H
#define FPU_IMPL_H

/* NOTE: this file shall be included from fpu/fpu.h (after flags.h, exceptions.h, rounding.h) */
#undef  PUBLIC
#define PUBLIC  /**/

#undef  PRIVATE
#define PRIVATE /**/

#undef  FFPU
#define FFPU    /**/

#undef  FPU
#define FPU     fpu.

/* -------------------------------------------------------------------------- */
/* --- IEEE FPU core methods (used by ESP32)                              --- */
/* -------------------------------------------------------------------------- */

/* Return the floating-point status register in m68k format */
static inline uae_u32 FFPU get_fpsr(void)
{
    uae_u32 condition_codes   = get_fpccr();
    uae_u32 exception_status  = get_exception_status();
    uae_u32 accrued_exception = get_accrued_exception();
    uae_u32 quotient          = FPU fpsr.quotient;
    return (condition_codes | quotient | exception_status | accrued_exception);
}

/* Set the floating-point status register from an m68k format */
static inline void FFPU set_fpsr(uae_u32 new_fpsr)
{
    set_fpccr             ( new_fpsr & FPSR_CCB               );
    set_exception_status  ( new_fpsr & FPSR_EXCEPTION_STATUS  );
    set_accrued_exception ( new_fpsr & FPSR_ACCRUED_EXCEPTION );
    FPU fpsr.quotient     = new_fpsr & FPSR_QUOTIENT;
}

/* -------------------------------------------------------------------------- */
/* --- Control word routines                                              --- */
/* -------------------------------------------------------------------------- */

/* Return the floating-point control register in m68k format */
static inline uae_u32 FFPU get_fpcr(void)
{
    uae_u32 rounding_precision = get_rounding_precision();
    uae_u32 rounding_mode      = get_rounding_mode();
    return (rounding_precision | rounding_mode);
}

/* Set the floating-point control register from an m68k format */
static inline void FFPU set_fpcr(uae_u32 new_fpcr)
{
    set_rounding_precision ( new_fpcr & FPCR_ROUNDING_PRECISION );
    set_rounding_mode      ( new_fpcr & FPCR_ROUNDING_MODE      );
    set_host_control_word();
}

/* -------------------------------------------------------------------------- */
/* --- Register access                                                    --- */
/* -------------------------------------------------------------------------- */

/* Retrieve a floating-point register value and convert it to double precision */
static inline double FFPU fpu_get_register(int r)
{
    return FPU registers[r];
}

#endif /* FPU_IMPL_H */
