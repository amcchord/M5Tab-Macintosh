/*
 *  fpu/mathlib.h - Floating-point math support library
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

#ifndef FPU_MATHLIB_H
#define FPU_MATHLIB_H

/* NOTE: this file shall be included only from fpu/fpu_*.cpp */
#undef  PUBLIC
#define PUBLIC  extern

#undef  PRIVATE
#define PRIVATE static

#undef  FFPU
#define FFPU    /**/

#undef  FPU
#define FPU     fpu.

#include <cmath>

/* -------------------------------------------------------------------------- */
/* --- Floating-point register types                                      --- */
/* -------------------------------------------------------------------------- */

// Single : S 8*E 23*F
#define FP_SINGLE_EXP_MAX       0xff
#define FP_SINGLE_EXP_BIAS      0x7f

// Double : S 11*E 52*F
#define FP_DOUBLE_EXP_MAX       0x7ff
#define FP_DOUBLE_EXP_BIAS      0x3ff

// Extended : S 15*E 64*F (80-bit, emulated with double on ESP32)
#define FP_EXTENDED_EXP_MAX     0x7fff
#define FP_EXTENDED_EXP_BIAS    0x3fff

/* -------------------------------------------------------------------------- */
/* --- Floating-point type shapes (IEEE-compliant)                        --- */
/* -------------------------------------------------------------------------- */

// IEEE-754 float format
union fpu_single_shape {
    fpu_single value;
    
    struct {
        // ESP32 is little-endian
        unsigned int mantissa:23;
        unsigned int exponent:8;
        unsigned int negative:1;
    } ieee;

    struct {
        unsigned int mantissa:22;
        unsigned int quiet_nan:1;
        unsigned int exponent:8;
        unsigned int negative:1;
    } ieee_nan;
};

// IEEE-754 double format
union fpu_double_shape {
    fpu_double value;
    
    struct {
        // ESP32 is little-endian
        unsigned int mantissa1:32;
        unsigned int mantissa0:20;
        unsigned int exponent:11;
        unsigned int negative:1;
    } ieee;

    struct {
        unsigned int mantissa1:32;
        unsigned int mantissa0:19;
        unsigned int quiet_nan:1;
        unsigned int exponent:11;
        unsigned int negative:1;
    } ieee_nan;

    struct {
        unsigned int lsw:32;
        unsigned int msw:32;
    } parts;
};

// Declare and initialize a pointer to a shape of the requested FP type
#define fp_declare_init_shape(psvar, rfvar, ftype) \
    fpu_ ## ftype ## _shape * psvar = (fpu_ ## ftype ## _shape *)( &rfvar )

/* -------------------------------------------------------------------------- */
/* --- Extra Math Functions                                               --- */
/* -------------------------------------------------------------------------- */

#undef isnan
#define isnan(x) fp_do_isnan((x))

PRIVATE inline bool FFPU fp_do_isnan(fpu_register const & r)
{
    fp_declare_init_shape(sxp, r, double);
    return (sxp->ieee_nan.exponent == FP_DOUBLE_EXP_MAX)
        && ((sxp->ieee_nan.mantissa0 != 0) || (sxp->ieee_nan.mantissa1 != 0));
}

#undef isinf
#define isinf(x) fp_do_isinf((x))

PRIVATE inline bool FFPU fp_do_isinf(fpu_register const & r)
{
    fp_declare_init_shape(sxp, r, double);
    return (sxp->ieee_nan.exponent == FP_DOUBLE_EXP_MAX)
        && (sxp->ieee_nan.mantissa0 == 0)
        && (sxp->ieee_nan.mantissa1 == 0);
}

#undef isneg
#define isneg(x) fp_do_isneg((x))

PRIVATE inline bool FFPU fp_do_isneg(fpu_register const & r)
{
    fp_declare_init_shape(sxp, r, double);
    return sxp->ieee.negative;
}

#undef iszero
#define iszero(x) fp_do_iszero((x))

PRIVATE inline bool FFPU fp_do_iszero(fpu_register const & r)
{
    fp_declare_init_shape(sxp, r, double);
    return (sxp->ieee.exponent == 0)
        && (sxp->ieee.mantissa0 == 0)
        && (sxp->ieee.mantissa1 == 0);
}

/* Floating-point flags structure for operand analysis */
struct fp_flags {
    bool negative;
    bool zero;
    bool infinity;
    bool nan;
    bool in_range;
};

PRIVATE fp_flags fl_source;
PRIVATE fp_flags fl_dest;

PRIVATE inline void FFPU get_dest_flags(fpu_register const & r)
{
    fl_dest.negative = isneg(r);
    fl_dest.zero     = iszero(r);
    fl_dest.infinity = isinf(r);
    fl_dest.nan      = isnan(r);
    fl_dest.in_range = !fl_dest.zero && !fl_dest.infinity && !fl_dest.nan;
}

PRIVATE inline void FFPU get_source_flags(fpu_register const & r)
{
    fl_source.negative = isneg(r);
    fl_source.zero     = iszero(r);
    fl_source.infinity = isinf(r);
    fl_source.nan      = isnan(r);
    fl_source.in_range = !fl_source.zero && !fl_source.infinity && !fl_source.nan;
}

PRIVATE inline void FFPU make_nan(fpu_register & r)
{
    fp_declare_init_shape(sxp, r, double);
    sxp->ieee.exponent  = FP_DOUBLE_EXP_MAX;
    sxp->ieee.mantissa0 = 0xfffff;
    sxp->ieee.mantissa1 = 0xffffffff;
}

PRIVATE inline void FFPU make_zero_positive(fpu_register & r)
{
    r = +0.0;
}

PRIVATE inline void FFPU make_zero_negative(fpu_register & r)
{
    r = -0.0;
}

PRIVATE inline void FFPU make_inf_positive(fpu_register & r)
{
    fp_declare_init_shape(sxp, r, double);
    sxp->ieee_nan.exponent  = FP_DOUBLE_EXP_MAX;
    sxp->ieee_nan.negative  = 0;
    sxp->ieee_nan.mantissa0 = 0;
    sxp->ieee_nan.mantissa1 = 0;
}

PRIVATE inline void FFPU make_inf_negative(fpu_register & r)
{
    fp_declare_init_shape(sxp, r, double);
    sxp->ieee_nan.exponent  = FP_DOUBLE_EXP_MAX;
    sxp->ieee_nan.negative  = 1;
    sxp->ieee_nan.mantissa0 = 0;
    sxp->ieee_nan.mantissa1 = 0;
}

PRIVATE inline fpu_register FFPU fast_fgetexp(fpu_register const & r)
{
    fp_declare_init_shape(sxp, r, double);
    return (sxp->ieee.exponent - FP_DOUBLE_EXP_BIAS);
}

// Normalize to range 1..2
PRIVATE inline void FFPU fast_remove_exponent(fpu_register & r)
{
    fp_declare_init_shape(sxp, r, double);
    sxp->ieee.exponent = FP_DOUBLE_EXP_BIAS;
}

// The sign of the quotient is the exclusive-OR of the sign bits
PRIVATE inline uae_u32 FFPU get_quotient_sign(fpu_register const & ra, fpu_register const & rb)
{
    fp_declare_init_shape(sap, ra, double);
    fp_declare_init_shape(sbp, rb, double);
    return ((sap->ieee.negative ^ sbp->ieee.negative) ? FPSR_QUOTIENT_SIGN : 0);
}

/* -------------------------------------------------------------------------- */
/* --- Math functions - use standard C library                            --- */
/* -------------------------------------------------------------------------- */

#define fp_log      log
#define fp_log10    log10
#define fp_exp      exp
#define fp_pow      pow
#define fp_fabs     fabs
#define fp_sqrt     sqrt
#define fp_sin      sin
#define fp_cos      cos
#define fp_tan      tan
#define fp_sinh     sinh
#define fp_cosh     cosh
#define fp_tanh     tanh
#define fp_asin     asin
#define fp_acos     acos
#define fp_atan     atan
#define fp_asinh    asinh
#define fp_acosh    acosh
#define fp_atanh    atanh
#define fp_floor    floor
#define fp_ceil     ceil

/* Rounding functions */
#define fp_round_to_minus_infinity(x) fp_floor(x)
#define fp_round_to_plus_infinity(x)  fp_ceil(x)
#define fp_round_to_zero(x)           trunc(x)
#define fp_round_to_nearest(x)        round(x)

#endif /* FPU_MATHLIB_H */
