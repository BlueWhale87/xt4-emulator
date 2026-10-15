/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_BLOCK_XT4_NOR_COVERED_H
#define HW_BLOCK_XT4_NOR_COVERED_H
#include "qemu/osdep.h"

/* Borrowed immutable bytes. Capacity does not imply coverage of any hole. */
typedef struct XT4CoveredNor {
    const uint8_t *bytes;
    uint32_t offset, length, capacity;
    uint64_t virtual_base;
} XT4CoveredNor;

/* Caller supplies storage for length output bytes. On failure it is untouched.
 * detail may be NULL; otherwise success clears it and failure sets a reason. */
bool xt4_nor_covered_read(const XT4CoveredNor *s, uint64_t address, uint8_t *out,
                          size_t length, const char **detail);
#endif
