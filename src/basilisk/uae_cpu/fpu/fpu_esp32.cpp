/*
 *  fpu/fpu_esp32.cpp - Minimal FPU emulation stub for ESP32
 *
 *  BasiliskII ESP32 Port
 *
 *  This is a minimal stub that allows the emulator to compile without
 *  full FPU emulation. FPU instructions trigger F-line exceptions.
 */

#include "sysdeps.h"
#include "m68k.h"
#include "memory.h"
#include "readcpu.h"
#include "newcpu.h"
#include "fpu/fpu.h"

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

/* Global FPU context - dynamically allocated */
fpu_t *fpu_ptr = NULL;

/*
 *  FPU initialization
 */
void fpu_init(bool integral_68040)
{
    (void)integral_68040;
    
    // Allocate FPU context in PSRAM if not already allocated
    if (fpu_ptr == NULL) {
#ifdef ARDUINO
        fpu_ptr = (fpu_t *)heap_caps_malloc(sizeof(fpu_t), MALLOC_CAP_SPIRAM);
#else
        fpu_ptr = (fpu_t *)malloc(sizeof(fpu_t));
#endif
        if (fpu_ptr == NULL) {
            write_log("[FPU] ERROR: Failed to allocate FPU context\n");
            return;
        }
    }
    
    // Initialize FPU context to zeros
    memset(&fpu, 0, sizeof(fpu));
    
    // Set default FPCR values
    fpu.fpcr.rounding_precision = FPCR_PRECISION_EXTENDED;
    fpu.fpcr.rounding_mode = FPCR_ROUND_NEAR;
    
    write_log("[FPU] Initialized (stub - FPU disabled)\n");
}

/*
 *  FPU reset
 */
void fpu_reset(void)
{
    memset(&fpu, 0, sizeof(fpu));
    fpu.fpcr.rounding_precision = FPCR_PRECISION_EXTENDED;
    fpu.fpcr.rounding_mode = FPCR_ROUND_NEAR;
}

/*
 *  FPU exit
 */
void fpu_exit(void)
{
    // Nothing to do
}

/*
 *  FPU instruction handlers - trigger F-line exception
 */
void fpuop_arithmetic(uae_u32 opcode, uae_u32 extra)
{
    (void)opcode;
    (void)extra;
    Exception(11, 0);  // F-line exception
}

void fpuop_dbcc(uae_u32 opcode, uae_u32 extra)
{
    (void)opcode;
    (void)extra;
    Exception(11, 0);
}

void fpuop_scc(uae_u32 opcode, uae_u32 extra)
{
    (void)opcode;
    (void)extra;
    Exception(11, 0);
}

void fpuop_trapcc(uae_u32 opcode, uaecptr oldpc)
{
    (void)opcode;
    (void)oldpc;
    Exception(11, 0);
}

void fpuop_bcc(uae_u32 opcode, uaecptr pc, uae_u32 extra)
{
    (void)opcode;
    (void)pc;
    (void)extra;
    Exception(11, 0);
}

void fpuop_save(uae_u32 opcode)
{
    (void)opcode;
    // FSAVE with null frame - just write 0
    uae_u32 ad;
    int mode = (opcode >> 3) & 7;
    int reg = opcode & 7;
    
    if (mode == 3) {  // (An)+
        ad = m68k_areg(regs, reg);
        put_long(ad, 0);
        m68k_areg(regs, reg) = ad + 4;
    } else if (mode == 4) {  // -(An)
        ad = m68k_areg(regs, reg) - 4;
        put_long(ad, 0);
        m68k_areg(regs, reg) = ad;
    } else {
        // Other addressing modes - just return null frame
    }
}

void fpuop_restore(uae_u32 opcode)
{
    (void)opcode;
    // FRESTORE - ignore the frame and reset FPU
    fpu_reset();
}
