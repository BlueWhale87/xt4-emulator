/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/block/xt4-nor-boot.h"

static XT4NorRequest request(uint8_t opcode)
{
    return (XT4NorRequest) { .protocol = XT4_NOR_SPI, .opcode = opcode };
}

static XT4NorEffect prepare(XT4NorState *s, XT4NorRequest r)
{
    XT4NorState before = *s;
    XT4NorEffect e;
    const char *detail = "not cleared";
    g_assert_true(xt4_nor_boot_prepare(s, &r, &e, &detail));
    g_assert_null(detail);
    g_assert_cmpmem(s, sizeof(*s), &before, sizeof(before));
    g_assert_cmpuint(e.expected_generation, ==, s->generation);
    return e;
}

static void commit(XT4NorState *s, const XT4NorEffect *e)
{
    const char *detail = "not cleared";
    uint64_t old = s->generation;
    g_assert_true(xt4_nor_boot_commit(s, e, &detail));
    g_assert_null(detail);
    g_assert_cmpuint(s->generation, >, old);
}

static void reject(XT4NorState *s, XT4NorRequest r)
{
    XT4NorState before = *s;
    XT4NorEffect e, original;
    const char *detail = NULL;
    memset(&e, 0xa5, sizeof(e));
    memcpy(&original, &e, sizeof(e));
    g_assert_false(xt4_nor_boot_prepare(s, &r, &e, &detail));
    g_assert_nonnull(detail);
    g_assert_cmpmem(&e, sizeof(e), &original, sizeof(original));
    g_assert_cmpmem(s, sizeof(*s), &before, sizeof(before));
}

static XT4NorRequest configuration(void)
{
    XT4NorRequest r = request(0x72);
    r.address_bytes = 4;
    r.tx_len = 1;
    r.tx[0] = 2;
    return r;
}

static void enter_opi(XT4NorState *s)
{
    XT4NorEffect e = prepare(s, request(6));
    commit(s, &e);
    e = prepare(s, configuration());
    commit(s, &e);
    g_assert_cmpint(s->protocol, ==, XT4_NOR_OPI_DTR);
    g_assert_false(s->write_enabled);
}

static void spi_id(void)
{
    XT4NorState s = {0};
    xt4_nor_boot_reset(&s);
    const uint8_t id[] = {0xc2, 0x80, 0x3a};
    for (int n = 1; n <= 3; n++) {
        XT4NorRequest r = request(0x9f);
        r.rx_len = n;
        XT4NorEffect e = prepare(&s, r);
        g_assert_cmpuint(e.rx_len, ==, n);
        g_assert_cmpmem(e.rx, n, id, n);
        g_assert_false(e.state_change);
        g_assert_false(e.ignored_write);
        g_assert_cmpuint(e.settle_ns, ==, 0);
        commit(&s, &e);
    }
}

static void spi_wel_status(void)
{
    XT4NorState s = {0};
    xt4_nor_boot_reset(&s);
    for (int round = 0; round < 3; round++) {
        XT4NorEffect e;
        if (round) {
            e = prepare(&s, request(round == 1 ? 6 : 4));
            g_assert_cmpint(e.state_change, ==, true);
            g_assert_cmpint(s.write_enabled, ==, round == 2);
            commit(&s, &e);
        }
        for (int addr = 0; addr <= 4; addr += 4) {
            for (int n = 1; n <= 8; n++) {
                XT4NorRequest r = request(5);
                r.address_bytes = addr;
                r.rx_len = n;
                e = prepare(&s, r);
                g_assert_cmpuint(e.rx_len, ==, n);
                for (int i = 0; i < n; i++) {
                    g_assert_cmphex(e.rx[i], ==, round == 1 ? 2 : 0);
                }
                commit(&s, &e);
            }
        }
    }
}

static void mode_change_and_ignored_write(void)
{
    XT4NorState s = {0};
    xt4_nor_boot_reset(&s);
    XT4NorEffect e = prepare(&s, configuration());
    g_assert_true(e.ignored_write);
    g_assert_false(e.state_change);
    g_assert_cmpuint(e.settle_ns, ==, 0);
    commit(&s, &e);
    g_assert_cmpint(s.protocol, ==, XT4_NOR_SPI);
    e = prepare(&s, request(6));
    g_assert_false(s.write_enabled);
    commit(&s, &e);
    e = prepare(&s, configuration());
    g_assert_cmpint(s.protocol, ==, XT4_NOR_SPI);
    g_assert_true(s.write_enabled);
    g_assert_true(e.state_change);
    g_assert_false(e.ignored_write);
    g_assert_cmpuint(e.settle_ns, ==, 40);
    commit(&s, &e);
    g_assert_cmpint(s.protocol, ==, XT4_NOR_OPI_DTR);
    g_assert_false(s.write_enabled);
    reject(&s, request(6));
}

static void opi_status(void)
{
    XT4NorState s = {0};
    xt4_nor_boot_reset(&s);
    enter_opi(&s);
    for (int round = 0; round < 2; round++) {
        XT4NorRequest r = request(6);
        r.protocol = XT4_NOR_OPI_DTR;
        r.has_extension = true;
        r.extension = 0xf9;
        if (round) {
            XT4NorEffect e = prepare(&s, r);
            commit(&s, &e);
        }
        r.opcode = 5;
        r.extension = 0xfa;
        r.address_bytes = 4;
        r.dummy_cycles = 4;
        for (int n = 2; n <= 8; n += 2) {
            r.rx_len = n;
            XT4NorEffect e = prepare(&s, r);
            g_assert_cmpuint(e.rx_len, ==, n);
            for (int i = 0; i < n; i++) {
                g_assert_cmphex(e.rx[i], ==, round ? 2 : 0);
            }
            commit(&s, &e);
        }
    }
    XT4NorRequest r = request(4);
    r.protocol = XT4_NOR_OPI_DTR;
    r.has_extension = true;
    r.extension = 0xfb;
    XT4NorEffect e = prepare(&s, r);
    commit(&s, &e);
    g_assert_false(s.write_enabled);
}

static void malformed_spi(void)
{
    XT4NorState s = {0};
    xt4_nor_boot_reset(&s);
    XT4NorRequest r;
    for (int kind = 0; kind < 18; kind++) {
        r = request(0x9f);
        r.rx_len = 1;
        switch (kind) {
        case 0: r.rx_len = 0; break;
        case 1: r.rx_len = 4; break;
        case 2: r.tx_len = 1; break;
        case 3: r.address_bytes = 4; break;
        case 4: r.dummy_cycles = 1; break;
        case 5: r.has_extension = true; break;
        case 6: r.protocol = XT4_NOR_OPI_DTR; break;
        case 7: r.opcode = 0xee; break;
        case 8: r.rx_len = 255; break;
        case 9: r.tx_len = 255; break;
        case 10: r = configuration(); r.tx[0] = 3; break;
        case 11: r = configuration(); r.address = 1; break;
        case 12: r = configuration(); r.address_bytes = 3; break;
        case 13: r = configuration(); r.rx_len = 1; break;
        case 14: r = configuration(); r.tx_len = 0; break;
        case 15: r = request(5); r.rx_len = 1; r.address_bytes = 2; break;
        case 16: r = request(5); r.rx_len = 1; r.address_bytes = 4; r.address = 1; break;
        default: r = request(6); r.rx_len = 1; break;
        }
        reject(&s, r);
    }
}

static void malformed_opi(void)
{
    XT4NorState s = {0};
    xt4_nor_boot_reset(&s);
    enter_opi(&s);
    for (int kind = 0; kind < 10; kind++) {
        XT4NorRequest r = request(5);
        r.protocol = XT4_NOR_OPI_DTR;
        r.has_extension = true;
        r.extension = 0xfa;
        r.address_bytes = r.dummy_cycles = 4;
        r.rx_len = 2;
        switch (kind) {
        case 0: r.extension = 0xfb; break;
        case 1: r.has_extension = false; break;
        case 2: r.protocol = XT4_NOR_SPI; break;
        case 3: r.address_bytes = 0; break;
        case 4: r.address = 1; break;
        case 5: r.dummy_cycles = 0; break;
        case 6: r.rx_len = 1; break;
        case 7: r.rx_len = 3; break;
        case 8: r.tx_len = 1; break;
        default: r.opcode = 0xee; r.extension = 0x11; break;
        }
        reject(&s, r);
    }
}

static void stale_generation_and_reset(void)
{
    XT4NorState s = {0};
    xt4_nor_boot_reset(&s);
    XT4NorEffect a = prepare(&s, request(6));
    XT4NorEffect b = prepare(&s, request(4));
    commit(&s, &a);
    XT4NorState before = s;
    const char *detail = NULL;
    g_assert_false(xt4_nor_boot_commit(&s, &a, &detail));
    g_assert_nonnull(detail);
    g_assert_cmpmem(&s, sizeof(s), &before, sizeof(before));
    g_assert_false(xt4_nor_boot_commit(&s, &b, &detail));
    g_assert_cmpmem(&s, sizeof(s), &before, sizeof(before));
    a = prepare(&s, configuration());
    xt4_nor_boot_reset(&s);
    before = s;
    g_assert_false(xt4_nor_boot_commit(&s, &a, &detail));
    g_assert_cmpmem(&s, sizeof(s), &before, sizeof(before));
    g_assert_cmpint(s.protocol, ==, XT4_NOR_SPI);
    g_assert_false(s.write_enabled);
}

static void selector2c_query_only(void)
{
    XT4NorState s = { .part = XT4_NOR_PART_SELECTOR2C };
    xt4_nor_boot_reset(&s);
    XT4NorRequest r = request(0x9f);
    r.rx_len = 1;
    XT4NorEffect e = prepare(&s, r);
    g_assert_cmpuint(e.rx_len, ==, 1);
    g_assert_cmpuint(e.rx[0], ==, 0x2c);
    g_assert_false(e.state_change);
    g_assert_cmpuint(e.settle_ns, ==, 0);
    commit(&s, &e);
    for (int n = 0; n <= 8; n++) {
        r.rx_len = n;
        if (n != 1) {
            reject(&s, r);
        }
    }
    r.rx_len = 1;
    r.address_bytes = 1; reject(&s, r); r.address_bytes = 0;
    r.dummy_cycles = 1; reject(&s, r); r.dummy_cycles = 0;
    r.tx_len = 1; reject(&s, r); r.tx_len = 0;
    r.has_extension = true; reject(&s, r); r.has_extension = false;
    r.protocol = XT4_NOR_OPI_DTR; reject(&s, r);
    reject(&s, request(6));
    r = request(5); r.rx_len = 1; reject(&s, r);
    reject(&s, configuration());
    xt4_nor_boot_reset(&s);
    r = request(0x9f); r.rx_len = 1;
    e = prepare(&s, r);
    g_assert_cmpuint(e.rx[0], ==, 0x2c);
    s.part = 99;
    reject(&s, r);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xt4/nor/spi-id", spi_id);
    g_test_add_func("/xt4/nor/spi-wel-status", spi_wel_status);
    g_test_add_func("/xt4/nor/mode-change", mode_change_and_ignored_write);
    g_test_add_func("/xt4/nor/opi-status", opi_status);
    g_test_add_func("/xt4/nor/malformed-spi", malformed_spi);
    g_test_add_func("/xt4/nor/malformed-opi", malformed_opi);
    g_test_add_func("/xt4/nor/stale-reset", stale_generation_and_reset);
    g_test_add_func("/xt4/nor/selector2c-query-only", selector2c_query_only);
    return g_test_run();
}
