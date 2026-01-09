/*
 *  fpu/fpu.h - FPU context definition and API
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

#ifndef FPU_H
#define FPU_H

#include "sysdeps.h"
#include "fpu/types.h"

/* ========================================================================== */
/* ========================= FPU CONTEXT DEFINITION ========================= */
/* ========================================================================== */

struct fpu_t {

    /* ---------------------------------------------------------------------- */
    /* --- Floating-Point Data Registers                                  --- */
    /* ---------------------------------------------------------------------- */
    
    /* The eight %fp0 .. %fp7 registers */
    fpu_register    registers[8];
    
    /* Used for lazy evaluation of FPU flags */
    fpu_register    result;
    
    /* ---------------------------------------------------------------------- */
    /* --- Floating-Point Control Register                                --- */
    /* ---------------------------------------------------------------------- */
    
    struct {
    
    /* Exception Enable Byte */
    uae_u32     exception_enable;
    #define     FPCR_EXCEPTION_ENABLE   0x0000ff00
    #define     FPCR_EXCEPTION_BSUN     0x00008000
    #define     FPCR_EXCEPTION_SNAN     0x00004000
    #define     FPCR_EXCEPTION_OPERR    0x00002000
    #define     FPCR_EXCEPTION_OVFL     0x00001000
    #define     FPCR_EXCEPTION_UNFL     0x00000800
    #define     FPCR_EXCEPTION_DZ       0x00000400
    #define     FPCR_EXCEPTION_INEX2    0x00000200
    #define     FPCR_EXCEPTION_INEX1    0x00000100
    
    /* Mode Control Byte Mask */
    #define     FPCR_MODE_CONTROL       0x000000ff
    
    /* Rounding precision */
    uae_u32     rounding_precision;
    #define     FPCR_ROUNDING_PRECISION 0x000000c0
    #define     FPCR_PRECISION_SINGLE   0x00000040
    #define     FPCR_PRECISION_DOUBLE   0x00000080
    #define     FPCR_PRECISION_EXTENDED 0x00000000
    
    /* Rounding mode */
    uae_u32     rounding_mode;
    #define     FPCR_ROUNDING_MODE      0x00000030
    #define     FPCR_ROUND_NEAR         0x00000000
    #define     FPCR_ROUND_ZERO         0x00000010
    #define     FPCR_ROUND_MINF         0x00000020
    #define     FPCR_ROUND_PINF         0x00000030
    
    }           fpcr;
    
    /* ---------------------------------------------------------------------- */
    /* --- Floating-Point Status Register                                 --- */
    /* ---------------------------------------------------------------------- */
    
    struct {
    
    /* Floating-Point Condition Code Byte */
    uae_u32     condition_codes;
    #define     FPSR_CCB                0xff000000
    #define     FPSR_CCB_NEGATIVE       0x08000000
    #define     FPSR_CCB_ZERO           0x04000000
    #define     FPSR_CCB_INFINITY       0x02000000
    #define     FPSR_CCB_NAN            0x01000000
    
    /* Quotient Byte */
    uae_u32     quotient;
    #define     FPSR_QUOTIENT           0x00ff0000
    #define     FPSR_QUOTIENT_SIGN      0x00800000
    #define     FPSR_QUOTIENT_VALUE     0x007f0000
    
    /* Exception Status Byte */
    uae_u32     exception_status;
    #define     FPSR_EXCEPTION_STATUS   FPCR_EXCEPTION_ENABLE
    #define     FPSR_EXCEPTION_BSUN     FPCR_EXCEPTION_BSUN
    #define     FPSR_EXCEPTION_SNAN     FPCR_EXCEPTION_SNAN
    #define     FPSR_EXCEPTION_OPERR    FPCR_EXCEPTION_OPERR
    #define     FPSR_EXCEPTION_OVFL     FPCR_EXCEPTION_OVFL
    #define     FPSR_EXCEPTION_UNFL     FPCR_EXCEPTION_UNFL
    #define     FPSR_EXCEPTION_DZ       FPCR_EXCEPTION_DZ
    #define     FPSR_EXCEPTION_INEX2    FPCR_EXCEPTION_INEX2
    #define     FPSR_EXCEPTION_INEX1    FPCR_EXCEPTION_INEX1
    
    /* Accrued Exception Byte */
    uae_u32     accrued_exception;
    #define     FPSR_ACCRUED_EXCEPTION  0x000000ff
    #define     FPSR_ACCR_IOP           0x00000080
    #define     FPSR_ACCR_OVFL          0x00000040
    #define     FPSR_ACCR_UNFL          0x00000020
    #define     FPSR_ACCR_DZ            0x00000010
    #define     FPSR_ACCR_INEX          0x00000008
    
    }           fpsr;
    
    /* ---------------------------------------------------------------------- */
    /* --- Floating-Point Instruction Address Register                    --- */
    /* ---------------------------------------------------------------------- */
    
    uae_u32     instruction_address;
    
    /* ---------------------------------------------------------------------- */
    /* --- Initialization / Finalization                                  --- */
    /* ---------------------------------------------------------------------- */
    
    /* Flag set if we emulate an integral 68040 FPU */
    bool        is_integral;
};

/* We handle only one global fpu */
extern fpu_t fpu;

/* Return the address of a particular register */
inline fpu_register * const fpu_register_address(int i)
    { return &fpu.registers[i]; }

/* Dump functions for m68k_dumpstate */
extern void fpu_dump_registers(void);
extern void fpu_dump_flags(void);

/* Accessors to FPU Control Register */
static inline uae_u32 get_fpcr(void);
static inline void set_fpcr(uae_u32 new_fpcr);

/* Accessors to FPU Status Register */
static inline uae_u32 get_fpsr(void);
static inline void set_fpsr(uae_u32 new_fpsr);
    
/* Accessors to FPU Instruction Address Register */
static inline uae_u32 get_fpiar()
    { return fpu.instruction_address; }
static inline void set_fpiar(uae_u32 new_fpiar)
    { fpu.instruction_address = new_fpiar; }

/* Initialization / Finalization */
extern void fpu_init(bool integral_68040);
extern void fpu_exit(void);
extern void fpu_reset(void);
    
/* Floating-point arithmetic instructions */
void fpuop_arithmetic(uae_u32 opcode, uae_u32 extra) REGPARAM;

/* Floating-point program control operations */
void fpuop_bcc(uae_u32 opcode, uaecptr pc, uae_u32 extra) REGPARAM;
void fpuop_dbcc(uae_u32 opcode, uae_u32 extra) REGPARAM;
void fpuop_scc(uae_u32 opcode, uae_u32 extra) REGPARAM;

/* Floating-point system control operations */
void fpuop_save(uae_u32 opcode) REGPARAM;
void fpuop_restore(uae_u32 opcode) REGPARAM;
void fpuop_trapcc(uae_u32 opcode, uaecptr oldpc) REGPARAM;

#endif /* FPU_H */
