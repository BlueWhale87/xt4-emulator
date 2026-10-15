/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/block/xt4-nor-covered.h"

static const uint8_t anonymous_bytes[] = {0, 17, 34, 51, 68, 85, 102, 119};

static XT4CoveredNor span(void)
{
    return (XT4CoveredNor) {
        .bytes = anonymous_bytes, .offset = 0x2a080,
        .length = sizeof(anonymous_bytes), .capacity = 0x4000000,
        .virtual_base = 0xf0000000,
    };
}

static void reject(const XT4CoveredNor *s, uint64_t address, size_t length)
{
    uint8_t output[16], before[16];
    memset(output, 0xa5, sizeof(output));
    memcpy(before, output, sizeof(output));
    const char *detail = NULL;
    g_assert_false(xt4_nor_covered_read(s, address, output, length, &detail));
    g_assert_nonnull(detail);
    g_assert_cmpmem(output, sizeof(output), before, sizeof(before));
}

static void covered_delivery(void)
{
    XT4CoveredNor s = span(), before = s;
    uint8_t output[12];
    memset(output, 0xa5, sizeof(output));
    const char *detail = "stale";
    g_assert_true(xt4_nor_covered_read(&s, 0xf002a080, output + 2, 8, &detail));
    g_assert_null(detail);
    g_assert_cmpmem(output + 2, 8, anonymous_bytes, 8);
    g_assert_cmphex(output[0], ==, 0xa5);
    g_assert_cmphex(output[1], ==, 0xa5);
    g_assert_cmphex(output[10], ==, 0xa5);
    g_assert_cmphex(output[11], ==, 0xa5);
    g_assert_true(xt4_nor_covered_read(&s, 0xf002a082, output, 4, &detail));
    g_assert_cmpmem(output, 4, anonymous_bytes + 2, 4);
    g_assert_cmpmem(&s, sizeof(s), &before, sizeof(before));
}

static void covered_holes_and_bounds(void)
{
    XT4CoveredNor s = span();
    const struct { uint64_t address; size_t length; } invalid[] = {
        {0xefffffff, 1}, {0xf002a07f, 1}, {0xf002a07f, 2},
        {0xf002a080, 9}, {0xf002a087, 2}, {0xf002a088, 1},
        {0xf002b080, 1}, {0xf3ffffff, 2}, {0xf4000000, 1},
        {0xf002a080, SIZE_MAX}, {UINT64_MAX, 1}, {UINT64_MAX - 2, 8},
        {0, 1}, {0x14002a080ULL, 1}, {0xf002a080, 0},
    };
    for (int i = 0; i < G_N_ELEMENTS(invalid); i++) {
        reject(&s, invalid[i].address, invalid[i].length);
    }
    /* Complete covered data at capacity end works without wrapping. */
    s.offset = 0x3fffff8;
    uint8_t out[8];
    const char *detail = "stale";
    g_assert_true(xt4_nor_covered_read(&s, 0xf3fffff8, out, 8, &detail));
    g_assert_cmpmem(out, sizeof(out), anonymous_bytes, sizeof(anonymous_bytes));
    reject(&s, 0xf3fffff8, 9);
    reject(&s, 0xf4000000, 1);
}

static void covered_invalid_span(void)
{
    XT4CoveredNor s = span();
    reject(NULL, 0xf002a080, 1);
    s.bytes = NULL; reject(&s, 0xf002a080, 1);
    s = span(); s.length = 0; reject(&s, 0xf002a080, 1);
    s = span(); s.length = UINT32_MAX; reject(&s, 0xf002a080, 1);
    s = span(); s.offset = UINT32_MAX; reject(&s, 0xf002a080, 1);
    s = span(); s.capacity = UINT32_MAX; reject(&s, 0xf002a080, 1);
    s = span(); s.capacity = 0; reject(&s, 0xf002a080, 1);
    s = span(); s.virtual_base = UINT64_MAX; reject(&s, 0xf002a080, 1);
    s = span(); s.virtual_base = 0; reject(&s, 0xf002a080, 1);
    s = span(); s.offset = 0x3fffffc; reject(&s, 0xf3fffffc, 1);
    s = span();
    const char *detail = NULL;
    g_assert_false(xt4_nor_covered_read(&s, 0xf002a080, NULL, 1, &detail));
    g_assert_nonnull(detail);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xt4/nor-covered/delivery", covered_delivery);
    g_test_add_func("/xt4/nor-covered/holes-bounds", covered_holes_and_bounds);
    g_test_add_func("/xt4/nor-covered/invalid-span", covered_invalid_span);
    return g_test_run();
}
