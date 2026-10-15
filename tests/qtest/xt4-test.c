/* X-T4 board checks. Snippets are written only to anonymous guest RAM. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qnum.h"

#define ENTRY 0x012910c4
#define SENTINEL 0x01022000

static char *new_fixture_scenario(const char *profile, const char *scenario)
{
    const char *root = g_getenv("XT4_RUN_ROOT");
    g_assert_nonnull(root);
    g_assert_cmpint(g_mkdir_with_parents(root, 0700), ==, 0);
    char *tmp = g_build_filename(root, "qtest-XXXXXX", NULL);
    g_assert_nonnull(g_mkdtemp(tmp));
    char *run = g_build_filename(tmp, "run", NULL);
    const char *script = "from pathlib import Path; import sys; "
        "from xt4lab.firmware import prepare_run; "
        "prepare_run(Path(sys.argv[1]).resolve(),Path(sys.argv[2]).resolve(),sys.argv[3],"
        "scenario_name=sys.argv[4] or None)";
    char *args[] = {(char *)g_getenv("XT4_PYTHON"), (char *)"-c", (char *)script,
                   (char *)g_getenv("XT4_RESEARCH_ROOT"), run, (char *)profile,
                   (char *)scenario, NULL};
    int status;
    g_assert_true(g_spawn_sync(g_getenv("XT4_SOURCE"), args, NULL, 0, NULL,
                              NULL, NULL, NULL, &status, NULL));
    g_assert_cmpint(status, ==, 0);
    g_free(tmp);
    return run;
}

static char *new_fixture_profile(const char *profile)
{
    return new_fixture_scenario(profile, "");
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

static QTestState *start_budget(const char *run, uint64_t budget)
{
    g_autofree char *quoted = g_shell_quote(run);
    return qtest_initf("-machine fujifilm-xt4,run-dir=%s,instruction-budget=%" PRIu64
                      " -nodefaults -nic none -S -accel tcg,thread=single"
                      " -icount shift=0,align=off,sleep=off", quoted, budget);
}

static QDict *execution_record(const char *run)
{
    g_autofree char *path = g_build_filename(run, "engine-events.jsonl", NULL);
    g_autofree char *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    QDict *last = NULL;
    for (int i = 0; lines[i] && *lines[i]; i++) {
        QDict *event = qobject_to(QDict, qobject_from_json(lines[i], NULL));
        if (!g_strcmp0(qdict_get_try_str(event, "event"), "execution-state")) {
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

static void fault_test_mode(int which, bool bounded)
{
    g_autofree char *run = new_fixture();
    QTestState *q = bounded ? start_budget(run, 100) : start(run, true);
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
    if (bounded) {
        QDict *accounting = execution_record(run);
        g_assert_cmpint(qdict_get_int(accounting, "engine_instruction_count"), >=, 0);
        g_assert_cmpint(qdict_get_int(accounting, "engine_instruction_count"), <, 100);
        qobject_unref(accounting);
    }
    qobject_unref(stop);
    qtest_quit(q);
}

static void fault_test(int which) { fault_test_mode(which, false); }

static void budget_strict_faults(void)
{
    for (int i = 0; i < 7; i++) {
        fault_test_mode(i, true);
    }
}

static void budget_exact(void)
{
    const unsigned budgets[] = {1, 2, 3, 4, 5, 63, 65535, 65536, 65537, 1000003};
    for (int i = 0; i < G_N_ELEMENTS(budgets); i++) {
        unsigned n = budgets[i];
        g_autofree char *run = new_fixture_profile("bootstrap-entry-diagnostic");
        QTestState *q = start_budget(run, n);
        const uint32_t code[] = {0xe3a00000, 0xe3012000, /* mov r0,0; movw r2,1000 */
                                 0xe2800001, 0xe5820000, 0xeafffffc};
        for (int j = 0; j < G_N_ELEMENTS(code); j++) {
            qtest_writel(q, j * 4, code[j]);
        }
        qtest_writel(q, 0x1000, 0);
        qtest_qmp_assert_success(q, "{ 'execute': 'cont' }");
        qtest_qmp_eventwait(q, "STOP");
        QDict *stop = stop_record(run);
        QDict *accounting = execution_record(run);
        g_assert_cmpstr(qdict_get_str(stop, "reason"), ==, "instruction_budget");
        g_assert_cmpint(qdict_get_int(accounting, "engine_instruction_count"), ==, n);
        g_assert_false(qdict_get_bool(accounting, "running"));
        g_assert_cmphex(qdict_get_int(stop, "pc"), ==,
                       n == 1 ? 4 : 8 + ((n - 2) % 3) * 4);
        g_assert_cmpuint(qtest_readl(q, 0x1000), ==, (n - 1) / 3);
        g_assert_true(qdict_haskey(stop, "value"));
        g_assert_false(qdict_haskey(stop, "device"));
        if (i == 0) {
            QDict *mutation = qtest_qmp(q, "{ 'execute': 'qom-set', 'arguments': {"
                "'path': '/machine', 'property': 'instruction-budget', 'value': 1000 } }");
            g_assert_true(qdict_haskey(mutation, "error"));
            qobject_unref(mutation);
        }
        qobject_unref(accounting);
        qobject_unref(stop);
        qtest_quit(q);
    }
}

static void budget_pause_reset_idle_warp(void)
{
    g_autofree char *run = new_fixture_profile("bootstrap-entry-diagnostic");
    QTestState *q = start_budget(run, 100);
    /* Architectural physical timer drives idle warp, but has no wired IRQ.
     * All snippets are test-only anonymous RAM, never firmware file changes. */
    const uint32_t code[] = {0xe3a00064, 0xee0e0f12, /* TVAL=100 */
                            0xe3a00001, 0xee0e0f32, /* CTL=enable */
                            0xe320f003, 0xeafffffd}; /* WFI; repeat WFI */
    for (int j = 0; j < G_N_ELEMENTS(code); j++) {
        qtest_writel(q, j * 4, code[j]);
    }
    qtest_qmp_assert_success(q, "{ 'execute': 'cont' }");
    qtest_qmp_eventwait(q, "STOP");
    /* The unwired generic timer eventually schedules its rollover at
     * INT64_MAX. This must stop cleanly as an unsupported clock boundary,
     * never invent 100 executed instructions or spin in timer delivery. */
    QDict *boundary = stop_record(run);
    g_assert_cmpstr(qdict_get_str(boundary, "reason"), ==, "unsupported_execution_deadline");
    qobject_unref(boundary);
    QDict *first = execution_record(run);
    int64_t count = qdict_get_int(first, "engine_instruction_count");
    g_assert_cmpint(count, >, 0);
    g_assert_cmpint(count, <, 100);
    g_assert_cmpint(qdict_get_int(first, "virtual_ns"), >, count);
    qobject_unref(first);
    /* A warm system reset does not rewind QEMU's global icount clock.
     * Like RunSession.reset(), use a fresh engine after clock exhaustion. */
    qtest_quit(q);
    g_free(run);
    run = new_fixture_profile("bootstrap-entry-diagnostic");
    q = start_budget(run, 100);
    qtest_writel(q, 0, 0xe320f003); /* WFI without any wake source */
    qtest_writel(q, 4, 0xeafffffe);
    qtest_qmp_assert_success(q, "{ 'execute': 'cont' }");
    g_usleep(20000);
    qtest_qmp_assert_success(q, "{ 'execute': 'stop' }");
    qtest_qmp_eventwait(q, "STOP");
    QDict *second = execution_record(run);
    count = qdict_get_int(second, "engine_instruction_count");
    g_assert_cmpint(count, >, 0);
    g_assert_cmpint(count, <, 100);
    qobject_unref(second);
    g_usleep(20000);
    qtest_qmp_assert_success(q, "{ 'execute': 'cont' }");
    g_usleep(20000);
    qtest_qmp_assert_success(q, "{ 'execute': 'stop' }");
    qtest_qmp_eventwait(q, "STOP");
    second = execution_record(run);
    g_assert_cmpint(qdict_get_int(second, "engine_instruction_count"), ==, count);
    qobject_unref(second);
    qtest_system_reset(q);
    /* Reset must discard the old baseline/budget and original bytes restore. */
    qtest_writel(q, 0, 0xeafffffe); /* b . */
    qtest_qmp_assert_success(q, "{ 'execute': 'cont' }");
    qtest_qmp_eventwait(q, "STOP");
    QDict *last = execution_record(run);
    g_assert_cmpint(qdict_get_int(last, "engine_instruction_count"), ==, 100);
    qobject_unref(last);
    qtest_quit(q);
}

static void budget_timer_order(void)
{
    for (int budget = 46; budget <= 48; budget++) {
        g_autofree char *run = new_fixture_scenario("bootstrap-timer-exploration", "zero-ports-v1");
        QTestState *q = start_budget(run, budget);
        const uint32_t code[] = {0xe3012000, 0xe3a00002,
            0xe30c1310, 0xe34f1ff3, 0xe5810000, 0xe3000001,
            0xe3400001, 0xe2411010, 0xe5810000};
        for (int j = 0; j < G_N_ELEMENTS(code); j++) {
            qtest_writel(q, j * 4, code[j]);
        }
        for (int j = 9; j < 65; j++) {
            qtest_writel(q, j * 4, 0xe1a00000); /* nop */
        }
        qtest_writel(q, 65 * 4, 0xe5820000); /* forbidden later sentinel */
        qtest_writel(q, 0x1000, 0);
        qtest_qmp_assert_success(q, "{ 'execute': 'cont' }");
        qtest_qmp_eventwait(q, "STOP");
        QDict *stop = stop_record(run);
        QDict *accounting = execution_record(run);
        g_assert_cmpint(qdict_get_int(accounting, "engine_instruction_count"), ==, MIN(budget, 47));
        if (budget == 46) {
            g_assert_cmpstr(qdict_get_str(stop, "reason"), ==, "instruction_budget");
        } else if (budget == 48) {
            g_assert_cmpstr(qdict_get_str(stop, "reason"), ==, "unsupported_device_transition");
        } else {
            g_assert_true(!strcmp(qdict_get_str(stop, "reason"), "instruction_budget") ||
                          !strcmp(qdict_get_str(stop, "reason"), "unsupported_device_transition"));
        }
        g_assert_cmpuint(qtest_readl(q, 0x1000), ==, 0);
        qobject_unref(accounting);
        qobject_unref(stop);
        qtest_quit(q);
    }
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

static void assert_unsigned_value(QDict *record, uint64_t expected)
{
    QNum *number = qobject_to(QNum, qdict_get(record, "value"));
    uint64_t actual;

    g_assert_nonnull(number);
    g_assert_true(qnum_get_try_uint(number, &actual));
    g_assert_cmphex(actual, ==, expected);
}

static void device_fault(const char *profile, const char *scenario, const char *model,
                       uint32_t address, uint32_t op, bool write,
                       unsigned width, uint64_t value, const char *detail,
                       bool preinitialize)
{
    g_autofree char *run = new_fixture_scenario(profile, scenario);
    g_autofree char *quoted = g_shell_quote(run);
    QTestState *q = (!strcmp(profile, "bootstrap-timer-exploration") ||
                    !strcmp(profile, "bootstrap-controller-exploration") ||
                    !strcmp(profile, "bootstrap-flash-exploration") ||
                    !strcmp(profile, "bootstrap-phy-exploration")) ?
        qtest_initf("-machine fujifilm-xt4,run-dir=%s -nodefaults -nic none -S "
                    "-accel tcg,thread=single -icount shift=0,align=off,sleep=off", quoted) :
        start(run, true);
    if (preinitialize) {
        qtest_writel(q, address, 0xffff0000);
    }
    if (!detail) {
        qtest_readl(q, 0xfff41014); /* catch-all must clear this stale detail */
    }
    uint32_t store = write && width != 8 ? value : 0x10001;
    uint32_t code[] = {
        mov_half(0, address, false), mov_half(0, address >> 16, true),
        mov_half(1, store, false), mov_half(1, store >> 16, true),
        mov_half(2, value, false), mov_half(2, value >> 16, true),
        mov_half(3, value >> 32, false), mov_half(3, value >> 48, true),
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
            assert_unsigned_value(stop, value);
        } else {
            g_assert_cmpint(qobject_type(qdict_get(stop, "value")), ==, QTYPE_QNULL);
        }
        if (detail) {
            g_assert_cmpstr(qdict_get_str(stop, "device"), ==, model);
            g_assert_cmpstr(qdict_get_str(stop, "detail"), ==, detail);
            if (write) {
                QDict *event = port_record(run);
                g_assert_false(qdict_get_bool(event, "success"));
                assert_unsigned_value(event, value);
                qobject_unref(event);
            }
        } else {
            g_assert_false(qdict_haskey(stop, "device"));
        }
    }
    g_assert_cmpuint(qtest_readl(q, 0x1a000), ==, 0);
    qobject_unref(stop);
    qtest_quit(q);
}

static void port_fault(uint32_t address, uint32_t op, bool write,
                       unsigned width, uint64_t value, const char *detail,
                       bool preinitialize)
{
    device_fault("bootstrap-port-config-diagnostic", "", "xt4-port-config-v1",
                  address, op, write, width, value, detail, preinitialize);
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

static void port_unsigned_fault_values(void)
{
    const uint64_t values[] = {
        UINT64_C(0x8000000000000000),
        UINT64_C(0xfedcba9876543210),
        UINT64_MAX,
    };

    for (int i = 0; i < G_N_ELEMENTS(values); i++) {
        port_fault(0xfff41018, 0xe1c020f0, true, 8, values[i],
                   "unsupported_port_access", false);
        port_fault(0xfff41500, 0xe1c020f0, true, 8, values[i],
                   "unsupported_port_access", false);
    }
}

static void port_synthetic_inputs(void)
{
    const char *names[] = {"zero-ports-v1", "one-ports-v1"};
    const uint32_t values[] = {0, UINT32_MAX};

    for (int fixture = 0; fixture < 2; fixture++) {
        g_autofree char *run = new_fixture_scenario("bootstrap-input-exploration",
                                                   names[fixture]);
        QTestState *q = start(run, false);
        for (int port = 0; port < 16; port++) {
            g_assert_cmphex(qtest_readl(q, 0xfff41014 + port * 0x40), ==, values[fixture]);
            QDict *event = port_record(run);
            g_assert_true(qdict_get_bool(event, "success"));
            g_assert_cmpstr(qdict_get_str(event, "device"), ==, "xt4-static-port-input-v1");
            g_assert_nonnull(strstr(qdict_get_str(event, "semantics"), "synthetic"));
            g_assert_cmpstr(qdict_get_str(event, "scenario"), ==, names[fixture]);
            qobject_unref(event);
        }
        qtest_writel(q, 0xfff41014, 0x12345678);
        QDict *event = port_record(run);
        g_assert_false(qdict_get_bool(event, "success"));
        qobject_unref(event);
        g_assert_cmphex(qtest_readl(q, 0xfff41014), ==, values[fixture]);
        qtest_writel(q, 0xfff41018, 0x12345678);
        qtest_writel(q, 0xfff41000, 0xffff0000);
        g_assert_cmphex(qtest_readl(q, 0xfff41014), ==, values[fixture]);
        qtest_system_reset(q);
        qtest_readl(q, 0xfff41018);
        port_state(run, false, 0, 0);
        g_assert_cmphex(qtest_readl(q, 0xfff41014), ==, values[fixture]);
        qtest_quit(q);
    }
}

static QTestState *new_timer(char **run)
{
    *run = new_fixture_scenario("bootstrap-timer-exploration", "zero-ports-v1");
    QTestState *q = start(*run, false);
    /* The qtest accelerator executes no guest instructions. Enable virtual
     * timer delivery; only clock_step/clock_set advance this test clock. */
    qtest_qmp_assert_success(q, "{ 'execute': 'cont' }");
    return q;
}

static void timer_result(const char *run, bool success, const char *detail)
{
    QDict *event = port_record(run);
    g_assert_cmpstr(qdict_get_str(event, "device"), ==, "xt4-timer-exploration-v1");
    g_assert_cmpint(qdict_get_bool(event, "success"), ==, success);
    if (detail) {
        g_assert_cmpstr(qdict_get_str(event, "detail"), ==, detail);
    }
    qobject_unref(event);
}

static void timer_register_knowledge(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_timer(&run);
    qtest_readl(q, 0xfff3c310);
    timer_result(run, false, "unknown_timer_readback");
    qtest_readl(q, 0xfff3c314);
    timer_result(run, false, "unknown_timer_readback");
    qtest_readl(q, 0xfff3c300);
    timer_result(run, false, "unknown_timer_control_readback");
    qtest_writel(q, 0xfff3c300, 0x10001);
    timer_result(run, false, "unknown_timer_count");
    qtest_writel(q, 0xfff3c310, 0);
    g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 0);
    qtest_writel(q, 0xfff3c300, 0x10001);
    timer_result(run, false, "unknown_timer_terminal_count");
    qtest_writel(q, 0xfff3c310, UINT32_MAX);
    qtest_writel(q, 0xfff3c314, 123);
    g_assert_cmphex(qtest_readl(q, 0xfff3c310), ==, UINT32_MAX);
    g_assert_cmpuint(qtest_readl(q, 0xfff3c314), ==, 123);
    const uint32_t bad[] = {0, 0x30003, 0x1000100, 0x10002, 0x10003, 0xffff0000};
    for (int i = 0; i < G_N_ELEMENTS(bad); i++) {
        qtest_writel(q, 0xfff3c300, bad[i]);
        timer_result(run, false, "unsupported_timer_control");
    }
    qtest_quit(q);
}

static void timer_virtual_countdown(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_timer(&run);
    qtest_writel(q, 0xfff3c310, 2000);
    qtest_writel(q, 0xfff3c300, 0x10001);
    qtest_clock_step(q, 1);
    g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 2000);
    qtest_clock_step(q, 18);
    g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 1999);
    qtest_clock_step(q, 981);
    g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 1946);
    for (int i = 0; i < 10; i++) {
        g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 1946);
    }
    g_usleep(1000);
    g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 1946);
    qtest_writel(q, 0xfff3c300, 0x10001); /* repeated enable cannot restart */
    qtest_clock_step(q, 500);
    g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 1919);
    qtest_writel(q, 0xfff3c300, 0x10000);
    qtest_clock_step(q, 1000);
    g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 1919);
    qtest_writel(q, 0xfff3c300, 0x10001);
    qtest_clock_step(q, 1000);
    g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 1865);
    qtest_writel(q, 0xfff3c310, 123);
    qtest_writel(q, 0xfff3c314, 7); /* adjacent word does not reload */
    qtest_writel(q, 0xfff3c310, 0);
    timer_result(run, false, "unknown_timer_terminal_count");
    g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 123);
    qtest_clock_step(q, 1000);
    g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 69);
    qtest_quit(q);
}

static void timer_isolation_and_reset(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_timer(&run);
    for (int ch = 0; ch < 8; ch++) {
        qtest_writel(q, 0xfff3c310 + ch * 8, 100 + ch);
        qtest_writel(q, 0xfff3c314 + ch * 8, 50 + ch);
    }
    qtest_writel(q, 0xfff3c300, 0x800080);
    qtest_clock_step(q, 1000);
    for (int ch = 0; ch < 8; ch++) {
        g_assert_cmpuint(qtest_readl(q, 0xfff3c310 + ch * 8), ==, ch == 7 ? 53 : 100 + ch);
        g_assert_cmpuint(qtest_readl(q, 0xfff3c314 + ch * 8), ==, 50 + ch);
    }
    qtest_system_reset(q);
    for (int ch = 0; ch < 8; ch++) {
        qtest_readl(q, 0xfff3c310 + ch * 8);
        timer_result(run, false, "unknown_timer_readback");
    }
    qtest_clock_step(q, 1000000);
    g_autofree char *path = g_build_filename(run, "engine-events.jsonl", NULL);
    g_autofree char *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_null(strstr(text, "unsupported_device_transition"));
    g_assert_cmpuint(qtest_readl(q, 0xfff41014), ==, 0);
    qtest_quit(q);
}

static void timer_terminal_boundary(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_timer(&run);
    qtest_writel(q, 0xfff3c310, 2);
    qtest_writel(q, 0xfff3c300, 0x10001);
    qtest_clock_step(q, 18);
    g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 2);
    qtest_clock_step(q, 1);
    g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 1);
    qtest_clock_step(q, 18);
    g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 1);
    qtest_clock_step(q, 1);
    QDict *stop = stop_record(run);
    g_assert_cmpstr(qdict_get_str(stop, "reason"), ==, "unsupported_device_transition");
    g_assert_cmpstr(qdict_get_str(stop, "detail"), ==, "unknown_timer_terminal_count");
    g_assert_cmpint(qobject_type(qdict_get(stop, "width")), ==, QTYPE_QNULL);
    g_assert_cmpint(qobject_type(qdict_get(stop, "value")), ==, QTYPE_QNULL);
    qobject_unref(stop);
    qtest_quit(q);
}

static void timer_wide_deadline(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_timer(&run);
    qtest_writel(q, 0xfff3c310, UINT32_MAX);
    qtest_writel(q, 0xfff3c300, 0x10001);
    QDict *event = port_record(run);
    g_assert_cmpint(qdict_get_int(event, "deadline_ns"), ==, INT64_C(79536431389));
    qobject_unref(event);
    qtest_clock_set(q, INT64_C(79536431388));
    g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 1);
    qtest_clock_step(q, 1);
    QDict *stop = stop_record(run);
    g_assert_cmpstr(qdict_get_str(stop, "reason"), ==, "unsupported_device_transition");
    qobject_unref(stop);
    qtest_quit(q);

    g_free(run);
    run = NULL;
    q = new_timer(&run);
    qtest_clock_set(q, INT64_MAX - 1000);
    qtest_writel(q, 0xfff3c310, UINT32_MAX);
    qtest_writel(q, 0xfff3c300, 0x10001);
    timer_result(run, false, "unsupported_timer_deadline");
    g_assert_cmphex(qtest_readl(q, 0xfff3c310), ==, UINT32_MAX);
    qtest_quit(q);
}

static void timer_invalid_operations(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_timer(&run);
    qtest_writel(q, 0xfff3c310, 2000);
    qtest_writeb(q, 0xfff3c310, 1);
    timer_result(run, false, "unsupported_timer_access");
    qtest_writew(q, 0xfff3c310, 1);
    timer_result(run, false, "unsupported_timer_access");
    qtest_writeq(q, 0xfff3c310, UINT64_MAX);
    timer_result(run, false, "unsupported_timer_access");
    QDict *event = port_record(run);
    assert_unsigned_value(event, UINT64_MAX);
    qobject_unref(event);
    qtest_writel(q, 0xfff3c311, 1);
    timer_result(run, false, "unsupported_timer_access");
    qtest_writel(q, 0xfff3c304, 1);
    timer_result(run, false, "unsupported_timer_access");
    g_assert_cmpuint(qtest_readl(q, 0xfff3c310), ==, 2000);
    qtest_quit(q);
    device_fault("bootstrap-timer-exploration", "zero-ports-v1", "xt4-timer-exploration-v1",
                  0xfff3c310, 0xe1c020f0, true, 8, UINT64_MAX, "unsupported_timer_access", false);
    device_fault("bootstrap-timer-exploration", "zero-ports-v1", "xt4-timer-exploration-v1",
                  0xfff3c311, 0xe5801000, true, 4, 0x10001, "unsupported_timer_access", false);
}

static QTestState *new_controller(char **run, const char *scenario)
{
    *run = new_fixture_scenario("bootstrap-controller-exploration", scenario);
    QTestState *q = start(*run, false);
    qtest_qmp_assert_success(q, "{ 'execute': 'cont' }");
    return q;
}

static QDict *controller_result(const char *run, const char *model,
                               bool success, const char *detail)
{
    QDict *event = port_record(run);
    g_assert_cmpstr(qdict_get_str(event, "device"), ==, model);
    g_assert_cmpint(qdict_get_bool(event, "success"), ==, success);
    if (detail) {
        g_assert_cmpstr(qdict_get_str(event, "detail"), ==, detail);
    }
    return event;
}

#define TRANSFER_MODEL "xt4-transfer-enable-exploration-v1"
#define CLOCK_MODEL "xt4-clock-apply-exploration-v1"

static void transfer_globals(void)
{
    const char *scenarios[] = {"zero-ports-v1", "one-ports-v1"};
    for (int fixture = 0; fixture < 2; fixture++) {
        g_autofree char *run = NULL;
        QTestState *q = new_controller(&run, scenarios[fixture]);
        uint32_t seed = fixture ? UINT32_MAX : 0;
        for (int i = 0; i < 16; i++) {
            uint32_t addr = 0xfffe0000 + i * 0x1000;
            g_assert_cmphex(qtest_readl(q, addr), ==, seed);
            qtest_writel(q, addr, seed ^ 0x10000000);
            QDict *event = controller_result(run, TRANSFER_MODEL, true, NULL);
            g_assert_cmpuint(qdict_get_int(event, "bank"), ==, i);
            g_assert_cmphex(qdict_get_int(event, "old_state"), ==, seed);
            g_assert_cmphex(qdict_get_int(event, "state"), ==, seed ^ 0x10000000);
            qobject_unref(event);
            g_assert_cmphex(qtest_readl(q, addr), ==, seed ^ 0x10000000);
            if (i < 15) {
                g_assert_cmphex(qtest_readl(q, addr + 0x1000), ==, seed);
            }
            qtest_writel(q, addr, seed);
            g_assert_cmphex(qtest_readl(q, addr), ==, seed);
            qtest_writel(q, addr, seed ^ 1);
            event = controller_result(run, TRANSFER_MODEL, false, "unsupported_transfer_global_write");
            qobject_unref(event);
            g_assert_cmphex(qtest_readl(q, addr), ==, seed);
        }
        qtest_writel(q, 0xfffef000, seed ^ 0x10000000);
        qtest_system_reset(q);
        for (int i = 0; i < 16; i++) {
            g_assert_cmphex(qtest_readl(q, 0xfffe0000 + i * 0x1000), ==, seed);
        }
        qtest_quit(q);
    }
}

static void transfer_invalid(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_controller(&run, "zero-ports-v1");
    qtest_writeb(q, 0xfffe0000, 1);
    QDict *event = controller_result(run, TRANSFER_MODEL, false, "unsupported_transfer_access");
    qobject_unref(event);
    qtest_writew(q, 0xfffe0000, 1);
    event = controller_result(run, TRANSFER_MODEL, false, "unsupported_transfer_access");
    qobject_unref(event);
    qtest_writeq(q, 0xfffe0000, UINT64_MAX);
    event = controller_result(run, TRANSFER_MODEL, false, "unsupported_transfer_access");
    assert_unsigned_value(event, UINT64_MAX);
    qobject_unref(event);
    qtest_writel(q, 0xfffe0001, 0x10000000);
    event = controller_result(run, TRANSFER_MODEL, false, "unsupported_transfer_access");
    qobject_unref(event);
    for (int offset = 4; offset <= 0x30; offset += 4) {
        qtest_readl(q, 0xfffe0000 + offset);
        event = controller_result(run, TRANSFER_MODEL, false, "unsupported_transfer_access");
        g_assert_cmpint(qobject_type(qdict_get(event, "value")), ==, QTYPE_QNULL);
        qobject_unref(event);
    }
    g_assert_cmpuint(qtest_readl(q, 0xfffe0000), ==, 0);
    qtest_quit(q);
    device_fault("bootstrap-controller-exploration", "zero-ports-v1", TRANSFER_MODEL,
                 0xfffe0000, 0xe1c020f0, true, 8, UINT64_MAX, "unsupported_transfer_access", false);
    device_fault("bootstrap-controller-exploration", "zero-ports-v1", TRANSFER_MODEL,
                 0xfffe0001, 0xe5801000, true, 4, 0x10000000, "unsupported_transfer_access", false);
}

static unsigned clock_transitions(const char *run, int64_t *when)
{
    g_autofree char *path = g_build_filename(run, "engine-events.jsonl", NULL);
    g_autofree char *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    unsigned count = 0;
    for (int i = 0; lines[i] && *lines[i]; i++) {
        QDict *event = qobject_to(QDict, qobject_from_json(lines[i], NULL));
        if (!g_strcmp0(qdict_get_try_str(event, "event"), "device-transition") &&
            !g_strcmp0(qdict_get_try_str(event, "device"), CLOCK_MODEL)) {
            count++;
            g_assert_true(qdict_get_bool(event, "success"));
            g_assert_cmpint(qobject_type(qdict_get(event, "width")), ==, QTYPE_QNULL);
            g_assert_cmpint(qobject_type(qdict_get(event, "value")), ==, QTYPE_QNULL);
            if (when) {
                *when = qdict_get_int(event, "virtual_ns");
            }
        }
        qobject_unref(event);
    }
    return count;
}

static void clock_apply_lifecycle(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_controller(&run, "zero-ports-v1");
    qtest_readl(q, 0xff701080);
    QDict *event = controller_result(run, CLOCK_MODEL, false, "unknown_clock_command_readback");
    qobject_unref(event);
    qtest_readl(q, 0xff701040);
    event = controller_result(run, CLOCK_MODEL, false, "unknown_clock_selection_readback");
    qobject_unref(event);
    qtest_writel(q, 0xff701080, 2);
    event = controller_result(run, CLOCK_MODEL, false, "unstaged_clock_apply");
    qobject_unref(event);
    for (int i = 0; i < 2; i++) {
        int64_t now = i; /* qtest time starts at zero; each apply advances one. */
        qtest_writel(q, 0xff701040 + i * 8, i ? 0x107 : 0x101);
        qtest_writel(q, 0xff701080, 2);
        g_assert_cmpuint(qtest_readl(q, 0xff701080), ==, 2);
        event = controller_result(run, CLOCK_MODEL, true, NULL);
        g_assert_true(qdict_get_bool(event, "busy"));
        g_assert_cmpuint(qdict_get_int(event, "active_known"), ==, i);
        qobject_unref(event);
        g_assert_cmpuint(clock_transitions(run, NULL), ==, i);
        qtest_writel(q, 0xff701080, 2);
        event = controller_result(run, CLOCK_MODEL, false, "busy_clock_apply");
        qobject_unref(event);
        qtest_writel(q, 0xff701040, 0x101);
        event = controller_result(run, CLOCK_MODEL, false, "busy_clock_apply");
        qobject_unref(event);
        qtest_clock_step(q, 1);
        int64_t when = -1;
        g_assert_cmpuint(clock_transitions(run, &when), ==, i + 1);
        g_assert_cmpint(when, ==, now + 1);
        g_assert_cmpuint(qtest_readl(q, 0xff701080), ==, 0);
        event = controller_result(run, CLOCK_MODEL, true, NULL);
        g_assert_false(qdict_get_bool(event, "busy"));
        g_assert_cmpuint(qdict_get_int(event, "active_known"), ==, i ? 3 : 1);
        g_assert_cmphex(qdict_get_int(event, "active0"), ==, 0x101);
        if (i) {
            g_assert_cmphex(qdict_get_int(event, "active1"), ==, 0x107);
        }
        qobject_unref(event);
        qtest_writel(q, 0xff701080, 2);
        event = controller_result(run, CLOCK_MODEL, false, "unstaged_clock_apply");
        qobject_unref(event);
    }
    qtest_writel(q, 0xff701040, 0x101);
    qtest_writel(q, 0xff701080, 2);
    qtest_system_reset(q);
    qtest_clock_step(q, 100);
    g_assert_cmpuint(clock_transitions(run, NULL), ==, 2);
    qtest_readl(q, 0xff701040);
    event = controller_result(run, CLOCK_MODEL, false, "unknown_clock_selection_readback");
    g_assert_cmpuint(qdict_get_int(event, "active_known"), ==, 0);
    qobject_unref(event);
    qtest_readl(q, 0xff701080);
    event = controller_result(run, CLOCK_MODEL, false, "unknown_clock_command_readback");
    qobject_unref(event);
    qtest_quit(q);
}

static void clock_invalid(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_controller(&run, "one-ports-v1");
    const uint32_t bad[] = {0, 1, 3, 0xffffffff};
    for (int i = 0; i < G_N_ELEMENTS(bad); i++) {
        qtest_writel(q, 0xff701080, bad[i]);
        QDict *event = controller_result(run, CLOCK_MODEL, false, "unsupported_clock_command");
        qobject_unref(event);
    }
    qtest_writel(q, 0xff701040, 0x107);
    QDict *event = controller_result(run, CLOCK_MODEL, false, "unsupported_clock_selection");
    qobject_unref(event);
    qtest_readl(q, 0xff701040);
    event = controller_result(run, CLOCK_MODEL, false, "unknown_clock_selection_readback");
    qobject_unref(event);
    qtest_writeq(q, 0xff701040, UINT64_MAX);
    event = controller_result(run, CLOCK_MODEL, false, "unsupported_clock_access");
    assert_unsigned_value(event, UINT64_MAX);
    qobject_unref(event);
    qtest_writeb(q, 0xff701080, 2);
    event = controller_result(run, CLOCK_MODEL, false, "unsupported_clock_access");
    qobject_unref(event);
    qtest_writew(q, 0xff701080, 2);
    event = controller_result(run, CLOCK_MODEL, false, "unsupported_clock_access");
    qobject_unref(event);
    qtest_writel(q, 0xff701041, 0x101);
    event = controller_result(run, CLOCK_MODEL, false, "unsupported_clock_access");
    qobject_unref(event);
    qtest_readl(q, 0xff701044);
    event = controller_result(run, CLOCK_MODEL, false, "unsupported_clock_access");
    qobject_unref(event);
    qtest_clock_set(q, INT64_MAX);
    qtest_writel(q, 0xff701040, 0x101);
    qtest_writel(q, 0xff701080, 2);
    event = controller_result(run, CLOCK_MODEL, false, "unsupported_clock_deadline");
    g_assert_false(qdict_get_bool(event, "busy"));
    g_assert_false(qdict_get_bool(event, "command_known"));
    qobject_unref(event);
    qtest_quit(q);
    device_fault("bootstrap-controller-exploration", "zero-ports-v1", CLOCK_MODEL,
                 0xff701040, 0xe1c020f0, true, 8, UINT64_MAX, "unsupported_clock_access", false);
}

#define FLASH_BASE 0xfffd8000
#define FLASH_MODEL "xt4-cadence-stig-exploration-v1"
#define NOR_MODEL "mx25um51245gxdi00-boot-v1"

static QTestState *new_flash(char **run)
{
    *run = new_fixture_scenario("bootstrap-flash-exploration", "zero-ports-v1");
    QTestState *q = start(*run, false);
    qtest_qmp_assert_success(q, "{ 'execute': 'cont' }");
    return q;
}

static void flash_error(const char *run, const char *detail)
{
    QDict *e = controller_result(run, FLASH_MODEL, false, detail);
    qobject_unref(e);
}

static unsigned nor_completions(const char *run, QDict **last)
{
    g_autofree char *path = g_build_filename(run, "engine-events.jsonl", NULL);
    g_autofree char *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    unsigned count = 0;
    if (last) {
        *last = NULL;
    }
    for (int i = 0; lines[i] && *lines[i]; i++) {
        QDict *e = qobject_to(QDict, qobject_from_json(lines[i], NULL));
        if (!g_strcmp0(qdict_get_try_str(e, "event"), "device-transition") &&
            !g_strcmp0(qdict_get_try_str(e, "device"), NOR_MODEL)) {
            count++;
            if (last) {
                qobject_unref(*last);
                *last = e;
                continue;
            }
        }
        qobject_unref(e);
    }
    return count;
}

static void flash_launch(QTestState *q, uint32_t command)
{
    qtest_writel(q, FLASH_BASE + 0x90, command);
    qtest_writel(q, FLASH_BASE + 0x90, command | 1);
}

static void flash_lifecycle(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_flash(&run);
    qtest_readl(q, FLASH_BASE); flash_error(run, "unknown_flash_register");
    qtest_writel(q, FLASH_BASE + 0xb4, 0x80200020);
    flash_error(run, "unknown_flash_config");
    qtest_readl(q, FLASH_BASE + 0xa0); flash_error(run, "unavailable_flash_data");
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x38), ==, 0x10005);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0xe0), ==, 0x13edfa00);
    qtest_writel(q, FLASH_BASE, 0x800c1);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE), ==, 0x800800c1);
    qtest_writel(q, FLASH_BASE + 0x44, 0x4000);
    flash_launch(q, 0x9fa00000); /* Three-byte JEDEC ID, read length N-1. */
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x90), ==, 0x9fa00002);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE) >> 31, ==, 0);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x40), ==, 0);
    qtest_clock_step(q, 63);
    g_assert_cmpuint(nor_completions(run, NULL), ==, 0);
    qtest_readl(q, FLASH_BASE + 0xa0); flash_error(run, "unavailable_flash_data");
    qtest_writel(q, FLASH_BASE + 0x94, 0); flash_error(run, "busy_flash_controller");
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x90), ==, 0x9fa00000);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0xa0), ==, 0x3a80c2);
    qtest_readl(q, FLASH_BASE + 0xa4); flash_error(run, "unavailable_flash_data");
    g_assert_cmphex(qtest_readl(q, FLASH_BASE) >> 31, ==, 1);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x40), ==, 0x4000);
    qtest_writel(q, FLASH_BASE + 0x44, 0);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x40), ==, 0x4000);
    qtest_writel(q, FLASH_BASE + 0x40, 0x4000);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x40), ==, 0);
    QDict *e = NULL;
    g_assert_cmpuint(nor_completions(run, &e), ==, 1);
    g_assert_cmpint(qdict_get_int(e, "virtual_ns"), ==, 64);
    g_assert_cmpint(qdict_get_int(e, "rx_len"), ==, 3);
    qobject_unref(e);
    flash_launch(q, 0x06000000);
    qtest_readl(q, FLASH_BASE + 0xa0); flash_error(run, "unavailable_flash_data");
    qtest_clock_step(q, 64);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x40), ==, 0); /* IRQ disabled. */
    qtest_quit(q);
}

static void flash_disabled_execute(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_flash(&run);
    qtest_writel(q, FLASH_BASE, 0x800c0);
    qtest_writel(q, FLASH_BASE + 0x90, 0x06000001);
    flash_error(run, "disabled_flash_controller");
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x90), ==, 0);
    qtest_clock_step(q, 64);
    g_assert_cmpuint(nor_completions(run, NULL), ==, 0);
    qtest_writel(q, FLASH_BASE, 0x800c1);
    flash_launch(q, 0x9fa00000);
    qtest_clock_step(q, 64);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0xa0), ==, 0x3a80c2);
    qtest_writel(q, FLASH_BASE, 0x800c0);
    qtest_writel(q, FLASH_BASE + 0x90, 0x06000001);
    flash_error(run, "disabled_flash_controller");
    /* Rejected launch cannot replace command, invalidate completed data,
     * schedule a callback, change IRQ status, or set the chip's WEL. */
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x90), ==, 0x9fa00000);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0xa0), ==, 0x3a80c2);
    qtest_clock_step(q, 64);
    g_assert_cmpuint(nor_completions(run, NULL), ==, 1);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x40), ==, 0);
    qtest_writel(q, FLASH_BASE, 0x800c1);
    flash_launch(q, 0x05800000); qtest_clock_step(q, 64);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0xa0), ==, 0);
    flash_launch(q, 0x06000000); qtest_clock_step(q, 64);
    flash_launch(q, 0x05800000); qtest_clock_step(q, 64);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0xa0), ==, 2);
    qtest_quit(q);
}

static void flash_mode_and_status(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_flash(&run);
    qtest_writel(q, FLASH_BASE, 0x800c1);
    qtest_writel(q, FLASH_BASE + 0x94, 0);
    qtest_writel(q, FLASH_BASE + 0xa8, 2);
    flash_launch(q, 0x720b8000); /* SPI WRCR2: 4 address bytes, 1 data byte. */
    qtest_clock_step(q, 64);
    QDict *e = NULL;
    nor_completions(run, &e);
    g_assert_true(qdict_get_bool(e, "ignored_write"));
    g_assert_false(qdict_get_bool(e, "write_enabled"));
    g_assert_cmpstr(qdict_get_str(e, "protocol"), ==, "spi");
    qobject_unref(e);
    flash_launch(q, 0x06000000); qtest_clock_step(q, 64);
    flash_launch(q, 0x05f00000); qtest_clock_step(q, 64);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0xa0), ==, 0x02020202);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0xa4), ==, 0x02020202);
    flash_launch(q, 0x720b8000);
    qtest_clock_step(q, 103);
    g_assert_cmpuint(nor_completions(run, NULL), ==, 3);
    qtest_clock_step(q, 1);
    nor_completions(run, &e);
    g_assert_false(qdict_get_bool(e, "write_enabled"));
    g_assert_cmpstr(qdict_get_str(e, "protocol"), ==, "opi-dtr");
    qobject_unref(e);
    qtest_writel(q, FLASH_BASE, 0x400800c1);
    qtest_writel(q, FLASH_BASE + 0xe0, 0x13edfaFA);
    flash_launch(q, 0x059b0200); /* OPI 05FA, addr4, dummy4, rx2. */
    qtest_clock_step(q, 64);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0xa0), ==, 0);
    qtest_quit(q);
}

static void flash_invalid(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_flash(&run);
    qtest_writel(q, FLASH_BASE, 0x800c1);
    qtest_writeb(q, FLASH_BASE + 0x94, 1); flash_error(run, "unsupported_flash_access");
    qtest_writew(q, FLASH_BASE + 0x94, 1); flash_error(run, "unsupported_flash_access");
    qtest_writel(q, FLASH_BASE + 0x95, 1); flash_error(run, "unsupported_flash_access");
    qtest_writeq(q, FLASH_BASE + 0x94, UINT64_MAX);
    QDict *e = controller_result(run, FLASH_MODEL, false, "unsupported_flash_access");
    assert_unsigned_value(e, UINT64_MAX); qobject_unref(e);
    qtest_readl(q, FLASH_BASE + 0x94); flash_error(run, "unknown_flash_register");
    qtest_writel(q, FLASH_BASE + 0xbc, 1); flash_error(run, "unsupported_flash_register");
    qtest_writel(q, FLASH_BASE, 0x800c9); flash_error(run, "unsupported_flash_config");
    g_assert_cmphex(qtest_readl(q, FLASH_BASE), ==, 0x800800c1);
    qtest_writel(q, FLASH_BASE + 0x90, 0x9f940001); flash_error(run, "unsupported_flash_command_fields");
    qtest_writel(q, FLASH_BASE + 0x90, 0x9f908001); flash_error(run, "unsupported_flash_command_fields");
    qtest_writel(q, FLASH_BASE + 0x90, 0x720b8001); flash_error(run, "unknown_flash_address");
    qtest_writel(q, FLASH_BASE + 0x94, 0);
    qtest_writel(q, FLASH_BASE + 0x90, 0x720b8001); flash_error(run, "unknown_flash_write_data");
    qtest_writel(q, FLASH_BASE + 0x90, 0xff000001); flash_error(run, "unsupported_nor_command");
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x90), ==, 0);
    qtest_writel(q, FLASH_BASE + 0x44, 1); flash_error(run, "unsupported_flash_irq_bits");
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x44), ==, 0);
    qtest_clock_step(q, 1000);
    g_assert_cmpuint(nor_completions(run, NULL), ==, 0);
    qtest_clock_set(q, INT64_MAX - 63);
    qtest_writel(q, FLASH_BASE + 0x90, 0x9fa00001); flash_error(run, "unsupported_flash_deadline");
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x90), ==, 0);
    qtest_quit(q);
    device_fault("bootstrap-flash-exploration", "zero-ports-v1", FLASH_MODEL,
                 FLASH_BASE + 0x90, 0xe1c020f0, true, 8, UINT64_MAX, "unsupported_flash_access", false);
}

static void flash_reset_pending(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_flash(&run);
    qtest_writel(q, FLASH_BASE, 0x800c1);
    flash_launch(q, 0x06000000);
    qtest_clock_step(q, 63);
    qtest_system_reset(q);
    qtest_clock_step(q, 1000);
    g_assert_cmpuint(nor_completions(run, NULL), ==, 0);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x90), ==, 0);
    qtest_writel(q, FLASH_BASE, 0x800c1);
    flash_launch(q, 0x05800000); qtest_clock_step(q, 64);
    g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0xa0), ==, 0);
    qtest_quit(q);
}

static void flash_terminal_pending(void)
{
    /* Command already pending when TCG starts. Budget wins even at the same
     * deadline, and no state effect may appear after the terminal stop. */
    const unsigned budgets[] = {16, 64};
    for (int i = 0; i < G_N_ELEMENTS(budgets); i++) {
        g_autofree char *run = new_fixture_scenario("bootstrap-flash-exploration", "zero-ports-v1");
        QTestState *q = start_budget(run, budgets[i]);
        qtest_writel(q, 0, 0xeafffffe);
        qtest_writel(q, FLASH_BASE, 0x800c1);
        flash_launch(q, 0x06000000);
        qtest_qmp_assert_success(q, "{ 'execute': 'cont' }");
        qtest_qmp_eventwait(q, "STOP");
        QDict *stop = stop_record(run);
        g_assert_cmpstr(qdict_get_str(stop, "reason"), ==, "instruction_budget");
        qobject_unref(stop);
        g_assert_cmpuint(nor_completions(run, NULL), ==, 0);
        g_assert_cmphex(qtest_readl(q, FLASH_BASE + 0x90) & 2, ==, 0);
        qtest_quit(q);
    }
}

#define PHY_BASE 0xfffd80b4
#define PHY_MODEL "xt4-cadence-phy-exploration-v1"

static QTestState *new_phy(char **run)
{
    *run = new_fixture_scenario("bootstrap-phy-exploration", "zero-ports-v1");
    QTestState *q = start(*run, false);
    qtest_qmp_assert_success(q, "{ 'execute': 'cont' }");
    return q;
}

static void phy_error(const char *run, const char *detail)
{
    QDict *e = controller_result(run, PHY_MODEL, false, detail);
    qobject_unref(e);
}

static unsigned phy_transitions(const char *run, int64_t *when)
{
    g_autofree char *path = g_build_filename(run, "engine-events.jsonl", NULL);
    g_autofree char *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    unsigned count = 0;
    for (int i = 0; lines[i] && *lines[i]; i++) {
        QDict *e = qobject_to(QDict, qobject_from_json(lines[i], NULL));
        if (!g_strcmp0(qdict_get_try_str(e, "event"), "device-transition") &&
            !g_strcmp0(qdict_get_try_str(e, "device"), PHY_MODEL)) {
            count++;
            g_assert_true(qdict_get_bool(e, "success"));
            g_assert_cmpint(qobject_type(qdict_get(e, "width")), ==, QTYPE_QNULL);
            g_assert_cmpint(qobject_type(qdict_get(e, "value")), ==, QTYPE_QNULL);
            if (when) {
                *when = qdict_get_int(e, "virtual_ns");
            }
        }
        qobject_unref(e);
    }
    return count;
}

static void phy_stage_master(QTestState *q)
{
    qtest_writel(q, FLASH_BASE, 0x800c9);
    qtest_writel(q, PHY_BASE, 0);
    qtest_writel(q, PHY_BASE + 4, 30);
    qtest_writel(q, PHY_BASE, 0x40000000);
}

static void phy_lifecycle(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_phy(&run);
    qtest_readl(q, PHY_BASE); phy_error(run, "unknown_phy_register");
    qtest_writel(q, PHY_BASE, 0); phy_error(run, "unknown_phy_controller_config");
    g_assert_cmphex(qtest_readl(q, PHY_BASE + 8), ==, 0);
    qtest_writel(q, FLASH_BASE, 0x800c8);
    flash_error(run, "disabled_phy_controller");
    qtest_readl(q, FLASH_BASE); flash_error(run, "unknown_flash_register");
    qtest_writel(q, FLASH_BASE, 0x800c1);
    qtest_writel(q, PHY_BASE, 0x80200020);
    g_assert_cmphex(qtest_readl(q, PHY_BASE), ==, 0x80200020);
    qtest_writel(q, PHY_BASE + 4, 30); phy_error(run, "disabled_phy");
    phy_stage_master(q);
    QDict *e = controller_result(run, PHY_MODEL, true, NULL); qobject_unref(e);
    g_assert_cmphex(qtest_readl(q, PHY_BASE + 8), ==, 0);
    qtest_writel(q, FLASH_BASE, 0x800c1);
    flash_error(run, "busy_phy_controller_config");
    g_assert_cmphex(qtest_readl(q, FLASH_BASE), ==, 0x800800c9);
    qtest_clock_step(q, 3);
    g_assert_cmphex(qtest_readl(q, PHY_BASE + 8), ==, 0);
    g_assert_cmpuint(phy_transitions(run, NULL), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, PHY_BASE + 8), ==, 0x8000);
    int64_t when;
    g_assert_cmpuint(phy_transitions(run, &when), ==, 1);
    g_assert_cmpint(when, ==, 4);
    qtest_writel(q, PHY_BASE, 0xc01f0020);
    qtest_clock_step(q, 3);
    g_assert_cmphex(qtest_readl(q, PHY_BASE + 8), ==, 0x8000);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, PHY_BASE + 8), ==, 0x8001);
    g_assert_cmpuint(phy_transitions(run, &when), ==, 2);
    g_assert_cmpint(when, ==, 8);
    qtest_writel(q, PHY_BASE, 0);
    g_assert_cmphex(qtest_readl(q, PHY_BASE + 8), ==, 0);
    qtest_writel(q, PHY_BASE, 0x40000000); phy_error(run, "unstaged_phy_master");
    qtest_clock_step(q, 100);
    g_assert_cmpuint(phy_transitions(run, NULL), ==, 2);
    qtest_quit(q);
}

static void phy_invalid_and_reset(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_phy(&run);
    qtest_writel(q, FLASH_BASE, 0x800c9);
    qtest_writeb(q, PHY_BASE, 1); phy_error(run, "unsupported_phy_access");
    qtest_writew(q, PHY_BASE, 1); phy_error(run, "unsupported_phy_access");
    qtest_writel(q, PHY_BASE + 1, 1); phy_error(run, "unsupported_phy_access");
    qtest_writeq(q, PHY_BASE, UINT64_MAX);
    QDict *e = controller_result(run, PHY_MODEL, false, "unsupported_phy_access");
    assert_unsigned_value(e, UINT64_MAX); qobject_unref(e);
    qtest_readl(q, PHY_BASE); phy_error(run, "unknown_phy_register");
    qtest_writel(q, PHY_BASE + 8, 0x8001); phy_error(run, "readonly_phy_observable");
    qtest_writel(q, PHY_BASE, 0xc01f0000); phy_error(run, "unstaged_phy_resync");
    qtest_writel(q, PHY_BASE, 0);
    qtest_writel(q, PHY_BASE + 4, 31); phy_error(run, "unsupported_phy_master");
    qtest_readl(q, PHY_BASE + 4); phy_error(run, "unknown_phy_register");
    qtest_writel(q, PHY_BASE, 0xc0200000); phy_error(run, "unsupported_phy_configuration");
    g_assert_cmphex(qtest_readl(q, PHY_BASE), ==, 0);
    qtest_writel(q, PHY_BASE + 4, 40);
    qtest_writel(q, PHY_BASE, 0x40000000);
    qtest_writel(q, PHY_BASE + 4, 50); phy_error(run, "busy_phy");
    qtest_clock_step(q, 3);
    qtest_writel(q, PHY_BASE, 0); /* Reset cancels the in-flight release. */
    qtest_clock_step(q, 100);
    g_assert_cmpuint(phy_transitions(run, NULL), ==, 0);
    g_assert_cmphex(qtest_readl(q, PHY_BASE + 8), ==, 0);
    qtest_writel(q, PHY_BASE + 4, 50);
    qtest_writel(q, PHY_BASE, 0x40000000);
    qtest_clock_step(q, 4);
    qtest_writel(q, PHY_BASE, 0xc01f007f);
    qtest_system_reset(q);
    qtest_clock_step(q, 100);
    g_assert_cmpuint(phy_transitions(run, NULL), ==, 1);
    qtest_readl(q, PHY_BASE); phy_error(run, "unknown_phy_register");
    qtest_readl(q, PHY_BASE + 4); phy_error(run, "unknown_phy_register");
    g_assert_cmphex(qtest_readl(q, PHY_BASE + 8), ==, 0);
    qtest_quit(q);
    device_fault("bootstrap-phy-exploration", "zero-ports-v1", PHY_MODEL,
                 PHY_BASE + 4, 0xe1c020f0, true, 8, UINT64_MAX, "unsupported_phy_access", false);
}

static void phy_overflow(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_phy(&run);
    qtest_writel(q, FLASH_BASE, 0x800c9);
    qtest_writel(q, PHY_BASE, 0);
    qtest_writel(q, PHY_BASE + 4, 30);
    qtest_clock_set(q, INT64_MAX - 3);
    qtest_writel(q, PHY_BASE, 0x40000000); phy_error(run, "unsupported_phy_deadline");
    g_assert_cmphex(qtest_readl(q, PHY_BASE), ==, 0);
    g_assert_cmphex(qtest_readl(q, PHY_BASE + 8), ==, 0);
    g_assert_cmpuint(phy_transitions(run, NULL), ==, 0);
    qtest_quit(q);
}

static void phy_budget_pending(void)
{
    const unsigned budgets[] = {2, 4};
    for (int i = 0; i < G_N_ELEMENTS(budgets); i++) {
        g_autofree char *run = new_fixture_scenario("bootstrap-phy-exploration", "zero-ports-v1");
        QTestState *q = start_budget(run, budgets[i]);
        qtest_writel(q, 0, 0xeafffffe);
        phy_stage_master(q);
        qtest_qmp_assert_success(q, "{ 'execute': 'cont' }");
        qtest_qmp_eventwait(q, "STOP");
        QDict *stop = stop_record(run);
        g_assert_cmpstr(qdict_get_str(stop, "reason"), ==, "instruction_budget");
        qobject_unref(stop);
        g_assert_cmpuint(phy_transitions(run, NULL), ==, 0);
        g_assert_cmphex(qtest_readl(q, PHY_BASE + 8), ==, 0);
        qtest_quit(q);
    }
}


#define CAL_ROUTE UINT64_C(0xfff4a500)
#define CAL_DMA UINT64_C(0xfffe1000)
#define CAL_IND UINT64_C(0xfffd8060)
#define CAL_DEST 0x1a000
#define CAL_SOURCE 0xf002a080
#define ROUTE_MODEL "xt4-request-route-exploration-v1"
#define DMA_MODEL "xt4-dma-channel1-exploration-v1"
#define IND_MODEL "xt4-indirect-read-exploration-v1"

static QTestState *new_calibration(char **run)
{
    *run = new_fixture_scenario("bootstrap-calibration-exploration", "zero-ports-v1");
    QTestState *q = start(*run, false);
    qtest_qmp_assert_success(q, "{ 'execute': 'cont' }");
    return q;
}

static void cal_error(const char *run, const char *model, const char *detail)
{
    QDict *e = controller_result(run, model, false, detail);
    qobject_unref(e);
}

static void cal_controller(QTestState *q)
{
    qtest_writel(q, FLASH_BASE, 0x800c1);
    flash_launch(q, 0x06000000); qtest_clock_step(q, 64);
    qtest_writel(q, FLASH_BASE + 0x94, 0);
    qtest_writel(q, FLASH_BASE + 0xa8, 2);
    flash_launch(q, 0x720b8000); qtest_clock_step(q, 104);
    qtest_writel(q, FLASH_BASE, 0xc10080c9);
    qtest_writel(q, FLASH_BASE + 4, 0x140333ee);
    qtest_writel(q, FLASH_BASE + 0x10, 1);
    qtest_writel(q, FLASH_BASE + 0x14, 0x101003);
    qtest_writel(q, FLASH_BASE + 0x1c, CAL_SOURCE);
    qtest_writel(q, FLASH_BASE + 0x20, 0x707);
    qtest_writel(q, FLASH_BASE + 0x80, 0xf);
    qtest_writel(q, FLASH_BASE + 0xe0, 0x11edfa00);
    qtest_writel(q, FLASH_BASE + 0xf0, 0x10);
    qtest_writel(q, PHY_BASE, 0);
    qtest_writel(q, PHY_BASE + 4, 30);
    qtest_writel(q, PHY_BASE, 0x40000000); qtest_clock_step(q, 4);
    qtest_writel(q, PHY_BASE, 0xc01f0000); qtest_clock_step(q, 4);
}

static void cal_descriptor(QTestState *q)
{
    qtest_writel(q, CAL_ROUTE, 0x01002a2c);
    qtest_writel(q, CAL_DMA, 0x10000000);
    qtest_writel(q, CAL_DMA + 0x28, 0x00201011);
    qtest_writel(q, CAL_DMA + 0x30, 0);
    qtest_writel(q, CAL_DMA + 0x10, 0x7f);
    qtest_writel(q, CAL_DMA + 0x14, CAL_SOURCE);
    qtest_writel(q, CAL_DMA + 0x18, CAL_DEST);
    qtest_writel(q, CAL_DMA + 0x1c, 0x30f04);
    qtest_writel(q, CAL_DMA + 0x20, 0x30f00);
    qtest_writel(q, CAL_DMA + 0x24, 0);
    qtest_writel(q, CAL_DMA + 0x28, 0x10201011);
    qtest_writel(q, CAL_IND + 4, 0);
    qtest_writel(q, CAL_IND + 8, CAL_SOURCE);
    qtest_writel(q, CAL_IND + 12, 128);
}

static unsigned cal_completions(const char *run)
{
    g_autofree char *path = g_build_filename(run, "engine-events.jsonl", NULL);
    g_autofree char *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    unsigned count = 0;
    for (int i = 0; lines[i] && *lines[i]; i++) {
        QDict *e = qobject_to(QDict, qobject_from_json(lines[i], NULL));
        if (!g_strcmp0(qdict_get_try_str(e, "event"), "device-transition") &&
            !g_strcmp0(qdict_get_try_str(e, "device"), IND_MODEL) &&
            qdict_get_bool(e, "success")) {
            g_assert_cmpint(qobject_type(qdict_get(e, "width")), ==, QTYPE_QNULL);
            g_assert_cmpint(qobject_type(qdict_get(e, "value")), ==, QTYPE_QNULL);
            g_assert_cmpint(qdict_get_int(e, "source_length"), ==, 128);
            g_assert_nonnull(qdict_get_try_str(e, "source_sha256"));
            g_assert_cmpint(qdict_get_int(e, "virtual_ns") - qdict_get_int(e, "started_ns"), ==, 64);
            count++;
        }
        qobject_unref(e);
    }
    return count;
}

static void cal_sentinel(QTestState *q)
{
    uint8_t value[128];
    memset(value, 0xa5, sizeof(value));
    qtest_memwrite(q, CAL_DEST, value, sizeof(value));
}

static void cal_unchanged(QTestState *q)
{
    uint8_t value[128], expected[128];
    memset(expected, 0xa5, sizeof(expected));
    qtest_memread(q, CAL_DEST, value, sizeof(value));
    g_assert_cmpmem(value, sizeof(value), expected, sizeof(expected));
}

static void calibration_lifecycle(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_calibration(&run);
    g_assert_cmphex(qtest_readl(q, CAL_DMA + 0x30), ==, 0);
    g_assert_cmphex(qtest_readl(q, CAL_IND), ==, 0);
    qtest_readl(q, CAL_ROUTE); cal_error(run, ROUTE_MODEL, "unknown_request_route");
    cal_controller(q); cal_descriptor(q); cal_sentinel(q);
    g_assert_cmphex(qtest_readl(q, CAL_DMA + 0x30), ==, 0);
    qtest_writel(q, CAL_IND, 1);
    g_assert_cmphex(qtest_readl(q, CAL_IND), ==, 1);
    g_assert_cmphex(qtest_readl(q, CAL_DMA + 0x30), ==, 0x10000);
    cal_unchanged(q); qtest_clock_step(q, 63); cal_unchanged(q);
    g_assert_cmpuint(cal_completions(run), ==, 0);
    qtest_writel(q, CAL_ROUTE, 0x01002c2c); cal_error(run, ROUTE_MODEL, "busy_calibration_transfer");
    qtest_writel(q, CAL_DMA + 0x18, 0); cal_error(run, DMA_MODEL, "busy_calibration_transfer");
    qtest_writel(q, CAL_IND, 0x20); cal_error(run, IND_MODEL, "busy_calibration_transfer");
    qtest_writel(q, FLASH_BASE, 0x800c1); flash_error(run, "busy_calibration_transfer");
    qtest_writel(q, FLASH_BASE + 0x90, 0x06000001); flash_error(run, "busy_calibration_transfer");
    qtest_clock_step(q, 1);
    g_assert_cmpuint(cal_completions(run), ==, 1);
    g_assert_cmphex(qtest_readl(q, CAL_DMA + 0x30), ==, 0);
    g_assert_cmphex(qtest_readl(q, CAL_DMA + 0x14), ==, CAL_SOURCE);
    g_assert_cmphex(qtest_readl(q, CAL_DMA + 0x18), ==, CAL_DEST);
    g_assert_cmphex(qtest_readl(q, CAL_IND), ==, 0x20);
    g_autofree char *media = g_build_filename(run, "inputs", "calibration.bin", NULL);
    g_autofree char *expected = NULL;
    size_t length;
    g_assert_true(g_file_get_contents(media, &expected, &length, NULL));
    uint8_t value[128]; qtest_memread(q, CAL_DEST, value, sizeof(value));
    g_assert_cmpuint(length, ==, sizeof(value));
    g_assert_cmpmem(value, sizeof(value), expected, length);
    qtest_writel(q, CAL_IND, 1); cal_error(run, IND_MODEL, "unacknowledged_indirect_done");
    qtest_writel(q, CAL_IND, 0x20);
    g_assert_cmphex(qtest_readl(q, CAL_IND), ==, 0);
    cal_sentinel(q); qtest_writel(q, CAL_IND, 1); qtest_clock_step(q, 64);
    g_assert_cmpuint(cal_completions(run), ==, 2);
    qtest_memread(q, CAL_DEST, value, sizeof(value));
    g_assert_cmpmem(value, sizeof(value), expected, length);
    qtest_writel(q, CAL_IND, 0x20); qtest_writel(q, CAL_ROUTE, 0x01002c2c);
    g_assert_cmphex(qtest_readl(q, CAL_ROUTE), ==, 0x01002c2c);
    qtest_quit(q);
}

static void calibration_invalid(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_calibration(&run);
    const uint64_t addresses[] = {CAL_ROUTE, CAL_DMA + 0x10, CAL_IND};
    const char *models[] = {ROUTE_MODEL, DMA_MODEL, IND_MODEL};
    for (unsigned i = 0; i < 3; i++) {
        qtest_writeb(q, addresses[i], 1); cal_error(run, models[i], "unsupported_transfer_path_access");
        qtest_writew(q, addresses[i], 1); cal_error(run, models[i], "unsupported_transfer_path_access");
        qtest_writel(q, addresses[i] + 1, 1); cal_error(run, models[i], "unsupported_transfer_path_access");
        qtest_writeq(q, addresses[i], UINT64_MAX);
        QDict *e = controller_result(run, models[i], false, "unsupported_transfer_path_access");
        assert_unsigned_value(e, UINT64_MAX); qobject_unref(e);
    }
    qtest_readl(q, CAL_DMA + 0x2c); cal_error(run, DMA_MODEL, "unsupported_transfer_path_access");
    qtest_readl(q, CAL_DMA + 0x10); cal_error(run, DMA_MODEL, "unknown_dma_descriptor");
    qtest_readl(q, CAL_IND + 8); cal_error(run, IND_MODEL, "unknown_indirect_parameters");
    qtest_writel(q, CAL_ROUTE, 0); cal_error(run, ROUTE_MODEL, "unsupported_request_route");
    qtest_writel(q, CAL_DMA + 0x28, 1); cal_error(run, DMA_MODEL, "unsupported_dma_control");
    qtest_writel(q, CAL_DMA + 0x30, 0x10000); cal_error(run, DMA_MODEL, "unsupported_dma_status_ack");
    const uint32_t commands[] = {0, 2, 0x21, 0xffffffff};
    for (unsigned i = 0; i < G_N_ELEMENTS(commands); i++) {
        qtest_writel(q, CAL_IND, commands[i]); cal_error(run, IND_MODEL, "unsupported_indirect_control");
    }
    cal_controller(q); cal_descriptor(q); cal_sentinel(q);
    const uint64_t regs[] = {CAL_DMA + 0x10, CAL_DMA + 0x14, CAL_DMA + 0x18,
                            CAL_DMA + 0x1c, CAL_DMA + 0x20, CAL_DMA + 0x24,
                            CAL_IND + 4, CAL_IND + 8, CAL_IND + 12,
                            FLASH_BASE + 0x1c, FLASH_BASE + 0x20, FLASH_BASE + 0x80};
    const uint32_t values[] = {0x7e, CAL_SOURCE+2, 0xffffffc0, 0x30f00, 0x30f04, 1,
                               1, CAL_SOURCE+2, 16, CAL_SOURCE+2, 0x708, 0};
    const uint32_t restore[] = {0x7f, CAL_SOURCE, CAL_DEST, 0x30f04, 0x30f00, 0,
                                0, CAL_SOURCE, 128, CAL_SOURCE, 0x707, 0xf};
    for (unsigned i = 0; i < G_N_ELEMENTS(regs); i++) {
        qtest_writel(q, regs[i], values[i]); qtest_writel(q, CAL_IND, 1);
        QDict *e = controller_result(run, IND_MODEL, false, NULL);
        g_assert_false(qdict_get_bool(e, "busy")); qobject_unref(e);
        g_assert_cmphex(qtest_readl(q, CAL_IND), ==, 0);
        g_assert_cmphex(qtest_readl(q, CAL_DMA + 0x30), ==, 0);
        qtest_clock_step(q, 64); cal_unchanged(q);
        qtest_writel(q, regs[i], restore[i]);
    }
    g_assert_cmpuint(cal_completions(run), ==, 0);
    qtest_quit(q);
}

static void calibration_reset_lost_and_overflow(void)
{
    for (unsigned mode = 0; mode < 4; mode++) {
        g_autofree char *run = NULL;
        QTestState *q = new_calibration(&run);
        cal_controller(q); cal_descriptor(q); cal_sentinel(q);
        if (mode == 3) {
            qtest_clock_set(q, INT64_MAX - 32);
            qtest_writel(q, CAL_IND, 1); cal_error(run, IND_MODEL, "unsupported_calibration_deadline");
        } else {
            qtest_writel(q, CAL_IND, 1);
            if (mode == 0) {
                qtest_system_reset(q);
            } else if (mode == 1) {
                qtest_writel(q, PHY_BASE, 0);
            } else {
                qtest_writel(q, CAL_DMA, 0);
            }
            qtest_clock_step(q, 64);
        }
        g_assert_cmpuint(cal_completions(run), ==, 0);
        if (mode == 0) {
            g_assert_cmphex(qtest_readl(q, CAL_IND), ==, 0);
            g_assert_cmphex(qtest_readl(q, CAL_DMA + 0x30), ==, 0);
        } else {
            cal_unchanged(q);
            if (mode != 3) {
                QDict *stop = stop_record(run);
                g_assert_cmpstr(qdict_get_str(stop, "reason"), ==, "unsupported_device_transition");
                g_assert_cmpint(qobject_type(qdict_get(stop, "width")), ==, QTYPE_QNULL);
                qobject_unref(stop);
            }
        }
        qtest_quit(q);
    }
}

static void calibration_missing_and_coverage(void)
{
    g_autofree char *run = NULL;
    QTestState *q = new_calibration(&run);
    cal_sentinel(q);
    qtest_writel(q, CAL_IND, 1);
    cal_error(run, IND_MODEL, "inactive_request_route");
    qtest_writel(q, CAL_ROUTE, 0x01002a2c);
    qtest_writel(q, CAL_IND, 1);
    cal_error(run, IND_MODEL, "disabled_transfer_global");
    qtest_writel(q, CAL_DMA, 0x10000000);
    qtest_writel(q, CAL_IND, 1);
    cal_error(run, IND_MODEL, "unknown_dma_descriptor");
    cal_controller(q);
    const uint64_t regs[] = {0x28, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24};
    const uint32_t values[] = {0x10201011, 127, CAL_SOURCE, CAL_DEST, 0x30f04, 0x30f00, 0};
    for (unsigned i = 0; i < G_N_ELEMENTS(regs); i++) {
        qtest_writel(q, CAL_DMA + regs[i], values[i]);
        qtest_writel(q, CAL_IND, 1);
        cal_error(run, IND_MODEL, i + 1 == G_N_ELEMENTS(regs) ?
                  "unknown_indirect_parameters" : "unknown_dma_descriptor");
        cal_unchanged(q);
    }
    cal_descriptor(q);
    /* All three source fields agree: these reach coverage admission rather
     * than being rejected merely for a descriptor/trigger mismatch. */
    const uint32_t sources[] = {CAL_SOURCE + 2, CAL_SOURCE - 2, CAL_SOURCE + 1,
                                0xf402a080, 0x2a080, 0xffffff80};
    for (unsigned i = 0; i < G_N_ELEMENTS(sources); i++) {
        qtest_writel(q, CAL_DMA + 0x14, sources[i]);
        qtest_writel(q, CAL_IND + 8, sources[i]);
        qtest_writel(q, FLASH_BASE + 0x1c, sources[i]);
        qtest_writel(q, CAL_IND, 1);
        QDict *e = controller_result(run, IND_MODEL, false, NULL);
        g_assert_false(qdict_get_bool(e, "busy"));
        qobject_unref(e);
        qtest_clock_step(q, 64);
        cal_unchanged(q);
        g_assert_cmphex(qtest_readl(q, CAL_IND), ==, 0);
        g_assert_cmphex(qtest_readl(q, CAL_DMA + 0x30), ==, 0);
    }
    qtest_writel(q, CAL_DMA + 0x14, CAL_SOURCE);
    qtest_writel(q, CAL_IND + 8, CAL_SOURCE);
    qtest_writel(q, FLASH_BASE + 0x1c, CAL_SOURCE);
    qtest_writel(q, CAL_DMA + 0x18, 0x1b041);
    qtest_writel(q, CAL_IND, 1);
    cal_error(run, IND_MODEL, "unsupported_calibration_destination");
    qtest_clock_step(q, 64);
    cal_unchanged(q);
    g_assert_cmpuint(cal_completions(run), ==, 0);
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
    qtest_add_func("/xt4/ports/unsigned-fault-values", port_unsigned_fault_values);
    qtest_add_func("/xt4/ports/synthetic-inputs-reset", port_synthetic_inputs);
    qtest_add_func("/xt4/timer/knowledge", timer_register_knowledge);
    qtest_add_func("/xt4/timer/time", timer_virtual_countdown);
    qtest_add_func("/xt4/timer/reset", timer_isolation_and_reset);
    qtest_add_func("/xt4/timer/terminal", timer_terminal_boundary);
    qtest_add_func("/xt4/timer/wide-deadline", timer_wide_deadline);
    qtest_add_func("/xt4/timer/invalid", timer_invalid_operations);
    qtest_add_func("/xt4/budget/exact", budget_exact);
    qtest_add_func("/xt4/budget/faults", budget_strict_faults);
    qtest_add_func("/xt4/budget/pause-reset-idle-warp", budget_pause_reset_idle_warp);
    qtest_add_func("/xt4/budget/timer-order", budget_timer_order);
    qtest_add_func("/xt4/calibration/lifecycle", calibration_lifecycle);
    qtest_add_func("/xt4/calibration/invalid", calibration_invalid);
    qtest_add_func("/xt4/calibration/reset-lost-overflow", calibration_reset_lost_and_overflow);
    qtest_add_func("/xt4/calibration/missing-and-coverage", calibration_missing_and_coverage);
    qtest_add_func("/xt4/phy/lifecycle", phy_lifecycle);
    qtest_add_func("/xt4/phy/invalid-reset", phy_invalid_and_reset);
    qtest_add_func("/xt4/phy/overflow", phy_overflow);
    qtest_add_func("/xt4/phy/budget-pending", phy_budget_pending);
    qtest_add_func("/xt4/flash/disabled-execute", flash_disabled_execute);
    qtest_add_func("/xt4/flash/lifecycle", flash_lifecycle);
    qtest_add_func("/xt4/flash/mode-status", flash_mode_and_status);
    qtest_add_func("/xt4/flash/invalid", flash_invalid);
    qtest_add_func("/xt4/flash/reset-pending", flash_reset_pending);
    qtest_add_func("/xt4/flash/terminal-pending", flash_terminal_pending);
    qtest_add_func("/xt4/controller/transfer-globals", transfer_globals);
    qtest_add_func("/xt4/controller/transfer-invalid", transfer_invalid);
    qtest_add_func("/xt4/controller/clock-lifecycle", clock_apply_lifecycle);
    qtest_add_func("/xt4/controller/clock-invalid", clock_invalid);
    return g_test_run();
}
