/* X-T4 board checks. Snippets are written only to anonymous guest RAM. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"

#define ENTRY 0x012910c4
#define SENTINEL 0x01022000

static char *new_fixture_profile(const char *profile)
{
    const char *root = g_getenv("XT4_RUN_ROOT");
    g_assert_nonnull(root);
    g_assert_cmpint(g_mkdir_with_parents(root, 0700), ==, 0);
    char *tmp = g_build_filename(root, "qtest-XXXXXX", NULL);
    g_assert_nonnull(g_mkdtemp(tmp));
    char *run = g_build_filename(tmp, "run", NULL);
    const char *script = "from pathlib import Path; import sys; "
        "from xt4lab.firmware import prepare_run; "
        "prepare_run(Path(sys.argv[1]).resolve(),Path(sys.argv[2]).resolve(),sys.argv[3])";
    char *args[] = {(char *)g_getenv("XT4_PYTHON"), (char *)"-c", (char *)script,
                   (char *)g_getenv("XT4_RESEARCH_ROOT"), run, (char *)profile, NULL};
    int status;
    g_assert_true(g_spawn_sync(g_getenv("XT4_SOURCE"), args, NULL, 0, NULL,
                              NULL, NULL, NULL, &status, NULL));
    g_assert_cmpint(status, ==, 0);
    g_free(tmp);
    return run;
}

static char *new_fixture(void)
{
    return new_fixture_profile("module-entry-diagnostic");
}

static QTestState *start(const char *run, bool tcg)
{
    g_autofree char *quoted = g_shell_quote(run);
    return qtest_initf("-machine fujifilm-xt4,run-dir=%s -nodefaults -nic none -S %s",
                       quoted, tcg ? "-accel tcg,thread=single" : "");
}

static QDict *stop_record(const char *run)
{
    g_autofree char *path = g_build_filename(run, "engine-events.jsonl", NULL);
    g_autofree char *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    QDict *stop = NULL;
    for (int i = 0; lines[i] && *lines[i]; i++) {
        QDict *event = qobject_to(QDict, qobject_from_json(lines[i], NULL));
        g_assert_nonnull(event);
        if (g_strcmp0(qdict_get_try_str(event, "event"), "stop") == 0) {
            g_assert_null(stop);
            stop = event;
        } else {
            qobject_unref(event);
        }
    }
    g_assert_nonnull(stop);
    return stop;
}

static void test_xt4_memory_and_reset(void)
{
    g_autofree char *run = new_fixture();
    QTestState *q = start(run, false);
    const uint32_t bases[] = {0x01021000, 0x01e0f000, 0x03979000,
                             0x01735000, 0x0270f000, 0x7fff0000};
    const uint32_t sizes[] = {0x714000, 0x900000, 0x9c4000, 0x6da000,
                             0x126a000, 0x10000};
    for (int i = 0; i < G_N_ELEMENTS(bases); i++) {
        uint8_t initial = qtest_readb(q, bases[i] + sizes[i] - 1);
        qtest_writeb(q, bases[i] + sizes[i] - 1, initial ^ 0xff);
        qtest_system_reset(q);
        g_assert_cmpuint(qtest_readb(q, bases[i] + sizes[i] - 1), ==, initial);
    }
    g_assert_cmphex(qtest_readl(q, 0x01021004), ==, 0x01021000);
    g_assert_cmphex(qtest_readl(q, ENTRY), ==, 0xe92d4810);
    g_assert_cmpuint(qtest_readb(q, 0x017349f8), ==, 0);
    qtest_quit(q);
}

static void test_xt4_bootstrap_memory_reset(void)
{
    g_autofree char *run = new_fixture_profile("bootstrap-entry-diagnostic");
    QTestState *q = start(run, false);
    g_assert_cmphex(qtest_readl(q, 0), ==, 0xe59ff018);
    g_assert_cmphex(qtest_readl(q, 0x20), ==, 0x100);
    g_assert_cmphex(qtest_readl(q, 0xb060), ==, 1);
    qtest_writel(q, 0, 0);
    qtest_writeb(q, 0x1b0bf, 0xa5);
    qtest_system_reset(q);
    g_assert_cmphex(qtest_readl(q, 0), ==, 0xe59ff018);
    g_assert_cmpuint(qtest_readb(q, 0x1b0bf), ==, 0);
    g_autofree char *path = g_build_filename(run, "engine-events.jsonl", NULL);
    g_autofree char *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i] && *lines[i]; i++) {
        QDict *event = qobject_to(QDict, qobject_from_json(lines[i], NULL));
        g_assert_nonnull(event);
        g_assert_cmpstr(qdict_get_str(event, "profile"), ==, "bootstrap-entry-diagnostic");
        QDict *regs = qdict_get_qdict(event, "registers");
        g_assert_cmphex(qdict_get_int(regs, "r15"), ==, 0);
        g_assert_cmphex(qdict_get_int(regs, "r13"), ==, 0);
        qobject_unref(event);
    }
    qtest_quit(q);
}

static void fault_test(int which)
{
    g_autofree char *run = new_fixture();
    QTestState *q = start(run, true);
    uint32_t code[] = {0xe3022000, 0xe3402102, /* r2 = sentinel */
                       0xe3000008, 0xe34000e2, /* r0 = unknown address */
                       0xe5d01000, 0xe5820000, 0xeafffffe};
    if (which == 1) {
        code[4] = 0xe5802000; /* store r2 to unknown */
    } else if (which == 2) {
        code[4] = 0xe12fff10; /* bx r0: unmapped fetch */
    } else if (which == 3) {
        code[4] = 0xe7f000f0; /* udf */
    } else if (which == 5) {
        code[4] = 0xef000000; /* ARM svc #0 */
    } else if (which == 6) {
        code[2] = 0xe3010105; /* Thumb entry at ENTRY + 64 | 1 */
        code[3] = 0xe3400129;
        code[4] = 0xe12fff10;
    }
    for (int i = 0; i < G_N_ELEMENTS(code); i++) {
        qtest_writel(q, ENTRY + i * 4, code[i]);
    }
    if (which == 4) {
        uint32_t mmu[] = {0xee110f10, 0xe3800001, 0xee010f10, 0xf57ff06f,
                          0xe5820000, 0xeafffffe};
        for (int i = 0; i < G_N_ELEMENTS(mmu); i++) {
            qtest_writel(q, ENTRY + 8 + i * 4, mmu[i]);
        }
    }
    qtest_writel(q, SENTINEL, 0);
    if (which == 6) {
        qtest_writel(q, ENTRY + 64, 0x6010df00); /* svc; str r0,[r2] */
    }
    qtest_qmp_assert_success(q, "{ 'execute': 'cont' }");
    qtest_qmp_eventwait(q, "STOP");
    QDict *stop = stop_record(run);
    g_assert_cmpint(qdict_get_int(stop, "core"), ==, 0);
    g_assert_cmpuint(qtest_readl(q, SENTINEL), ==, 0);
    if (which < 3) {
        g_assert_cmpstr(qdict_get_str(stop, "reason"), ==, "unsupported_access");
        g_assert_cmphex(qdict_get_int(stop, "physical_address"), ==, 0x00e20008);
        g_assert_cmphex(qdict_get_int(stop, "pc"), ==,
                        which == 2 ? 0x00e20008 : ENTRY + 16);
        g_assert_cmpstr(qdict_get_str(stop, "access"), ==,
                        which == 0 ? "read" : which == 1 ? "write" : "fetch");
        if (which == 0) {
            g_assert_cmpint(qdict_get_int(stop, "width"), ==, 1);
        } else if (which == 1) {
            g_assert_cmpint(qdict_get_int(stop, "width"), ==, 4);
            g_assert_cmphex(qdict_get_int(stop, "value"), ==, SENTINEL);
        }
    } else {
        g_assert_cmpstr(qdict_get_str(stop, "reason"), ==, "unexpected_exception");
        if (which == 3 || which == 5) {
            g_assert_cmphex(qdict_get_int(stop, "pc"), ==, ENTRY + 16);
            if (which == 5) {
                g_assert_cmphex(qdict_get_int(stop, "architectural_pc"), ==, ENTRY + 20);
            }
        } else if (which == 6) {
            g_assert_cmphex(qdict_get_int(stop, "pc"), ==, ENTRY + 64);
            g_assert_cmphex(qdict_get_int(stop, "architectural_pc"), ==, ENTRY + 66);
        } else {
            g_assert_cmpuint(qdict_get_int(stop, "pc"), >=, ENTRY + 16);
            g_assert_cmpuint(qdict_get_int(stop, "pc"), <=, ENTRY + 24);
        }
    }
    qobject_unref(stop);
    qtest_quit(q);
}

static void read_fault(void) { fault_test(0); }
static void write_fault(void) { fault_test(1); }
static void fetch_fault(void) { fault_test(2); }
static void undefined_fault(void) { fault_test(3); }
static void mmu_fault(void) { fault_test(4); }
static void arm_svc(void) { fault_test(5); }
static void thumb_svc(void) { fault_test(6); }


static QDict *port_record(const char *run)
{
    g_autofree char *path = g_build_filename(run, "engine-events.jsonl", NULL);
    g_autofree char *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    QDict *last = NULL;
    for (int i = 0; lines[i] && *lines[i]; i++) {
        QDict *event = qobject_to(QDict, qobject_from_json(lines[i], NULL));
        if (!g_strcmp0(qdict_get_try_str(event, "event"), "device-access")) {
            if (last) {
                qobject_unref(last);
            }
            last = event;
        } else {
            qobject_unref(event);
        }
    }
    g_assert_nonnull(last);
    return last;
}

static void port_state(const char *run, bool success, uint32_t state,
                       uint32_t known)
{
    QDict *event = port_record(run);
    g_assert_cmpstr(qdict_get_str(event, "device"), ==, "xt4-port-config-v1");
    g_assert_cmpint(qdict_get_bool(event, "success"), ==, success);
    g_assert_cmphex(qdict_get_int(event, "state"), ==, state);
    g_assert_cmphex(qdict_get_int(event, "known_mask"), ==, known);
    qobject_unref(event);
}

static void port_masked_updates(void)
{
    g_autofree char *run = new_fixture_profile("bootstrap-port-config-diagnostic");
    QTestState *q = start(run, false);
    for (int selector = 0; selector < 3; selector++) {
        uint32_t address = 0xfff41000 + selector * 4;
        qtest_writel(q, address, 0xffff9000);
        qtest_writel(q, address, 0x00010001);
        port_state(run, true, 0x9001, 0xffff);
        qtest_writel(q, address, 0x10000000);
        port_state(run, true, 0x8001, 0xffff);
        qtest_readl(q, address);
        port_state(run, false, 0x8001, 0xffff);
    }
    qtest_writel(q, 0xfff413c0, 0x00010001); /* port15 */
    port_state(run, true, 1, 1);
    qtest_writel(q, 0xfff41500, 0x00020002); /* separate global */
    port_state(run, true, 2, 2);
    qtest_readl(q, 0xfff41500);
    port_state(run, false, 2, 2);
    qtest_writel(q, 0xfff41000, 0); /* no-op, prior port0 state */
    port_state(run, true, 0x8001, 0xffff);
    qtest_quit(q);
}

static void port_two_bit_masks(void)
{
    g_autofree char *run = new_fixture_profile("bootstrap-port-config-diagnostic");
    QTestState *q = start(run, false);
    for (int selector = 3; selector <= 4; selector++) {
        uint32_t address = 0xfff41000 + selector * 4;
        qtest_writel(q, address, 0x00030002);
        port_state(run, true, 2, 3);
        qtest_writel(q, address, 0x000c0004);
        port_state(run, true, 6, 15);
        qtest_writel(q, address, 0x00010001); /* unsupported half field */
        port_state(run, false, 6, 15);
        qtest_writel(q, address, 0xffff1555);
        port_state(run, true, 0x1555, 0xffff);
    }
    qtest_quit(q);
}

static void port_full_word_rmw(void)
{
    g_autofree char *run = new_fixture_profile("bootstrap-port-config-diagnostic");
    QTestState *q = start(run, false);
    for (int selector = 6; selector <= 7; selector++) {
        uint32_t address = 0xfff41000 + selector * 4;
        qtest_writel(q, address, 0xc0ff3f0f);
        g_assert_cmphex(qtest_readl(q, address), ==, 0xc0ff3f0f);
        qtest_writel(q, address, 0xc0ff3f4f); /* replace bits6..7 only */
        g_assert_cmphex(qtest_readl(q, address), ==, 0xc0ff3f4f);
        port_state(run, true, 0xc0ff3f4f, 0xffffffff);
    }
    qtest_writel(q, 0xfff4109c, 0x00015555);
    g_assert_cmphex(qtest_readl(q, 0xfff4109c), ==, 0x00015555);
    qtest_readl(q, 0xfff410dc); /* selector7, different port, unknown */
    port_state(run, false, 0, 0);
    qtest_quit(q);
}

static void port_reset(void)
{
    g_autofree char *run = new_fixture_profile("bootstrap-port-config-diagnostic");
    QTestState *q = start(run, false);
    qtest_writel(q, 0xfff41018, 0x12345678);
    qtest_system_reset(q);
    qtest_readl(q, 0xfff41018);
    port_state(run, false, 0, 0);
    qtest_writel(q, 0xfff41000, 0x00010001);
    port_state(run, true, 1, 1);
    qtest_readl(q, 0xfff41000);
    port_state(run, false, 1, 1);
    qtest_quit(q);
}

static uint32_t mov_half(int reg, uint16_t value, bool upper)
{
    return (upper ? 0xe3400000 : 0xe3000000) |
           ((value & 0xf000) << 4) | (reg << 12) | (value & 0xfff);
}

static void port_fault(uint32_t address, uint32_t op, bool write,
                       unsigned width, uint64_t value, const char *detail,
                       bool preinitialize)
{
    g_autofree char *run = new_fixture_profile("bootstrap-port-config-diagnostic");
    QTestState *q = start(run, true);
    if (preinitialize) {
        qtest_writel(q, address, 0xffff0000);
    }
    if (!detail) {
        qtest_readl(q, 0xfff41014); /* catch-all must clear this stale detail */
    }
    uint32_t code[] = {
        mov_half(0, address, false), mov_half(0, address >> 16, true),
        mov_half(1, 1, false), mov_half(1, 1, true),
        mov_half(2, 0x7788, false), mov_half(2, 0x5566, true),
        mov_half(3, 0x3344, false), mov_half(3, 0x1122, true),
        mov_half(4, 0xa000, false), mov_half(4, 1, true),
        op, 0xe5841000, 0xeafffffe,
    };
    for (int i = 0; i < G_N_ELEMENTS(code); i++) {
        qtest_writel(q, i * 4, code[i]);
    }
    qtest_writel(q, 0x1a000, 0);
    qtest_qmp_assert_success(q, "{ 'execute': 'cont' }");
    qtest_qmp_eventwait(q, "STOP");
    QDict *stop = stop_record(run);
    if (address & 3) {
        /* Device-memory alignment traps before the MMIO callback. */
        g_assert_cmpstr(qdict_get_str(stop, "reason"), ==, "unexpected_exception");
        g_assert_cmphex(qdict_get_int(stop, "pc"), ==, 40);
        g_assert_cmphex(qdict_get_int(stop, "virtual_address"), ==, address);
        g_assert_cmpint(qobject_type(qdict_get(stop, "width")), ==, QTYPE_QNULL);
        g_assert_cmpint(qobject_type(qdict_get(stop, "value")), ==, QTYPE_QNULL);
    } else {
        g_assert_cmpstr(qdict_get_str(stop, "reason"), ==, "unsupported_access");
        g_assert_cmphex(qdict_get_int(stop, "pc"), ==, 40);
        g_assert_cmphex(qdict_get_int(stop, "physical_address"), ==, address);
        g_assert_cmpuint(qdict_get_int(stop, "width"), ==, width);
        g_assert_cmpstr(qdict_get_str(stop, "access"), ==, write ? "write" : "read");
        if (write) {
            g_assert_cmphex(qdict_get_int(stop, "value"), ==, value);
        } else {
            g_assert_cmpint(qobject_type(qdict_get(stop, "value")), ==, QTYPE_QNULL);
        }
        if (detail) {
            g_assert_cmpstr(qdict_get_str(stop, "device"), ==, "xt4-port-config-v1");
            g_assert_cmpstr(qdict_get_str(stop, "detail"), ==, detail);
        } else {
            g_assert_false(qdict_haskey(stop, "device"));
        }
    }
    g_assert_cmpuint(qtest_readl(q, 0x1a000), ==, 0);
    qobject_unref(stop);
    qtest_quit(q);
}

static void port_unknown_reads(void)
{
    port_fault(0xfff41014, 0xe5901000, false, 4, 0, "unknown_port_input", false);
    for (int s = 0; s < 8; s++) {
        if (s == 5) {
            continue;
        }
        port_fault(0xfff41000 + s * 4, 0xe5901000, false, 4, 0,
                   "unknown_port_readback", s < 5);
    }
    port_fault(0xfff41500, 0xe5901000, false, 4, 0, "unknown_port_readback", true);
}

static void port_invalid_operations(void)
{
    port_fault(0xfff41000, 0xe5c01000, true, 1, 1, "unsupported_port_access", false);
    port_fault(0xfff41000, 0xe1c010b0, true, 2, 1, "unsupported_port_access", false);
    port_fault(0xfff41000, 0xe1c020f0, true, 8, 0x1122334455667788ULL, "unsupported_port_access", false);
    port_fault(0xfff41001, 0xe5801000, true, 4, 0x10001, "unsupported_port_access", false);
    port_fault(0xfff41020, 0xe5801000, true, 4, 0x10001, "unsupported_port_access", false);
    port_fault(0xfff41014, 0xe5801000, true, 4, 0x10001, "unsupported_port_write", false);
    port_fault(0xfff4100c, 0xe5801000, true, 4, 0x10001, "unsupported_port_mask", false);
    port_fault(0xfff41400, 0xe5801000, true, 4, 0x10001, NULL, false);
    g_autofree char *run = new_fixture_profile("bootstrap-port-config-diagnostic");
    QTestState *q = start(run, false);
    qtest_writel(q, 0xfff41001, 0x00010001);
    port_state(run, false, 0, 0);
    qtest_writeq(q, 0xfff41018, 0x1122334455667788ULL);
    QDict *event = port_record(run);
    g_assert_false(qdict_get_bool(event, "success"));
    g_assert_cmpuint(qdict_get_int(event, "width"), ==, 8);
    g_assert_cmpint(qdict_get_int(event, "known_mask"), ==, 0);
    qobject_unref(event);
    qtest_readl(q, 0xfff41018);
    port_state(run, false, 0, 0);
    qtest_writeq(q, 0xfff41500, 0x1122334455667788ULL);
    event = port_record(run);
    g_assert_false(qdict_get_bool(event, "success"));
    g_assert_cmpuint(qdict_get_int(event, "width"), ==, 8);
    qobject_unref(event);
    qtest_writel(q, 0xfff41500, 0);
    port_state(run, true, 0, 0); /* wide write must not define any low bits */
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/xt4/memory-reset", test_xt4_memory_and_reset);
    qtest_add_func("/xt4/bootstrap-memory-reset", test_xt4_bootstrap_memory_reset);
    qtest_add_func("/xt4/fault/read", read_fault);
    qtest_add_func("/xt4/fault/write", write_fault);
    qtest_add_func("/xt4/fault/fetch", fetch_fault);
    qtest_add_func("/xt4/fault/undefined", undefined_fault);
    qtest_add_func("/xt4/fault/mmu", mmu_fault);
    qtest_add_func("/xt4/fault/arm-svc", arm_svc);
    qtest_add_func("/xt4/fault/thumb-svc", thumb_svc);
    qtest_add_func("/xt4/ports/masked", port_masked_updates);
    qtest_add_func("/xt4/ports/two-bit", port_two_bit_masks);
    qtest_add_func("/xt4/ports/full-word", port_full_word_rmw);
    qtest_add_func("/xt4/ports/reset", port_reset);
    qtest_add_func("/xt4/ports/unknown-reads", port_unknown_reads);
    qtest_add_func("/xt4/ports/invalid", port_invalid_operations);
    return g_test_run();
}
