/*
 * quickdraw_accel.cpp - Conservative native QuickDraw fast paths
 *
 * The implementation follows the classic QuickDraw Pascal trap ABI and handles
 * measured 1-bit and 8-bit indexed operations with exact visible/clip-region
 * intersections. It preserves the original QuickDraw handler as a fallback.
 */

#include <string.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "quickdraw_accel.h"
#include "video.h"
#include "uae_cpu/memory.h"
#include "uae_cpu/newcpu.h"

#ifdef ARDUINO
#include <esp_attr.h>
#endif

namespace {

constexpr uint16 kCopyBitsTrap = 0xA8EC;
constexpr uint16 kScrollRectTrap = 0xA8EF;
constexpr uint16 kLineToTrap = 0xA891;
constexpr uint16 kMoveToTrap = 0xA893;
constexpr uint16 kFrameRectTrap = 0xA8A1;
constexpr uint16 kPaintRectTrap = 0xA8A2;
constexpr uint16 kFrameOvalTrap = 0xA8B7;
constexpr uint16 kPaintOvalTrap = 0xA8B8;
constexpr uint16 kColorPortVersionMask = 0xC000;
constexpr uint16 kPatternCopyMode = 8;

struct QDRect {
    int32 top;
    int32 left;
    int32 bottom;
    int32 right;
};

struct NativeBitmap {
    uint32 record;
    uint32 base;
    uint32 row_bytes;
    QDRect bounds;
    uint16 depth;
};

struct NativeSpan {
    uint8 *host;
    bool frame;
    uint32 frame_offset;
};

struct NativeRegion {
    uint32 record;
    uint32 size;
    QDRect bounds;
    bool simple;
};

struct NativePort {
    uint32 record;
    NativeBitmap bitmap;
    QDRect bounds;
    NativeRegion visible;
    NativeRegion clipping;
};

constexpr uint32 kMaxRegionEndpoints = 32;

struct XIntervals {
    int32 endpoints[kMaxRegionEndpoints];
    uint32 count;
};

struct NativePattern {
    uint8 rows[8];
    uint8 foreground;
    uint8 background;
};

#ifdef ARDUINO
DRAM_ATTR
#endif
static QuickDrawAccelStats s_stats = {};
static uint16 s_region_snapshot[64] = {};
static uint32 s_region_snapshot_words = 0;

static inline bool guestRamRange(uint32 address, uint32 size)
{
    return address < RAMSize && size <= RAMSize - address;
}

static bool readRect(uint32 address, QDRect &rect)
{
    if (!guestRamRange(address, 8)) return false;
    rect.top = (int16)get_word(address + 0);
    rect.left = (int16)get_word(address + 2);
    rect.bottom = (int16)get_word(address + 4);
    rect.right = (int16)get_word(address + 6);
    return true;
}

static inline bool rectValid(const QDRect &rect)
{
    return rect.left < rect.right && rect.top < rect.bottom;
}

static bool intersectRect(QDRect &rect, const QDRect &clip)
{
    if (rect.left < clip.left) rect.left = clip.left;
    if (rect.top < clip.top) rect.top = clip.top;
    if (rect.right > clip.right) rect.right = clip.right;
    if (rect.bottom > clip.bottom) rect.bottom = clip.bottom;
    return rectValid(rect);
}

static bool readSimpleRegion(uint32 handle, QDRect &bounds)
{
    if (!guestRamRange(handle, 4)) return false;
    const uint32 region = get_long(handle);
    if (!guestRamRange(region, 10) || get_word(region) != 10) return false;
    return readRect(region + 2, bounds);
}

static void captureRegionSnapshot(uint32 handle)
{
    if (s_region_snapshot_words != 0 || !guestRamRange(handle, 4)) return;
    const uint32 region = get_long(handle);
    if (!guestRamRange(region, 10)) return;
    const uint32 bytes = (uint16)get_word(region);
    if (bytes < 10 || !guestRamRange(region, bytes)) return;
    const uint32 words = bytes / 2 < 64 ? bytes / 2 : 64;
    for (uint32 index = 0; index < words; ++index)
        s_region_snapshot[index] = (uint16)get_word(region + index * 2);
    s_region_snapshot_words = words;
}

static bool readRegion(uint32 handle, NativeRegion &native)
{
    if (!guestRamRange(handle, 4)) return false;
    const uint32 region = get_long(handle);
    if (!guestRamRange(region, 10)) return false;
    const uint32 size = (uint16)get_word(region);
    if (size < 10 || (size & 1) != 0 || !guestRamRange(region, size) ||
        !readRect(region + 2, native.bounds)) {
        return false;
    }

    native.record = region;
    native.size = size;
    native.simple = size == 10;
    if (native.simple) return true;

    /* Validate the documented inversion-point stream once up front. Each
     * boundary is y, an even count of ascending x values, then 0x7fff; a
     * final y=0x7fff terminates the region. */
    uint32 cursor = region + 10;
    const uint32 end = region + size;
    int32 previous_y = INT32_MIN;
    while (cursor + 2 <= end) {
        const uint16 y_word = (uint16)get_word(cursor);
        cursor += 2;
        if (y_word == 0x7fff) return cursor == end;
        const int32 y = (int16)y_word;
        if (y < previous_y) return false;
        previous_y = y;
        uint32 x_count = 0;
        int32 previous_x = INT32_MIN;
        bool terminated = false;
        while (cursor + 2 <= end) {
            const uint16 x_word = (uint16)get_word(cursor);
            cursor += 2;
            if (x_word == 0x7fff) {
                terminated = true;
                break;
            }
            const int32 x = (int16)x_word;
            if (x < previous_x) return false;
            previous_x = x;
            ++x_count;
        }
        if (!terminated || (x_count & 1) != 0) return false;
    }
    return false;
}

static bool toggleEndpoint(XIntervals &intervals, int32 value)
{
    uint32 position = 0;
    while (position < intervals.count && intervals.endpoints[position] < value)
        ++position;
    if (position < intervals.count && intervals.endpoints[position] == value) {
        for (uint32 index = position + 1; index < intervals.count; ++index)
            intervals.endpoints[index - 1] = intervals.endpoints[index];
        --intervals.count;
        return true;
    }
    if (intervals.count >= kMaxRegionEndpoints) return false;
    for (uint32 index = intervals.count; index > position; --index)
        intervals.endpoints[index] = intervals.endpoints[index - 1];
    intervals.endpoints[position] = value;
    ++intervals.count;
    return true;
}

static bool regionIntervalsAtY(const NativeRegion &region, int32 y,
                               XIntervals &intervals)
{
    intervals.count = 0;
    if (y < region.bounds.top || y >= region.bounds.bottom) return true;
    if (region.simple) {
        if (!rectValid(region.bounds)) return true;
        intervals.endpoints[0] = region.bounds.left;
        intervals.endpoints[1] = region.bounds.right;
        intervals.count = 2;
        return true;
    }

    uint32 cursor = region.record + 10;
    const uint32 end = region.record + region.size;
    while (cursor + 2 <= end) {
        const uint16 y_word = (uint16)get_word(cursor);
        cursor += 2;
        if (y_word == 0x7fff) break;
        const int32 boundary_y = (int16)y_word;
        if (boundary_y > y) break;
        while (cursor + 2 <= end) {
            const uint16 x_word = (uint16)get_word(cursor);
            cursor += 2;
            if (x_word == 0x7fff) break;
            if (!toggleEndpoint(intervals, (int16)x_word)) return false;
        }
    }
    return (intervals.count & 1) == 0;
}

static bool intersectIntervals(const XIntervals &a, const XIntervals &b,
                               XIntervals &out)
{
    out.count = 0;
    uint32 ai = 0;
    uint32 bi = 0;
    while (ai + 1 < a.count && bi + 1 < b.count) {
        const int32 left = a.endpoints[ai] > b.endpoints[bi]
                               ? a.endpoints[ai] : b.endpoints[bi];
        const int32 right = a.endpoints[ai + 1] < b.endpoints[bi + 1]
                                ? a.endpoints[ai + 1] : b.endpoints[bi + 1];
        if (left < right) {
            if (out.count + 2 > kMaxRegionEndpoints) return false;
            out.endpoints[out.count++] = left;
            out.endpoints[out.count++] = right;
        }
        if (a.endpoints[ai + 1] < b.endpoints[bi + 1]) ai += 2;
        else bi += 2;
    }
    return true;
}

static void clampIntervals(XIntervals &intervals, int32 left, int32 right)
{
    uint32 output = 0;
    for (uint32 index = 0; index + 1 < intervals.count; index += 2) {
        const int32 clipped_left = intervals.endpoints[index] > left
                                       ? intervals.endpoints[index] : left;
        const int32 clipped_right = intervals.endpoints[index + 1] < right
                                        ? intervals.endpoints[index + 1] : right;
        if (clipped_left < clipped_right) {
            intervals.endpoints[output++] = clipped_left;
            intervals.endpoints[output++] = clipped_right;
        }
    }
    intervals.count = output;
}

static bool writeSimpleRegion(uint32 handle, const QDRect &bounds)
{
    if (!guestRamRange(handle, 4)) return false;
    const uint32 region = get_long(handle);
    if (!guestRamRange(region, 10)) return false;
    put_word(region + 0, 10);
    put_word(region + 2, (uint16)bounds.top);
    put_word(region + 4, (uint16)bounds.left);
    put_word(region + 6, (uint16)bounds.bottom);
    put_word(region + 8, (uint16)bounds.right);
    return true;
}

static bool readBitmap(uint32 address, NativeBitmap &bitmap)
{
    if (!guestRamRange(address, 14)) return false;

    uint32 record = address;
    uint16 row_word = (uint16)get_word(record + 4);

    /*
     * A CGrafPort exposes a compatibility BitMap at port+2. Its baseAddr is
     * actually a PixMapHandle and its rowBytes slot is portVersion (0xc000).
     * Resolve that indirection before parsing a normal PixMap record.
     */
    if ((row_word & kColorPortVersionMask) == kColorPortVersionMask) {
        const uint32 pixmap_handle = get_long(record);
        if (!guestRamRange(pixmap_handle, 4)) return false;
        record = get_long(pixmap_handle);
        if (!guestRamRange(record, 50)) return false;
        row_word = (uint16)get_word(record + 4);
    }

    const uint32 row_bytes = row_word & 0x3fff;
    if (row_bytes == 0 || row_bytes > 16384) return false;

    QDRect bounds;
    if (!readRect(record + 6, bounds) || !rectValid(bounds)) return false;

    uint16 depth = 1;
    if (row_word & 0x8000) {
        if (!guestRamRange(record, 50)) return false;
        depth = (uint16)get_word(record + 32);
    }
    if (depth != 1 && depth != 8) return false;
    const uint32 width = (uint32)(bounds.right - bounds.left);
    const uint32 required_row_bytes = depth == 1 ? (width + 7) / 8 : width;
    if (required_row_bytes > row_bytes) return false;

    bitmap.record = record;
    bitmap.base = get_long(record);
    bitmap.row_bytes = row_bytes;
    bitmap.bounds = bounds;
    bitmap.depth = depth;
    return true;
}

static bool translateSpan(const NativeBitmap &bitmap, int32 x, int32 y,
                          uint32 length, NativeSpan &span)
{
    if (bitmap.depth != 8) return false;
    if (x < bitmap.bounds.left || x >= bitmap.bounds.right ||
        y < bitmap.bounds.top || y >= bitmap.bounds.bottom || length == 0 ||
        length > (uint32)(bitmap.bounds.right - x)) {
        return false;
    }

    const uint64 offset =
        (uint64)(uint32)(y - bitmap.bounds.top) * bitmap.row_bytes +
        (uint32)(x - bitmap.bounds.left);
    if (offset > UINT32_MAX) return false;
    const uint32 guest = bitmap.base + (uint32)offset;
    if (guest < bitmap.base) return false;

    if (guestRamRange(guest, length)) {
        span.host = RAMBaseHost + guest;
        span.frame = false;
        span.frame_offset = 0;
        return true;
    }
    if (MacFrameLayout == FLAYOUT_DIRECT && guest >= MacFrameBaseMac) {
        const uint32 frame_offset = guest - MacFrameBaseMac;
        if (frame_offset < MacFrameSize && length <= MacFrameSize - frame_offset) {
            span.host = MacFrameBaseHost + frame_offset;
            span.frame = true;
            span.frame_offset = frame_offset;
            return true;
        }
    }
    return false;
}

static bool translateBitmapBytes(const NativeBitmap &bitmap, int32 y,
                                 uint32 byte_offset, uint32 length,
                                 NativeSpan &span)
{
    if (y < bitmap.bounds.top || y >= bitmap.bounds.bottom || length == 0 ||
        byte_offset > bitmap.row_bytes ||
        length > bitmap.row_bytes - byte_offset) {
        return false;
    }
    const uint64 offset =
        (uint64)(uint32)(y - bitmap.bounds.top) * bitmap.row_bytes + byte_offset;
    if (offset > UINT32_MAX) return false;
    const uint32 guest = bitmap.base + (uint32)offset;
    if (guest < bitmap.base) return false;
    if (guestRamRange(guest, length)) {
        span.host = RAMBaseHost + guest;
        span.frame = false;
        span.frame_offset = 0;
        return true;
    }
    if (MacFrameLayout == FLAYOUT_DIRECT && guest >= MacFrameBaseMac) {
        const uint32 frame_offset = guest - MacFrameBaseMac;
        if (frame_offset < MacFrameSize && length <= MacFrameSize - frame_offset) {
            span.host = MacFrameBaseHost + frame_offset;
            span.frame = true;
            span.frame_offset = frame_offset;
            return true;
        }
    }
    return false;
}

static bool translatePixelRange(const NativeBitmap &bitmap, int32 x, int32 y,
                                uint32 length, NativeSpan &span,
                                uint32 &first_bit, uint32 &byte_count)
{
    if (x < bitmap.bounds.left || x >= bitmap.bounds.right ||
        y < bitmap.bounds.top || y >= bitmap.bounds.bottom || length == 0 ||
        length > (uint32)(bitmap.bounds.right - x)) {
        return false;
    }
    if (bitmap.depth == 8) {
        first_bit = 0;
        byte_count = length;
        return translateSpan(bitmap, x, y, length, span);
    }
    if (bitmap.depth != 1) return false;
    const uint32 pixel = (uint32)(x - bitmap.bounds.left);
    const uint32 byte_offset = pixel >> 3;
    first_bit = pixel & 7;
    byte_count = (first_bit + length + 7) >> 3;
    return translateBitmapBytes(bitmap, y, byte_offset, byte_count, span);
}

static bool copyPixelRange(const NativeBitmap &source_bitmap,
                           const NativeBitmap &destination_bitmap,
                           int32 source_x, int32 source_y,
                           int32 destination_x, int32 destination_y,
                           uint32 width, bool right_to_left,
                           uint64 &bytes_copied)
{
    if (source_bitmap.depth != destination_bitmap.depth) return false;
    NativeSpan source;
    NativeSpan destination;
    uint32 source_bit = 0;
    uint32 destination_bit = 0;
    uint32 source_bytes = 0;
    uint32 destination_bytes = 0;
    if (!translatePixelRange(source_bitmap, source_x, source_y, width,
                             source, source_bit, source_bytes) ||
        !translatePixelRange(destination_bitmap, destination_x, destination_y,
                             width, destination, destination_bit,
                             destination_bytes)) {
        return false;
    }
    if (source_bitmap.depth == 8) {
        memmove(destination.host, source.host, width);
        bytes_copied += width;
    } else {
        auto copy_one = [&](uint32 index) {
            const uint32 source_index = source_bit + index;
            const uint32 destination_index = destination_bit + index;
            const uint8 source_mask = (uint8)(0x80U >> (source_index & 7));
            const uint8 destination_mask =
                (uint8)(0x80U >> (destination_index & 7));
            if (source.host[source_index >> 3] & source_mask)
                destination.host[destination_index >> 3] |= destination_mask;
            else
                destination.host[destination_index >> 3] &=
                    (uint8)~destination_mask;
        };
        auto copy_eight = [&](uint32 index) {
            const uint32 source_index = source_bit + index;
            const uint32 source_byte = source_index >> 3;
            uint16 pair = (uint16)source.host[source_byte] << 8;
            if (source_byte + 1 < source_bytes)
                pair |= source.host[source_byte + 1];
            const uint8 value =
                (uint8)((pair << (source_index & 7)) >> 8);
            destination.host[(destination_bit + index) >> 3] = value;
        };

        if (!right_to_left) {
            uint32 index = 0;
            while (index < width && ((destination_bit + index) & 7) != 0)
                copy_one(index++);
            while (index + 8 <= width) {
                copy_eight(index);
                index += 8;
            }
            while (index < width) copy_one(index++);
        } else {
            uint32 index = width;
            while (index > 0 && ((destination_bit + index) & 7) != 0) {
                --index;
                copy_one(index);
            }
            while (index >= 8) {
                index -= 8;
                copy_eight(index);
            }
            while (index > 0) {
                --index;
                copy_one(index);
            }
        }
        bytes_copied += destination_bytes;
    }
    if (destination.frame)
        VideoMarkDirtyRange(destination.frame_offset, destination_bytes);
    return true;
}

static void markFrameRows(const NativeBitmap &bitmap, const QDRect &rect)
{
    NativeSpan first;
    NativeSpan last;
    const uint32 width = (uint32)(rect.right - rect.left);
    uint32 first_bit = 0;
    uint32 last_bit = 0;
    uint32 first_bytes = 0;
    uint32 last_bytes = 0;
    if (!translatePixelRange(bitmap, rect.left, rect.top, width, first,
                             first_bit, first_bytes) || !first.frame)
        return;
    if (!translatePixelRange(bitmap, rect.left, rect.bottom - 1, width, last,
                             last_bit, last_bytes) || !last.frame)
        return;
    const uint32 end = last.frame_offset + last_bytes;
    if (end > first.frame_offset)
        VideoMarkDirtyRange(first.frame_offset, end - first.frame_offset);
}

static bool readCurrentPort(NativePort &native)
{
    /* InitGraf stores the address of the per-process QuickDraw globals at
     * (A5). The first global is thePort itself. Low memory 0x0904 is merely
     * CurrentA5; treating it as the port produced a pointer into 68k code. */
    const uint32 a5 = m68k_areg(regs, 5);
    if (!guestRamRange(a5, 4)) {
        ++s_stats.port_address_failures;
        return false;
    }
    const uint32 qd_globals = get_long(a5);
    if (!guestRamRange(qd_globals, 4)) {
        ++s_stats.port_address_failures;
        return false;
    }
    const uint32 port = get_long(qd_globals);
    if (!guestRamRange(port, 108)) {
        ++s_stats.port_address_failures;
        return false;
    }

    /* Picture/region/poly recording and custom GrafProcs require QuickDraw. */
    s_stats.last_pic_save = get_long(port + 92);
    s_stats.last_rgn_save = get_long(port + 96);
    s_stats.last_poly_save = get_long(port + 100);
    s_stats.last_graf_procs = get_long(port + 104);
    if (s_stats.last_pic_save != 0 || s_stats.last_rgn_save != 0 ||
        s_stats.last_poly_save != 0 || s_stats.last_graf_procs != 0) {
        ++s_stats.port_recording_failures;
        return false;
    }
    if (!readBitmap(port + 2, native.bitmap)) {
        ++s_stats.port_bitmap_failures;
        return false;
    }
    if (!readRect(port + 16, native.bounds) || !rectValid(native.bounds) ||
        !intersectRect(native.bounds, native.bitmap.bounds)) {
        ++s_stats.port_rect_failures;
        return false;
    }

    const uint32 visible_handle = get_long(port + 24);
    const uint32 clipping_handle = get_long(port + 28);
    if (guestRamRange(visible_handle, 4) && guestRamRange(get_long(visible_handle), 2))
        s_stats.last_vis_region_size = (uint16)get_word(get_long(visible_handle));
    if (guestRamRange(clipping_handle, 4) && guestRamRange(get_long(clipping_handle), 2))
        s_stats.last_clip_region_size = (uint16)get_word(get_long(clipping_handle));
    if (!readRegion(visible_handle, native.visible)) {
        captureRegionSnapshot(visible_handle);
        ++s_stats.port_visible_region_failures;
        return false;
    }
    if (!readRegion(clipping_handle, native.clipping)) {
        ++s_stats.port_clip_region_failures;
        return false;
    }
    native.record = port;
    return true;
}

static bool portIntervalsAtY(const NativePort &port, const QDRect &area,
                             int32 y, XIntervals &intervals)
{
    intervals.count = 0;
    if (y < area.top || y >= area.bottom ||
        y < port.bounds.top || y >= port.bounds.bottom) {
        return true;
    }
    XIntervals visible;
    XIntervals clipping;
    if (!regionIntervalsAtY(port.visible, y, visible) ||
        !regionIntervalsAtY(port.clipping, y, clipping) ||
        !intersectIntervals(visible, clipping, intervals)) {
        return false;
    }
    const int32 left = area.left > port.bounds.left ? area.left : port.bounds.left;
    const int32 right = area.right < port.bounds.right ? area.right : port.bounds.right;
    clampIntervals(intervals, left, right);
    return true;
}

static bool simpleCurrentPort(uint32 &port, NativeBitmap &bitmap, QDRect &clip)
{
    NativePort native;
    if (!readCurrentPort(native)) return false;
    if (!native.visible.simple) {
        captureRegionSnapshot(get_long(native.record + 24));
        ++s_stats.port_visible_region_failures;
        return false;
    }
    if (!native.clipping.simple) {
        ++s_stats.port_clip_region_failures;
        return false;
    }
    port = native.record;
    bitmap = native.bitmap;
    clip = native.bounds;
    return intersectRect(clip, native.visible.bounds) &&
           intersectRect(clip, native.clipping.bounds);
}

static bool sameBitmap(const NativeBitmap &a, const NativeBitmap &b)
{
    return a.base == b.base && a.row_bytes == b.row_bytes &&
           a.depth == b.depth &&
           a.bounds.top == b.bounds.top && a.bounds.left == b.bounds.left &&
           a.bounds.bottom == b.bounds.bottom && a.bounds.right == b.bounds.right;
}

static bool copyRectBytes(const NativeBitmap &src_bitmap,
                          const NativeBitmap &dst_bitmap,
                          const QDRect &src_rect, const QDRect &dst_rect,
                          uint64 &bytes_copied)
{
    const int32 original_width = src_rect.right - src_rect.left;
    const int32 original_height = src_rect.bottom - src_rect.top;
    if (original_width <= 0 || original_height <= 0 ||
        src_bitmap.depth != dst_bitmap.depth ||
        original_width != dst_rect.right - dst_rect.left ||
        original_height != dst_rect.bottom - dst_rect.top) {
        return false;
    }

    /* Clip both rectangles while keeping source and destination aligned. */
    int32 x0 = 0;
    int32 y0 = 0;
    int32 x1 = original_width;
    int32 y1 = original_height;
    if (src_rect.left + x0 < src_bitmap.bounds.left)
        x0 = src_bitmap.bounds.left - src_rect.left;
    if (dst_rect.left + x0 < dst_bitmap.bounds.left)
        x0 = dst_bitmap.bounds.left - dst_rect.left;
    if (src_rect.top + y0 < src_bitmap.bounds.top)
        y0 = src_bitmap.bounds.top - src_rect.top;
    if (dst_rect.top + y0 < dst_bitmap.bounds.top)
        y0 = dst_bitmap.bounds.top - dst_rect.top;
    if (src_rect.left + x1 > src_bitmap.bounds.right)
        x1 = src_bitmap.bounds.right - src_rect.left;
    if (dst_rect.left + x1 > dst_bitmap.bounds.right)
        x1 = dst_bitmap.bounds.right - dst_rect.left;
    if (src_rect.top + y1 > src_bitmap.bounds.bottom)
        y1 = src_bitmap.bounds.bottom - src_rect.top;
    if (dst_rect.top + y1 > dst_bitmap.bounds.bottom)
        y1 = dst_bitmap.bounds.bottom - dst_rect.top;
    if (x0 >= x1 || y0 >= y1) {
        bytes_copied = 0;
        return true;
    }

    const uint32 width = (uint32)(x1 - x0);
    const int32 height = y1 - y0;
    const int32 src_y = src_rect.top + y0;
    const int32 dst_y = dst_rect.top + y0;
    const bool bottom_up = sameBitmap(src_bitmap, dst_bitmap) && dst_y > src_y;

    /* Resolve every row before the first write. Unsupported address layouts
     * must fall through without leaving QuickDraw a partially copied image. */
    for (int32 row = 0; row < height; ++row) {
        NativeSpan src;
        NativeSpan dst;
        uint32 src_bit = 0;
        uint32 dst_bit = 0;
        uint32 src_bytes = 0;
        uint32 dst_bytes = 0;
        if (!translatePixelRange(src_bitmap, src_rect.left + x0, src_y + row,
                                 width, src, src_bit, src_bytes) ||
            !translatePixelRange(dst_bitmap, dst_rect.left + x0, dst_y + row,
                                 width, dst, dst_bit, dst_bytes)) {
            return false;
        }
    }

    bytes_copied = 0;
    const bool same = sameBitmap(src_bitmap, dst_bitmap);
    for (int32 row_index = 0; row_index < height; ++row_index) {
        const int32 row = bottom_up ? height - 1 - row_index : row_index;
        const bool right_to_left =
            same && src_y + row == dst_y + row &&
            src_rect.left + x0 < dst_rect.left + x0;
        if (!copyPixelRange(src_bitmap, dst_bitmap,
                            src_rect.left + x0, src_y + row,
                            dst_rect.left + x0, dst_y + row,
                            width, right_to_left, bytes_copied)) {
            return false;
        }
    }
    return true;
}

static bool findPortColor(uint32 port, const NativeBitmap &bitmap,
                          bool foreground, uint8 &pixel)
{
    /* Color QuickDraw caches the Color Manager's resolved pixel values in
     * fgColor/bkColor. Prefer those over an exact RGB table search: indexed
     * devices may legitimately approximate the requested RGB value. */
    const uint32 resolved = get_long(port + (foreground ? 80 : 84));
    if (resolved <= 0xff) {
        pixel = (uint8)resolved;
        return true;
    }
    if (!guestRamRange(bitmap.record + 42, 4)) return false;
    const uint32 table_handle = get_long(bitmap.record + 42);
    if (!guestRamRange(table_handle, 4)) return false;
    const uint32 table = get_long(table_handle);
    if (!guestRamRange(table, 8)) return false;
    const int32 last = (int16)get_word(table + 6);
    if (last < 0 || last > 255 || !guestRamRange(table + 8, (uint32)(last + 1) * 8))
        return false;

    const uint32 rgb = port + (foreground ? 36 : 42);
    if (!guestRamRange(rgb, 6)) return false;
    const uint16 red = (uint16)get_word(rgb + 0);
    const uint16 green = (uint16)get_word(rgb + 2);
    const uint16 blue = (uint16)get_word(rgb + 4);
    uint64 best_distance = UINT64_MAX;
    uint8 best_pixel = 0;
    for (int32 index = 0; index <= last; ++index) {
        const uint32 entry = table + 8 + (uint32)index * 8;
        const int64 dr = (int32)(uint16)get_word(entry + 2) - red;
        const int64 dg = (int32)(uint16)get_word(entry + 4) - green;
        const int64 db = (int32)(uint16)get_word(entry + 6) - blue;
        const uint64 distance = (uint64)(dr * dr + dg * dg + db * db);
        if (distance < best_distance) {
            best_distance = distance;
            best_pixel = (uint8)get_word(entry + 0);
        }
        if (distance == 0) {
            pixel = (uint8)get_word(entry + 0);
            return true;
        }
    }
    pixel = best_pixel;
    return true;
}

static bool readNativePattern(uint32 handle, uint32 port,
                              const NativeBitmap &bitmap,
                              NativePattern &native)
{
    if (!guestRamRange(handle, 4)) return false;
    const uint32 pattern = get_long(handle);
    if (!guestRamRange(pattern, 28) || get_word(pattern) != 0) return false;

    for (uint32 index = 0; index < 8; ++index)
        native.rows[index] = (uint8)get_byte(pattern + 20 + index);
    if (bitmap.depth == 1) {
        native.foreground = 1;
        native.background = 0;
        return true;
    }
    return findPortColor(port, bitmap, true, native.foreground) &&
           findPortColor(port, bitmap, false, native.background);
}

static bool readPortPattern(uint32 port, uint32 offset,
                            const NativeBitmap &bitmap,
                            NativePattern &native)
{
    const bool color_port =
        ((uint16)get_word(port + 6) & kColorPortVersionMask) ==
        kColorPortVersionMask;
    if (bitmap.depth == 1 && !color_port) {
        if (!guestRamRange(port + offset, 8)) return false;
        for (uint32 index = 0; index < 8; ++index)
            native.rows[index] = (uint8)get_byte(port + offset + index);
        native.foreground = 1;
        native.background = 0;
        return true;
    }
    if (!guestRamRange(port + offset, 4)) return false;
    return readNativePattern(get_long(port + offset), port, bitmap, native);
}

static bool backgroundPattern(uint32 port, const NativeBitmap &bitmap,
                              NativePattern &native)
{
    return readPortPattern(port, 32, bitmap, native);
}

static void fillPattern(uint8 *destination, uint32 width, int32 x, int32 y,
                        const NativePattern &pattern)
{
    const uint8 bits = pattern.rows[(uint32)y & 7];
    if (bits == 0) {
        memset(destination, pattern.background, width);
        return;
    }
    if (bits == 0xff) {
        memset(destination, pattern.foreground, width);
        return;
    }
    for (uint32 index = 0; index < width; ++index) {
        const uint32 bit = 7 - ((uint32)(x + (int32)index) & 7);
        destination[index] = (bits & (1U << bit))
                                 ? pattern.foreground : pattern.background;
    }
}

static bool fillPatternRange(const NativeBitmap &bitmap, int32 x, int32 y,
                             uint32 width, const NativePattern &pattern,
                             uint64 &bytes_filled)
{
    NativeSpan destination;
    uint32 first_bit = 0;
    uint32 destination_bytes = 0;
    if (!translatePixelRange(bitmap, x, y, width, destination,
                             first_bit, destination_bytes)) {
        return false;
    }
    if (bitmap.depth == 8) {
        fillPattern(destination.host, width, x, y, pattern);
        bytes_filled += width;
    } else {
        const uint8 pattern_bits = pattern.rows[(uint32)y & 7];
        uint8 aligned_pattern = 0;
        for (uint32 bit = 0; bit < 8; ++bit) {
            if (pattern_bits &
                (0x80U >> ((uint32)(bitmap.bounds.left + (int32)bit) & 7))) {
                aligned_pattern |= (uint8)(0x80U >> bit);
            }
        }
        auto fill_one = [&](uint32 index) {
            const bool value =
                (pattern_bits & (0x80U >> ((uint32)(x + (int32)index) & 7))) != 0;
            const uint32 destination_index = first_bit + index;
            const uint8 mask = (uint8)(0x80U >> (destination_index & 7));
            if (value)
                destination.host[destination_index >> 3] |= mask;
            else
                destination.host[destination_index >> 3] &= (uint8)~mask;
        };
        uint32 index = 0;
        while (index < width && ((first_bit + index) & 7) != 0)
            fill_one(index++);
        const uint32 full_bytes = (width - index) >> 3;
        if (full_bytes != 0) {
            memset(destination.host + ((first_bit + index) >> 3),
                   aligned_pattern, full_bytes);
            index += full_bytes << 3;
        }
        while (index < width) fill_one(index++);
        bytes_filled += destination_bytes;
    }
    if (destination.frame)
        VideoMarkDirtyRange(destination.frame_offset, destination_bytes);
    return true;
}

static bool fillRectBytes(const NativeBitmap &bitmap, const QDRect &rect, uint8 value,
                          uint64 &bytes_filled)
{
    if (!rectValid(rect)) {
        bytes_filled = 0;
        return true;
    }
    const uint32 width = (uint32)(rect.right - rect.left);
    for (int32 y = rect.top; y < rect.bottom; ++y) {
        NativeSpan dst;
        if (!translateSpan(bitmap, rect.left, y, width, dst)) return false;
    }
    for (int32 y = rect.top; y < rect.bottom; ++y) {
        NativeSpan dst;
        if (!translateSpan(bitmap, rect.left, y, width, dst)) return false;
        memset(dst.host, value, width);
        if (dst.frame) VideoMarkDirtyRange(dst.frame_offset, width);
    }
    bytes_filled = (uint64)width * (uint32)(rect.bottom - rect.top);
    return true;
}

static bool shiftIntervals(const XIntervals &input, int32 delta,
                           XIntervals &output)
{
    output.count = input.count;
    for (uint32 index = 0; index < input.count; ++index) {
        const int64 shifted = (int64)input.endpoints[index] + delta;
        if (shifted < INT32_MIN || shifted > INT32_MAX) return false;
        output.endpoints[index] = (int32)shifted;
    }
    return true;
}

static bool subtractIntervals(const XIntervals &whole, const XIntervals &removed,
                              XIntervals &output)
{
    output.count = 0;
    uint32 remove_index = 0;
    for (uint32 index = 0; index + 1 < whole.count; index += 2) {
        int32 cursor = whole.endpoints[index];
        const int32 end = whole.endpoints[index + 1];
        while (remove_index + 1 < removed.count &&
               removed.endpoints[remove_index + 1] <= cursor) {
            remove_index += 2;
        }
        uint32 scan = remove_index;
        while (scan + 1 < removed.count && removed.endpoints[scan] < end) {
            if (removed.endpoints[scan] > cursor) {
                if (output.count + 2 > kMaxRegionEndpoints) return false;
                output.endpoints[output.count++] = cursor;
                output.endpoints[output.count++] = removed.endpoints[scan] < end
                                                        ? removed.endpoints[scan] : end;
            }
            if (removed.endpoints[scan + 1] > cursor)
                cursor = removed.endpoints[scan + 1];
            if (cursor >= end) break;
            scan += 2;
        }
        if (cursor < end) {
            if (output.count + 2 > kMaxRegionEndpoints) return false;
            output.endpoints[output.count++] = cursor;
            output.endpoints[output.count++] = end;
        }
    }
    return true;
}

static bool scrollIntervalsAtY(const NativePort &port, const QDRect &area,
                               int32 y, int32 dh, int32 dv,
                               XIntervals &copied, XIntervals &exposed)
{
    XIntervals destination;
    XIntervals source;
    XIntervals shifted_source;
    if (!portIntervalsAtY(port, area, y, destination) ||
        !portIntervalsAtY(port, area, y - dv, source) ||
        !shiftIntervals(source, dh, shifted_source) ||
        !intersectIntervals(destination, shifted_source, copied) ||
        !subtractIntervals(destination, copied, exposed)) {
        return false;
    }
    return true;
}

static bool validateIntervalSpans(const NativeBitmap &bitmap,
                                  const XIntervals &intervals, int32 y,
                                  int32 source_dx, int32 source_dy,
                                  bool include_source)
{
    for (uint32 index = 0; index + 1 < intervals.count; index += 2) {
        const int32 left = intervals.endpoints[index];
        const uint32 width = (uint32)(intervals.endpoints[index + 1] - left);
        NativeSpan destination;
        uint32 destination_bit = 0;
        uint32 destination_bytes = 0;
        if (!translatePixelRange(bitmap, left, y, width, destination,
                                 destination_bit, destination_bytes)) {
            return false;
        }
        if (include_source) {
            NativeSpan source;
            uint32 source_bit = 0;
            uint32 source_bytes = 0;
            if (!translatePixelRange(bitmap, left + source_dx, y + source_dy,
                                     width, source, source_bit, source_bytes)) {
                return false;
            }
        }
    }
    return true;
}

static inline void finishTrap(uint32 original_sp, uint32 argument_bytes);

static bool copyPortRectBytes(const NativeBitmap &source_bitmap,
                              const NativeBitmap &destination_bitmap,
                              const QDRect &source_rect,
                              const QDRect &destination_rect,
                              const NativePort &port,
                              uint64 &bytes_copied)
{
    const int32 width = destination_rect.right - destination_rect.left;
    const int32 height = destination_rect.bottom - destination_rect.top;
    if (width <= 0 || height <= 0 ||
        source_bitmap.depth != destination_bitmap.depth ||
        width != source_rect.right - source_rect.left ||
        height != source_rect.bottom - source_rect.top) {
        return false;
    }

    const int32 source_dx = source_rect.left - destination_rect.left;
    const int32 source_dy = source_rect.top - destination_rect.top;
    QDRect area = destination_rect;
    if (area.left < destination_bitmap.bounds.left)
        area.left = destination_bitmap.bounds.left;
    if (area.right > destination_bitmap.bounds.right)
        area.right = destination_bitmap.bounds.right;
    if (area.top < destination_bitmap.bounds.top)
        area.top = destination_bitmap.bounds.top;
    if (area.bottom > destination_bitmap.bounds.bottom)
        area.bottom = destination_bitmap.bounds.bottom;
    if (area.left < source_bitmap.bounds.left - source_dx)
        area.left = source_bitmap.bounds.left - source_dx;
    if (area.right > source_bitmap.bounds.right - source_dx)
        area.right = source_bitmap.bounds.right - source_dx;
    if (area.top < source_bitmap.bounds.top - source_dy)
        area.top = source_bitmap.bounds.top - source_dy;
    if (area.bottom > source_bitmap.bounds.bottom - source_dy)
        area.bottom = source_bitmap.bounds.bottom - source_dy;
    if (!rectValid(area)) {
        bytes_copied = 0;
        return true;
    }

    for (int32 y = area.top; y < area.bottom; ++y) {
        XIntervals intervals;
        if (!portIntervalsAtY(port, area, y, intervals) ||
            !validateIntervalSpans(destination_bitmap, intervals, y,
                                   source_dx, source_dy, false)) {
            return false;
        }
        for (uint32 index = 0; index + 1 < intervals.count; index += 2) {
            NativeSpan source;
            const int32 left = intervals.endpoints[index];
            const uint32 span_width =
                (uint32)(intervals.endpoints[index + 1] - left);
            uint32 source_bit = 0;
            uint32 source_bytes = 0;
            if (!translatePixelRange(source_bitmap, left + source_dx,
                                     y + source_dy, span_width, source,
                                     source_bit, source_bytes)) {
                return false;
            }
        }
    }

    bytes_copied = 0;
    const bool same = sameBitmap(source_bitmap, destination_bitmap);
    const bool bottom_up = same && source_dy < 0;
    const bool right_to_left = same && source_dy == 0 && source_dx < 0;
    for (int32 row_index = 0; row_index < area.bottom - area.top; ++row_index) {
        const int32 y = bottom_up ? area.bottom - 1 - row_index
                                  : area.top + row_index;
        XIntervals intervals;
        if (!portIntervalsAtY(port, area, y, intervals)) return false;
        for (uint32 span_index = 0; span_index + 1 < intervals.count;
             span_index += 2) {
            const uint32 index = right_to_left
                                     ? intervals.count - 2 - span_index
                                     : span_index;
            const int32 left = intervals.endpoints[index];
            const uint32 span_width =
                (uint32)(intervals.endpoints[index + 1] - left);
            if (!copyPixelRange(source_bitmap, destination_bitmap,
                                left + source_dx, y + source_dy,
                                left, y, span_width, right_to_left,
                                bytes_copied)) {
                return false;
            }
        }
    }
    return true;
}

static bool rectangleIntervals(const QDRect &rect, int32 y, bool frame,
                               int32 pen_h, int32 pen_v,
                               XIntervals &intervals)
{
    intervals.count = 0;
    if (y < rect.top || y >= rect.bottom) return true;
    if (!frame || y < rect.top + pen_v || y >= rect.bottom - pen_v ||
        pen_h * 2 >= rect.right - rect.left) {
        intervals.endpoints[0] = rect.left;
        intervals.endpoints[1] = rect.right;
        intervals.count = 2;
        return true;
    }
    intervals.endpoints[0] = rect.left;
    intervals.endpoints[1] = rect.left + pen_h;
    intervals.endpoints[2] = rect.right - pen_h;
    intervals.endpoints[3] = rect.right;
    intervals.count = 4;
    return true;
}

static bool filledOvalIntervals(const QDRect &rect, int32 y,
                                XIntervals &intervals)
{
    intervals.count = 0;
    if (!rectValid(rect) || y < rect.top || y >= rect.bottom) return true;
    const int64 width = rect.right - rect.left;
    const int64 height = rect.bottom - rect.top;
    const int64 dy2 = 2LL * y + 1 - (rect.top + rect.bottom);
    const uint64 width2 = (uint64)(width * width);
    const uint64 height2 = (uint64)(height * height);
    const uint64 rhs = width2 * height2;

    auto inside = [&](int32 x) {
        const int64 dx2 = 2LL * x + 1 - (rect.left + rect.right);
        return (uint64)(dx2 * dx2) * height2 +
                   (uint64)(dy2 * dy2) * width2 <= rhs;
    };
    const int32 center = rect.left + (int32)(width / 2);
    if (!inside(center)) return true;

    int32 low = rect.left;
    int32 high = center;
    while (low < high) {
        const int32 middle = low + (high - low) / 2;
        if (inside(middle)) high = middle;
        else low = middle + 1;
    }
    const int32 inset = low - rect.left;
    intervals.endpoints[0] = low;
    intervals.endpoints[1] = rect.right - inset;
    intervals.count = intervals.endpoints[0] < intervals.endpoints[1] ? 2 : 0;
    return true;
}

static bool ovalIntervals(const QDRect &rect, int32 y, bool frame,
                          int32 pen_h, int32 pen_v,
                          XIntervals &intervals)
{
    XIntervals outer;
    if (!filledOvalIntervals(rect, y, outer)) return false;
    if (!frame || outer.count == 0) {
        intervals = outer;
        return true;
    }
    QDRect inner = {rect.top + pen_v, rect.left + pen_h,
                    rect.bottom - pen_v, rect.right - pen_h};
    XIntervals inside;
    if (!filledOvalIntervals(inner, y, inside) ||
        !subtractIntervals(outer, inside, intervals)) {
        return false;
    }
    return true;
}

static bool paintNativeShape(const NativePort &port, const QDRect &rect,
                             bool oval, bool frame,
                             const NativePattern &pattern,
                             int32 pen_h, int32 pen_v,
                             uint64 &bytes_painted)
{
    if (!rectValid(rect)) {
        bytes_painted = 0;
        return true;
    }
    QDRect area = rect;
    if (!intersectRect(area, port.bounds) ||
        !intersectRect(area, port.visible.bounds) ||
        !intersectRect(area, port.clipping.bounds)) {
        bytes_painted = 0;
        return true;
    }

    for (int32 y = area.top; y < area.bottom; ++y) {
        XIntervals shape;
        XIntervals visible;
        XIntervals clipped;
        const bool shape_ok = oval
                                  ? ovalIntervals(rect, y, frame, pen_h, pen_v,
                                                  shape)
                                  : rectangleIntervals(rect, y, frame, pen_h,
                                                       pen_v, shape);
        if (!shape_ok || !portIntervalsAtY(port, area, y, visible) ||
            !intersectIntervals(shape, visible, clipped) ||
            !validateIntervalSpans(port.bitmap, clipped, y, 0, 0, false)) {
            return false;
        }
    }

    bytes_painted = 0;
    for (int32 y = area.top; y < area.bottom; ++y) {
        XIntervals shape;
        XIntervals visible;
        XIntervals clipped;
        if (oval) ovalIntervals(rect, y, frame, pen_h, pen_v, shape);
        else rectangleIntervals(rect, y, frame, pen_h, pen_v, shape);
        portIntervalsAtY(port, area, y, visible);
        intersectIntervals(shape, visible, clipped);
        for (uint32 index = 0; index + 1 < clipped.count; index += 2) {
            const int32 left = clipped.endpoints[index];
            const uint32 width =
                (uint32)(clipped.endpoints[index + 1] - left);
            if (!fillPatternRange(port.bitmap, left, y, width, pattern,
                                  bytes_painted)) {
                return false;
            }
        }
    }
    return true;
}

static bool paintLinePoint(const NativePort &port, int32 h, int32 v,
                           int32 pen_h, int32 pen_v,
                           const NativePattern &pattern, bool write,
                           uint64 &bytes_painted)
{
    QDRect area = {v, h, v + pen_v, h + pen_h};
    if (!intersectRect(area, port.bounds) ||
        !intersectRect(area, port.visible.bounds) ||
        !intersectRect(area, port.clipping.bounds)) {
        return true;
    }
    for (int32 y = area.top; y < area.bottom; ++y) {
        XIntervals visible;
        if (!portIntervalsAtY(port, area, y, visible)) return false;
        if (!write) {
            if (!validateIntervalSpans(port.bitmap, visible, y, 0, 0, false))
                return false;
            continue;
        }
        for (uint32 index = 0; index + 1 < visible.count; index += 2) {
            const int32 left = visible.endpoints[index];
            const uint32 width =
                (uint32)(visible.endpoints[index + 1] - left);
            if (!fillPatternRange(port.bitmap, left, y, width, pattern,
                                  bytes_painted)) {
                return false;
            }
        }
    }
    return true;
}

template <typename Visitor>
static bool visitLinePoints(int32 old_h, int32 old_v,
                            int32 new_h, int32 new_v, Visitor visitor)
{
    int32 h = old_h;
    int32 v = old_v;
    const int32 dh = new_h >= old_h ? new_h - old_h : old_h - new_h;
    const int32 dv = new_v >= old_v ? new_v - old_v : old_v - new_v;
    const int32 step_h = old_h < new_h ? 1 : -1;
    const int32 step_v = old_v < new_v ? 1 : -1;
    int32 error = dh - dv;
    while (h != new_h || v != new_v) {
        if (!visitor(h, v)) return false;
        const int32 doubled = error * 2;
        if (doubled > -dv) {
            error -= dv;
            h += step_h;
        }
        if (doubled < dh) {
            error += dh;
            v += step_v;
        }
    }
    return true;
}

static bool tryMoveTo(uint32 sp)
{
    ++s_stats.move_attempts;
    if (!guestRamRange(sp, 4)) {
        ++s_stats.move_failures;
        return false;
    }
    const uint32 a5 = m68k_areg(regs, 5);
    if (!guestRamRange(a5, 4)) {
        ++s_stats.move_failures;
        return false;
    }
    const uint32 qd_globals = get_long(a5);
    if (!guestRamRange(qd_globals, 4)) {
        ++s_stats.move_failures;
        return false;
    }
    const uint32 port = get_long(qd_globals);
    if (!guestRamRange(port, 108) ||
        ((uint16)get_word(port + 6) & kColorPortVersionMask) !=
            kColorPortVersionMask) {
        ++s_stats.move_failures;
        return false;
    }
    NativeBitmap bitmap;
    if (!readBitmap(port + 2, bitmap) || bitmap.depth != 1) {
        ++s_stats.move_failures;
        return false;
    }
    put_word(port + 48, get_word(sp));
    put_word(port + 50, get_word(sp + 2));
    finishTrap(sp, 4);
    ++s_stats.move_calls;
    return true;
}

static bool tryLineTo(uint32 sp)
{
    ++s_stats.line_attempts;
    if (!guestRamRange(sp, 4)) {
        ++s_stats.line_failures;
        return false;
    }
    NativePort port;
    if (!readCurrentPort(port) || port.bitmap.depth != 1 ||
        get_word(port.record + 56) != kPatternCopyMode) {
        ++s_stats.line_failures;
        return false;
    }
    const int32 pen_v = (int16)get_word(port.record + 52);
    const int32 pen_h = (int16)get_word(port.record + 54);
    if (pen_h <= 0 || pen_v <= 0 || (int16)get_word(port.record + 66) < 0) {
        ++s_stats.line_failures;
        return false;
    }
    NativePattern pattern;
    if (!readPortPattern(port.record, 58, port.bitmap, pattern)) {
        ++s_stats.line_failures;
        return false;
    }
    const int32 old_v = (int16)get_word(port.record + 48);
    const int32 old_h = (int16)get_word(port.record + 50);
    const int32 new_v = (int16)get_word(sp);
    const int32 new_h = (int16)get_word(sp + 2);
    uint64 painted = 0;
    bool line_ok = true;
    if (old_v == new_v && old_h != new_h) {
        QDRect line = {
            old_v,
            old_h < new_h ? old_h : new_h + 1,
            old_v + pen_v,
            old_h < new_h ? new_h + pen_h - 1 : old_h + pen_h,
        };
        line_ok = paintNativeShape(port, line, false, false, pattern,
                                   pen_h, pen_v, painted);
    } else if (old_h == new_h && old_v != new_v) {
        QDRect line = {
            old_v < new_v ? old_v : new_v + 1,
            old_h,
            old_v < new_v ? new_v + pen_v - 1 : old_v + pen_v,
            old_h + pen_h,
        };
        line_ok = paintNativeShape(port, line, false, false, pattern,
                                   pen_h, pen_v, painted);
    } else if (old_h != new_h || old_v != new_v) {
        line_ok = visitLinePoints(old_h, old_v, new_h, new_v,
                                  [&](int32 h, int32 v) {
                                      return paintLinePoint(
                                          port, h, v, pen_h, pen_v,
                                          pattern, false, painted);
                                  });
        if (line_ok) {
            line_ok = visitLinePoints(old_h, old_v, new_h, new_v,
                         [&](int32 h, int32 v) {
                             return paintLinePoint(port, h, v, pen_h, pen_v,
                                                   pattern, true, painted);
                         });
        }
    }
    if (!line_ok) {
        ++s_stats.line_failures;
        return false;
    }
    put_word(port.record + 48, (uint16)new_v);
    put_word(port.record + 50, (uint16)new_h);
    finishTrap(sp, 4);
    ++s_stats.line_calls;
    s_stats.line_bytes += painted;
    return true;
}

static bool tryShape(uint16 opcode, uint32 sp)
{
    ++s_stats.shape_attempts;
    if (!guestRamRange(sp, 4)) {
        ++s_stats.shape_failures;
        return false;
    }
    QDRect rect;
    if (!readRect(get_long(sp), rect)) {
        ++s_stats.shape_failures;
        return false;
    }
    NativePort port;
    if (!readCurrentPort(port) || get_word(port.record + 56) != kPatternCopyMode) {
        ++s_stats.shape_failures;
        return false;
    }
    const int32 pen_v = (int16)get_word(port.record + 52);
    const int32 pen_h = (int16)get_word(port.record + 54);
    if (pen_h <= 0 || pen_v <= 0 || (int16)get_word(port.record + 66) < 0) {
        ++s_stats.shape_failures;
        return false;
    }
    NativePattern pattern;
    if (!readPortPattern(port.record, 58, port.bitmap, pattern)) {
        ++s_stats.shape_failures;
        return false;
    }
    const bool oval = opcode == kFrameOvalTrap || opcode == kPaintOvalTrap;
    const bool frame = opcode == kFrameRectTrap || opcode == kFrameOvalTrap;
    uint64 painted = 0;
    if (!paintNativeShape(port, rect, oval, frame, pattern,
                          pen_h, pen_v, painted)) {
        ++s_stats.shape_failures;
        return false;
    }
    finishTrap(sp, 4);
    ++s_stats.shape_calls;
    s_stats.shape_bytes += painted;
    return true;
}

static inline void finishTrap(uint32 original_sp, uint32 argument_bytes)
{
    m68k_areg(regs, 7) = original_sp + argument_bytes;
    m68k_incpc(2);
}

static bool tryCopyBits(uint32 sp)
{
    ++s_stats.copy_attempts;
    /* maskRgn.l, mode.w, dstRect.l, srcRect.l, dstBits.l, srcBits.l */
    if (!guestRamRange(sp, 22) || get_long(sp) != 0 || get_word(sp + 4) != 0) {
        ++s_stats.copy_arg_failures;
        return false;
    }

    const uint32 dst_rect_address = get_long(sp + 6);
    const uint32 src_rect_address = get_long(sp + 10);
    const uint32 dst_bits = get_long(sp + 14);
    const uint32 src_bits = get_long(sp + 18);

    QDRect src_rect;
    QDRect dst_rect;
    NativeBitmap src_bitmap;
    NativeBitmap dst_bitmap;
    if (!readRect(src_rect_address, src_rect) || !readRect(dst_rect_address, dst_rect) ||
        !readBitmap(src_bits, src_bitmap) || !readBitmap(dst_bits, dst_bitmap)) {
        ++s_stats.copy_bitmap_failures;
        return false;
    }

    NativePort port;
    if (!readCurrentPort(port)) {
        ++s_stats.copy_port_failures;
        return false;
    }

    uint64 copied = 0;
    const bool targets_port = sameBitmap(dst_bitmap, port.bitmap);
    const bool accelerated = targets_port
                                 ? copyPortRectBytes(src_bitmap, dst_bitmap,
                                                     src_rect, dst_rect,
                                                     port, copied)
                                 : copyRectBytes(src_bitmap, dst_bitmap,
                                                 src_rect, dst_rect, copied);
    if (!accelerated) {
        ++s_stats.copy_execution_failures;
        return false;
    }
    finishTrap(sp, 22);
    ++s_stats.copy_calls;
    s_stats.copy_bytes += copied;
    return true;
}

static bool tryScrollRect(uint32 sp)
{
    ++s_stats.scroll_attempts;
    /* updateRgn.l, dv.w, dh.w, rect.l */
    if (!guestRamRange(sp, 12)) {
        ++s_stats.scroll_arg_failures;
        return false;
    }
    const uint32 update_region = get_long(sp + 0);
    const int32 dv = (int16)get_word(sp + 4);
    const int32 dh = (int16)get_word(sp + 6);
    const uint32 rect_address = get_long(sp + 8);
    if (update_region == 0 || (dh != 0 && dv != 0)) {
        ++s_stats.scroll_arg_failures;
        return false;
    }

    QDRect area;
    if (!readRect(rect_address, area) || !rectValid(area)) {
        ++s_stats.scroll_arg_failures;
        return false;
    }
    NativePort port;
    if (!readCurrentPort(port)) {
        ++s_stats.scroll_port_failures;
        return false;
    }

    /* Validate the caller-owned region before touching pixels. A failed fast
     * path must leave the original QuickDraw handler a pristine operation. */
    if (!guestRamRange(update_region, 4) ||
        !guestRamRange(get_long(update_region), 10)) {
        ++s_stats.scroll_region_failures;
        return false;
    }

    if (!intersectRect(area, port.bounds) ||
        !intersectRect(area, port.visible.bounds) ||
        !intersectRect(area, port.clipping.bounds)) {
        const QDRect empty = {0, 0, 0, 0};
        if (!writeSimpleRegion(update_region, empty)) return false;
        finishTrap(sp, 12);
        ++s_stats.scroll_calls;
        return true;
    }

    NativePattern background;
    if (!backgroundPattern(port.record, port.bitmap, background)) {
        ++s_stats.scroll_background_failures;
        return false;
    }

    /* Validate every clipped row before changing the framebuffer. Complex
     * visible regions are represented as XOR inversion boundaries; requiring
     * both source and destination membership preserves covered windows. */
    for (int32 y = area.top; y < area.bottom; ++y) {
        XIntervals copied;
        XIntervals exposed;
        if (!scrollIntervalsAtY(port, area, y, dh, dv, copied, exposed) ||
            !validateIntervalSpans(port.bitmap, copied, y, -dh, -dv, true) ||
            !validateIntervalSpans(port.bitmap, exposed, y, 0, 0, false)) {
            ++s_stats.scroll_execution_failures;
            return false;
        }
    }

    uint64 moved = 0;
    for (int32 row_index = 0; row_index < area.bottom - area.top; ++row_index) {
        const int32 y = dv > 0 ? area.bottom - 1 - row_index
                               : area.top + row_index;
        XIntervals copied;
        XIntervals exposed_unused;
        if (!scrollIntervalsAtY(port, area, y, dh, dv,
                                copied, exposed_unused)) {
            ++s_stats.scroll_execution_failures;
            return false;
        }
        for (uint32 span_index = 0; span_index + 1 < copied.count;
             span_index += 2) {
            const uint32 index = dh > 0 ? copied.count - 2 - span_index
                                        : span_index;
            const int32 left = copied.endpoints[index];
            const uint32 width = (uint32)(copied.endpoints[index + 1] - left);
            if (!copyPixelRange(port.bitmap, port.bitmap,
                                left - dh, y - dv, left, y, width,
                                dh > 0, moved)) {
                ++s_stats.scroll_execution_failures;
                return false;
            }
        }
    }

    uint64 filled = 0;
    bool has_exposed = false;
    QDRect exposed_bounds = {0, 0, 0, 0};
    for (int32 y = area.top; y < area.bottom; ++y) {
        XIntervals copied_unused;
        XIntervals exposed;
        if (!scrollIntervalsAtY(port, area, y, dh, dv,
                                copied_unused, exposed)) {
            ++s_stats.scroll_execution_failures;
            return false;
        }
        for (uint32 index = 0; index + 1 < exposed.count; index += 2) {
            const int32 left = exposed.endpoints[index];
            const int32 right = exposed.endpoints[index + 1];
            const uint32 width = (uint32)(right - left);
            if (!fillPatternRange(port.bitmap, left, y, width, background,
                                  filled)) {
                ++s_stats.scroll_execution_failures;
                return false;
            }
            if (!has_exposed) {
                exposed_bounds = {y, left, y + 1, right};
                has_exposed = true;
            } else {
                if (y < exposed_bounds.top) exposed_bounds.top = y;
                if (left < exposed_bounds.left) exposed_bounds.left = left;
                if (y + 1 > exposed_bounds.bottom) exposed_bounds.bottom = y + 1;
                if (right > exposed_bounds.right) exposed_bounds.right = right;
            }
        }
    }
    if (!has_exposed) exposed_bounds = {0, 0, 0, 0};
    if (!writeSimpleRegion(update_region, exposed_bounds)) return false;

    markFrameRows(port.bitmap, area);
    finishTrap(sp, 12);
    ++s_stats.scroll_calls;
    s_stats.scroll_bytes += moved + filled;
    return true;
}

} // namespace

bool QuickDrawAccelTryTrap(uint16 opcode)
{
    const uint32 sp = m68k_areg(regs, 7);
    if (opcode == kMoveToTrap) return tryMoveTo(sp);
    if (opcode == kLineToTrap) return tryLineTo(sp);
    if (opcode == kCopyBitsTrap) return tryCopyBits(sp);
    if (opcode == kScrollRectTrap) return tryScrollRect(sp);
    if (opcode == kFrameRectTrap || opcode == kPaintRectTrap ||
        opcode == kFrameOvalTrap || opcode == kPaintOvalTrap) {
        return tryShape(opcode, sp);
    }
    return false;
}

void QuickDrawAccelResetStats(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_region_snapshot, 0, sizeof(s_region_snapshot));
    s_region_snapshot_words = 0;
}

void QuickDrawAccelReadStats(QuickDrawAccelStats *stats)
{
    if (stats) *stats = s_stats;
}

uint32 QuickDrawAccelReadRegionSnapshot(uint16 *words, uint32 capacity)
{
    const uint32 count = s_region_snapshot_words < capacity
                             ? s_region_snapshot_words : capacity;
    if (words) {
        for (uint32 index = 0; index < count; ++index)
            words[index] = s_region_snapshot[index];
    }
    return count;
}
