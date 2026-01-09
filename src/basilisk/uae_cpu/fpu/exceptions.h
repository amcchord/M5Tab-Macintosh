/*
 *  fpu/exceptions.h - FPU exception handling
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

#ifndef FPU_EXCEPTIONS_H
#define FPU_EXCEPTIONS_H

/* NOTE: this file shall be included only from fpu/fpu_*.cpp */
#undef  PUBLIC
#define PUBLIC  extern

#undef  PRIVATE
#define PRIVATE static

#undef  FFPU
#define FFPU    /**/

#undef  FPU
#define FPU     fpu.

/* ESP32 uses generic exceptions (no x86 assembly) */
#define FPU_USE_GENERIC_EXCEPTIONS
#define FPU_USE_GENERIC_ACCRUED_EXCEPTIONS

/* -------------------------------------------------------------------------- */
/* --- Generic Exceptions Handling                                        --- */
/* -------------------------------------------------------------------------- */

#ifdef FPU_USE_GENERIC_EXCEPTIONS

/* Initialize native exception management - nothing to do */
PRIVATE inline void FFPU fpu_init_native_exceptions(void)
    { }

/* Return m68k floating-point exception status */
PRIVATE inline uae_u32 FFPU get_exception_status(void)
    { return FPU fpsr.exception_status; }

/* Set new exception status. Assumes mask against FPSR_EXCEPTION to be already performed */
PRIVATE inline void FFPU set_exception_status(uae_u32 new_status)
    { FPU fpsr.exception_status = new_status; }

#endif /* FPU_USE_GENERIC_EXCEPTIONS */

#ifdef FPU_USE_GENERIC_ACCRUED_EXCEPTIONS

/* Initialize native accrued exception management - nothing to do */
PRIVATE inline void FFPU fpu_init_native_accrued_exceptions(void)
    { }

/* Return m68k accrued exception byte */
PRIVATE inline uae_u32 FFPU get_accrued_exception(void)
    { return FPU fpsr.accrued_exception; }

/* Set new accrued exception byte */
PRIVATE inline void FFPU set_accrued_exception(uae_u32 new_status)
    { FPU fpsr.accrued_exception = new_status; }

#endif /* FPU_USE_GENERIC_ACCRUED_EXCEPTIONS */

#endif /* FPU_EXCEPTIONS_H */
