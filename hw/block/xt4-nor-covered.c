/* SPDX-License-Identifier: GPL-2.0-or-later
 * Strict virtual-window lookup. No file backend, wrapping or unknown fill.
 */
#include "qemu/osdep.h"
#include "hw/block/xt4-nor-covered.h"

bool xt4_nor_covered_read(const XT4CoveredNor *s, uint64_t address, uint8_t *out,
                          size_t length, const char **detail)
{
    const char *reason;
    uint64_t offset;
    if (!s || !s->bytes || !out || !length || !s->length ||
        s->virtual_base != UINT64_C(0xf0000000) || s->capacity != 0x4000000 ||
        s->offset > s->capacity || s->length > s->capacity - s->offset) {
        reason = "invalid_covered_nor_span_or_request";
    } else if (address < s->virtual_base ||
               (offset = address - s->virtual_base) >= s->capacity ||
               length > s->capacity - offset) {
        reason = "unsupported_covered_nor_range";
    } else if (offset < s->offset || offset - s->offset >= s->length ||
               length > s->length - (offset - s->offset)) {
        reason = "unknown_covered_nor_bytes";
    } else {
        memmove(out, s->bytes + (offset - s->offset), length);
        if (detail) {
            *detail = NULL;
        }
        return true;
    }
    if (detail) {
        *detail = reason;
    }
    return false;
}
