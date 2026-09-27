/*
 * hid_descriptor.cpp - Minimal HID report-descriptor parser.
 *
 * See hid_descriptor.h for the calling contract. This file only models
 * enough of the HID spec to decode the standard mouse usages (X, Y,
 * Wheel, Buttons) on a Generic Desktop / Button collection. Everything
 * else is skipped. The output layout is consumed by input_esp32.cpp's
 * processMouseReport with the existing heuristic decoder still in
 * place as a fallback.
 */

#include "hid_descriptor.h"

#include <string.h>

#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01
#define HID_USAGE_PAGE_BUTTON          0x09

#define HID_USAGE_GD_X      0x30
#define HID_USAGE_GD_Y      0x31
#define HID_USAGE_GD_WHEEL  0x38

/* Short-item header decoding (bSize, bType, bTag) per HID 1.11 §6.2.2. */
#define HID_ITEM_SIZE(b)   ((b) & 0x03)
#define HID_ITEM_TYPE(b)   (((b) >> 2) & 0x03)
#define HID_ITEM_TAG(b)    (((b) >> 4) & 0x0F)

#define HID_TYPE_MAIN      0
#define HID_TYPE_GLOBAL    1
#define HID_TYPE_LOCAL     2

/* Main-item tags */
#define HID_MAIN_INPUT          0x08
#define HID_MAIN_OUTPUT         0x09
#define HID_MAIN_FEATURE        0x0B
#define HID_MAIN_COLLECTION     0x0A
#define HID_MAIN_END_COLLECTION 0x0C

/* Global-item tags */
#define HID_GLOBAL_USAGE_PAGE   0x00
#define HID_GLOBAL_LOG_MIN      0x01
#define HID_GLOBAL_LOG_MAX      0x02
#define HID_GLOBAL_PHY_MIN      0x03
#define HID_GLOBAL_PHY_MAX      0x04
#define HID_GLOBAL_UNIT_EXP     0x05
#define HID_GLOBAL_UNIT         0x06
#define HID_GLOBAL_REPORT_SIZE  0x07
#define HID_GLOBAL_REPORT_ID    0x08
#define HID_GLOBAL_REPORT_COUNT 0x09
#define HID_GLOBAL_PUSH         0x0A
#define HID_GLOBAL_POP          0x0B

/* Local-item tags */
#define HID_LOCAL_USAGE         0x00
#define HID_LOCAL_USAGE_MIN     0x01
#define HID_LOCAL_USAGE_MAX     0x02

/* Input main-item flags (from data[0] of an Input item) */
#define HID_INPUT_CONSTANT      0x01
#define HID_INPUT_VARIABLE      0x02

#define USAGE_STACK_DEPTH       16

typedef struct {
    /* Globals */
    uint16_t usage_page;
    int32_t  logical_min;
    int32_t  logical_max;
    uint8_t  report_size;
    uint8_t  report_count;
    uint8_t  report_id;     /* 0 means "no Report ID has been declared yet" */

    /* Locals (cleared after every Main item) */
    uint16_t usages[USAGE_STACK_DEPTH];
    uint8_t  usage_count;
    uint16_t usage_min;
    uint16_t usage_max;
    bool     have_usage_min;
    bool     have_usage_max;
} ParserState;

/* Per-Report-ID bit offset, since each Report ID is a separate report
 * stream and starts counting from 0. We track up to a small number of
 * IDs explicitly; descriptors that use more than this just won't get
 * accurate offsets for the extra IDs (parser returns valid=false). */
#define MAX_REPORT_IDS 8

typedef struct {
    uint8_t  report_id;
    uint16_t bit_offset;
} ReportCursor;

static int32_t sign_extend(int32_t value, int bytes)
{
    if (bytes == 1) {
        return (int32_t)(int8_t)value;
    }
    if (bytes == 2) {
        return (int32_t)(int16_t)value;
    }
    return value;
}

static int32_t read_item_data(const uint8_t *p, int size_code)
{
    /* size_code is the bSize field: 0=>0 bytes, 1=>1, 2=>2, 3=>4. */
    switch (size_code) {
    case 0: return 0;
    case 1: return sign_extend(p[0], 1);
    case 2: return sign_extend((int32_t)p[0] | ((int32_t)p[1] << 8), 2);
    case 3: return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
    default: return 0;
    }
}

static uint32_t read_item_data_unsigned(const uint8_t *p, int size_code)
{
    switch (size_code) {
    case 0: return 0;
    case 1: return p[0];
    case 2: return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    case 3: return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    default: return 0;
    }
}

/* Find or allocate a cursor entry for the current report_id. */
static ReportCursor *get_cursor(ReportCursor *cursors, int *cursor_count,
                                uint8_t report_id)
{
    for (int i = 0; i < *cursor_count; i++) {
        if (cursors[i].report_id == report_id) {
            return &cursors[i];
        }
    }
    if (*cursor_count >= MAX_REPORT_IDS) {
        return NULL;
    }
    cursors[*cursor_count].report_id = report_id;
    cursors[*cursor_count].bit_offset = 0;
    (*cursor_count)++;
    return &cursors[*cursor_count - 1];
}

bool HidParseMouseDescriptor(const uint8_t *desc, size_t len,
                             HidMouseLayout *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (desc == NULL || len == 0) {
        return false;
    }

    ParserState st;
    memset(&st, 0, sizeof(st));
    st.logical_min = 0;
    st.logical_max = 0;

    ReportCursor cursors[MAX_REPORT_IDS];
    int cursor_count = 0;
    /* Seed a cursor for "no report ID" so the first Input item before
     * any Report ID declaration has somewhere to write. */
    cursors[0].report_id = 0;
    cursors[0].bit_offset = 0;
    cursor_count = 1;

    /* Track whether we've seen any Report ID. If we have, the device
     * sends its reports prefixed with the ID byte and our caller must
     * skip it before applying the bit offsets we record. */
    bool any_report_id_seen = false;

    // Each Report ID has an independent layout. Never combine buttons or
    // axes from unrelated streams on a composite/gaming mouse.
    HidMouseLayout layouts[MAX_REPORT_IDS] = {};

    size_t i = 0;
    while (i < len) {
        uint8_t header = desc[i++];
        /* Long items are extremely rare on real mice; bail out when we
         * see one to keep the parser bounded. */
        if (header == 0xFE) {
            return false;
        }

        int size_code = HID_ITEM_SIZE(header);
        int data_bytes = (size_code == 3) ? 4 : size_code;
        if (i + (size_t)data_bytes > len) {
            return false;
        }

        const uint8_t *data = desc + i;
        i += (size_t)data_bytes;

        int type = HID_ITEM_TYPE(header);
        int tag  = HID_ITEM_TAG(header);

        if (type == HID_TYPE_GLOBAL) {
            switch (tag) {
            case HID_GLOBAL_USAGE_PAGE:
                st.usage_page = (uint16_t)read_item_data_unsigned(data, size_code);
                break;
            case HID_GLOBAL_LOG_MIN:
                st.logical_min = read_item_data(data, size_code);
                break;
            case HID_GLOBAL_LOG_MAX:
                st.logical_max = read_item_data(data, size_code);
                break;
            case HID_GLOBAL_REPORT_SIZE:
                st.report_size = (uint8_t)read_item_data_unsigned(data, size_code);
                break;
            case HID_GLOBAL_REPORT_COUNT:
                st.report_count = (uint8_t)read_item_data_unsigned(data, size_code);
                break;
            case HID_GLOBAL_REPORT_ID:
                st.report_id = (uint8_t)read_item_data_unsigned(data, size_code);
                any_report_id_seen = true;
                break;
            default:
                /* Push/Pop and the unit/exponent items are ignored. */
                break;
            }
        } else if (type == HID_TYPE_LOCAL) {
            switch (tag) {
            case HID_LOCAL_USAGE:
                if (st.usage_count < USAGE_STACK_DEPTH) {
                    /* Some descriptors use a 4-byte usage where the high
                     * 16 bits are the usage page; we only care about
                     * the low 16 bits for our recognised usages. */
                    st.usages[st.usage_count++] =
                        (uint16_t)read_item_data_unsigned(data, size_code);
                }
                break;
            case HID_LOCAL_USAGE_MIN:
                st.usage_min = (uint16_t)read_item_data_unsigned(data, size_code);
                st.have_usage_min = true;
                break;
            case HID_LOCAL_USAGE_MAX:
                st.usage_max = (uint16_t)read_item_data_unsigned(data, size_code);
                st.have_usage_max = true;
                break;
            default:
                break;
            }
        } else if (type == HID_TYPE_MAIN) {
            if (tag == HID_MAIN_INPUT) {
                uint32_t flags = read_item_data_unsigned(data, size_code);
                bool is_constant = (flags & HID_INPUT_CONSTANT) != 0;
                bool is_variable = (flags & HID_INPUT_VARIABLE) != 0;

                ReportCursor *cur = get_cursor(cursors, &cursor_count,
                                               st.report_id);
                if (!cur) return false;
                HidMouseLayout *candidate = &layouts[cur - cursors];
                const uint16_t bit_offset = cur->bit_offset;

                uint16_t total_bits =
                    (uint16_t)st.report_size * (uint16_t)st.report_count;

                if (total_bits > UINT16_MAX - bit_offset) return false;

                if (is_constant) {
                    /* Padding: just advance the cursor. */
                } else if (is_variable) {
                    /* Variable: each of report_count items is its own
                     * field of report_size bits. */
                    if (st.usage_page == HID_USAGE_PAGE_BUTTON &&
                        st.report_size == 1) {
                        /* Button bitfield. Record the starting bit
                         * offset and how many bits there are. We only
                         * support the first button group we see. */
                        if (!candidate->has_buttons) {
                            candidate->has_buttons = true;
                            candidate->button_offset_bits = bit_offset;
                            uint16_t count = st.report_count;
                            if (count > 8) {
                                count = 8;
                            }
                            candidate->button_count = (uint8_t)count;
                        }
                    } else if (st.usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
                        /* For Generic Desktop fields, walk usages one at
                         * a time. The usage stack holds the most recent
                         * Usage items in order; if the stack runs out
                         * the last usage is repeated (HID rule). */
                        for (int k = 0; k < st.report_count; k++) {
                            uint16_t usage = 0;
                            if (k < st.usage_count) {
                                usage = st.usages[k];
                            } else if (st.usage_count > 0) {
                                usage = st.usages[st.usage_count - 1];
                            }
                            uint16_t field_offset =
                                bit_offset + (uint16_t)k * st.report_size;
                            bool is_signed = (st.logical_min < 0);

                            if (usage == HID_USAGE_GD_X) {
                                candidate->x_offset_bits = field_offset;
                                candidate->x_size_bits = st.report_size;
                                candidate->x_signed = is_signed;
                            } else if (usage == HID_USAGE_GD_Y) {
                                candidate->y_offset_bits = field_offset;
                                candidate->y_size_bits = st.report_size;
                                candidate->y_signed = is_signed;
                            } else if (usage == HID_USAGE_GD_WHEEL) {
                                candidate->has_wheel = true;
                                candidate->wheel_offset_bits = field_offset;
                                candidate->wheel_size_bits = st.report_size;
                                candidate->wheel_signed = is_signed;
                            }
                        }
                    }
                    /* Other usage pages are skipped. */
                }

                if (cur != NULL) {
                    cur->bit_offset = bit_offset + total_bits;
                }
            }

            /* All Main items reset the local state. */
            st.usage_count = 0;
            st.usage_min = 0;
            st.usage_max = 0;
            st.have_usage_min = false;
            st.have_usage_max = false;
        }
    }

    for (int c = 0; c < cursor_count; ++c) {
        const auto &candidate = layouts[c];
        if (!candidate.x_size_bits || candidate.x_size_bits > 16 ||
            !candidate.y_size_bits || candidate.y_size_bits > 16 ||
            (candidate.has_wheel && (!candidate.wheel_size_bits || candidate.wheel_size_bits > 16)))
            continue;
        if (any_report_id_seen && cursors[c].report_id == 0) continue;
        *out = candidate;
        out->valid = true;
        out->report_id = cursors[c].report_id;
        out->report_bytes = (cursors[c].bit_offset + 7u) / 8u + (any_report_id_seen ? 1 : 0);
        return true;
    }
    return false;
}

static int16_t extract_field(const uint8_t *data, size_t len,
                             uint16_t bit_offset, uint8_t bit_size,
                             bool is_signed)
{
    if (bit_size == 0 || bit_size > 16) {
        return 0;
    }
    /* Bounds check: don't read past end of buffer. */
    uint32_t end_bit = (uint32_t)bit_offset + bit_size;
    if (((end_bit + 7) / 8) > len) {
        return 0;
    }

    uint32_t value = 0;
    /* Read up to 3 consecutive bytes covering the field. */
    uint16_t byte_offset = bit_offset / 8;
    uint8_t  shift = bit_offset & 7;
    uint16_t bits_needed = bit_size + shift;
    int byte_count = (bits_needed + 7) / 8;
    for (int b = 0; b < byte_count && (byte_offset + b) < len; b++) {
        value |= (uint32_t)data[byte_offset + b] << (8 * b);
    }
    value >>= shift;
    uint32_t mask = (bit_size == 32) ? 0xFFFFFFFFu
                                     : ((1u << bit_size) - 1u);
    value &= mask;

    if (is_signed) {
        uint32_t sign_bit = 1u << (bit_size - 1);
        if (value & sign_bit) {
            value |= ~mask;
        }
        return (int16_t)(int32_t)value;
    }
    return (int16_t)value;
}

bool HidDecodeMouseReport(const uint8_t *data, size_t len,
                          const HidMouseLayout *layout,
                          uint8_t *buttons,
                          int16_t *dx, int16_t *dy, int16_t *dw)
{
    if (data == NULL || layout == NULL || !layout->valid) {
        return false;
    }
    if (len == 0 || len < layout->report_bytes) {
        return false;
    }

    /* If the device uses Report IDs, the first byte of each report is
     * the ID. Skip past it (and require it to match our layout's ID)
     * before applying bit offsets - the offsets we recorded already
     * assume a prefix-stripped buffer because every Report-ID-bearing
     * descriptor counts bits from after the ID in our parser. */
    const uint8_t *body = data;
    size_t         body_len = len;
    if (layout->report_id != 0) {
        if (data[0] != layout->report_id) {
            return false;
        }
        body = data + 1;
        body_len = len - 1;
    }

    int16_t lx = extract_field(body, body_len,
                               layout->x_offset_bits,
                               layout->x_size_bits,
                               layout->x_signed);
    int16_t ly = extract_field(body, body_len,
                               layout->y_offset_bits,
                               layout->y_size_bits,
                               layout->y_signed);
    int16_t lw = 0;
    if (layout->has_wheel) {
        lw = extract_field(body, body_len,
                           layout->wheel_offset_bits,
                           layout->wheel_size_bits,
                           layout->wheel_signed);
    }

    uint8_t btn = 0;
    if (layout->has_buttons) {
        /* Buttons are at most 8 bits packed at button_offset_bits. */
        uint16_t bit_offset = layout->button_offset_bits;
        uint8_t  count = layout->button_count;
        if (count > 8) {
            count = 8;
        }
        for (uint8_t b = 0; b < count; b++) {
            uint16_t pos = bit_offset + b;
            uint16_t byte_idx = pos / 8;
            uint8_t  bit_idx  = pos & 7;
            if (byte_idx >= body_len) {
                break;
            }
            if (body[byte_idx] & (1u << bit_idx)) {
                btn |= (1u << b);
            }
        }
    }

    if (buttons) *buttons = btn;
    if (dx)      *dx = lx;
    if (dy)      *dy = ly;
    if (dw)      *dw = lw;
    return true;
}
