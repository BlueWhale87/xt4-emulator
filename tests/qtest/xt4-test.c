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
    return g_test_run();
}
