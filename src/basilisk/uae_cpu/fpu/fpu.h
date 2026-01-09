/*
 *  fpu/fpu.h - Minimal FPU stub for ESP32
 *
 *  BasiliskII ESP32 Port
 */

#ifndef FPU_H
#define FPU_H

#include "sysdeps.h"

/* FPU register type - use double */
typedef double fpu_register;
typedef double fpu_double;
typedef float fpu_single;

/* Extended precision (use double on ESP32) */
typedef double fpu_extended;

/* FPU control register fields */
#define FPCR_ROUNDING_MODE     0x00000030
#define FPCR_ROUNDING_PRECISION 0x000000C0
#define FPCR_ROUND_NEAR        0x00000000
#define FPCR_ROUND_ZERO        0x00000010
#define FPCR_ROUND_MINF        0x00000020
#define FPCR_ROUND_PINF        0x00000030
#define FPCR_PRECISION_SINGLE  0x00000040
#define FPCR_PRECISION_DOUBLE  0x00000080
#define FPCR_PRECISION_EXTENDED 0x00000000

/* FPSR Condition Code Byte */
#define FPSR_CCB               0xFF000000
#define FPSR_CCB_NEGATIVE      0x08000000
#define FPSR_CCB_ZERO          0x04000000
#define FPSR_CCB_INFINITY      0x02000000
#define FPSR_CCB_NAN           0x01000000

/* FPSR Exception Status Byte */
#define FPSR_EXCEPTION_STATUS  0x0000FF00
#define FPSR_EXCEPTION_BSUN    0x00008000
#define FPSR_EXCEPTION_SNAN    0x00004000
#define FPSR_EXCEPTION_OPERR   0x00002000
#define FPSR_EXCEPTION_OVFL    0x00001000
#define FPSR_EXCEPTION_UNFL    0x00000800
#define FPSR_EXCEPTION_DZ      0x00000400
#define FPSR_EXCEPTION_INEX2   0x00000200
#define FPSR_EXCEPTION_INEX1   0x00000100

/* FPSR Accrued Exception Byte */
#define FPSR_ACCRUED_EXCEPTION 0x000000F8

/* FPU context structure */
struct fpu_t {
    fpu_register registers[8];  /* fp0-fp7 */
    fpu_register result;
    
    struct {
        uae_u32 exception_enable;
        uae_u32 rounding_mode;
        uae_u32 rounding_precision;
    } fpcr;
    
    uae_u32 fpsr;
    uae_u32 fpiar;
    
    uae_u32 instruction_address;
    bool is_integral;
};

/* Global FPU context - dynamically allocated in PSRAM */
extern fpu_t *fpu_ptr;
#define fpu (*fpu_ptr)

/* FPU functions */
extern void fpu_init(bool integral_68040);
extern void fpu_exit(void);
extern void fpu_reset(void);

/* FPU instruction handlers */
extern void fpuop_arithmetic(uae_u32 opcode, uae_u32 extra);
extern void fpuop_dbcc(uae_u32 opcode, uae_u32 extra);
extern void fpuop_scc(uae_u32 opcode, uae_u32 extra);
extern void fpuop_trapcc(uae_u32 opcode, uaecptr oldpc);
extern void fpuop_bcc(uae_u32 opcode, uaecptr pc, uae_u32 extra);
extern void fpuop_save(uae_u32 opcode);
extern void fpuop_restore(uae_u32 opcode);

/* Debug functions (stubs) */
static inline void fpu_dump_registers(void) {}
static inline void fpu_dump_flags(void) {}

#endif /* FPU_H */
