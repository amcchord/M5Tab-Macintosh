/*
 * quickdraw_accel.h - Conservative native QuickDraw fast paths
 *
 * These helpers accelerate measured 1-bit and 8-bit indexed drawing paths.
 * Unsupported ports, regions, pixel formats, transfer modes, recording state,
 * and custom drawing procedures fall through to the original Macintosh
 * QuickDraw implementation before guest state is changed.
 */

#ifndef QUICKDRAW_ACCEL_H
#define QUICKDRAW_ACCEL_H

#include "sysdeps.h"

struct QuickDrawAccelStats {
    uint32 copy_attempts;
    uint32 copy_calls;
    uint32 copy_arg_failures;
    uint32 copy_bitmap_failures;
    uint32 copy_port_failures;
    uint32 copy_execution_failures;
    uint32 scroll_attempts;
    uint32 scroll_calls;
    uint32 scroll_arg_failures;
    uint32 scroll_port_failures;
    uint32 scroll_region_failures;
    uint32 scroll_background_failures;
    uint32 scroll_execution_failures;
    uint32 shape_attempts;
    uint32 shape_calls;
    uint32 shape_failures;
    uint32 move_attempts;
    uint32 move_calls;
    uint32 move_failures;
    uint32 line_attempts;
    uint32 line_calls;
    uint32 line_failures;
    uint32 port_address_failures;
    uint32 port_recording_failures;
    uint32 port_bitmap_failures;
    uint32 port_rect_failures;
    uint32 port_visible_region_failures;
    uint32 port_clip_region_failures;
    uint32 last_vis_region_size;
    uint32 last_clip_region_size;
    uint32 last_pic_save;
    uint32 last_rgn_save;
    uint32 last_poly_save;
    uint32 last_graf_procs;
    uint64 copy_bytes;
    uint64 scroll_bytes;
    uint64 shape_bytes;
    uint64 line_bytes;
};

bool QuickDrawAccelTryTrap(uint16 opcode);
void QuickDrawAccelResetStats(void);
void QuickDrawAccelReadStats(QuickDrawAccelStats *stats);
uint32 QuickDrawAccelReadRegionSnapshot(uint16 *words, uint32 capacity);

#endif
