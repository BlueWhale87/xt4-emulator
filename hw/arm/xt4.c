/* SPDX-License-Identifier: GPL-2.0-or-later
 * Offline X-T4 module-entry diagnostics. No physical device backends.
 */
#include "qemu/osdep.h"
#include "hw/arm/xt4.h"
#include "hw/arm/machines-qom.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qjson.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"
#include "system/reset.h"
#include "system/address-spaces.h"
#include "system/block-backend-global-state.h"
#include "semihosting/semihost.h"
#include "target/arm/internals.h"

typedef struct XT4RegionDefinition {
    const char *name;
    uint32_t base, size;
    size_t image_size;
    const char *hash;
} XT4RegionDefinition;

struct XT4ProfileDefinition {
    const char *name;
    uint32_t entry, sp;
    const XT4RegionDefinition *regions;
    size_t region_count;
    bool port_config, synthetic_inputs;
};

static const XT4RegionDefinition module_regions[] = {
    { "primary", 0x01021000, 0x714000, 7420408,
      "0b5b0fb0ee5fe6b109dae9964b5cddfe18edab46b4a0c195f9da194448906eeb" },
    { "secondary", 0x01e0f000, 0x900000, 9437184,
      "e1ef8b30903d3078fde103cdd134fca7bfc4337d67bcaf55c3080ffd5fd93b13" },
    { "wireless", 0x03979000, 0x9c4000, 10240000,
      "6c8d7b8b0a37df8c2b636c504c07a80318243b313d5bf435e2c39ef3f56e9f80" },
    { "primary-tail", 0x01735000, 0x6da000, 0, NULL },
    { "secondary-tail", 0x0270f000, 0x126a000, 0, NULL },
    { "synthetic-stack", 0x7fff0000, 0x10000, 0, NULL },
};

static const XT4RegionDefinition bootstrap_regions[] = {
    { "bootstrap", 0, 0x1b0c0, 0xc880,
      "bf2d1d4ca9d1d60e5125a964988a3f1852e177c3176d35dc59dcc63afe41f6b1" },
};

static const XT4ProfileDefinition profiles[] = {
    { "module-entry-diagnostic", 0x012910c4, 0x80000000,
      module_regions, G_N_ELEMENTS(module_regions), false, false },
    { "bootstrap-entry-diagnostic", 0, 0,
      bootstrap_regions, G_N_ELEMENTS(bootstrap_regions), false, false },
    { "bootstrap-port-config-diagnostic", 0, 0,
      bootstrap_regions, G_N_ELEMENTS(bootstrap_regions), true, false },
    { "bootstrap-input-exploration", 0, 0,
      bootstrap_regions, G_N_ELEMENTS(bootstrap_regions), true, true },
};

static G_NORETURN void invalid(const char *message)
{
    error_report("X-T4: %s", message);
    exit(EXIT_FAILURE);
}

static uint8_t *read_file(int dirfd, const char *name, size_t *length)
{
    int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        st.st_size < 0 || st.st_size > 16 * 1024 * 1024) {
        if (fd >= 0) {
            close(fd);
        }
        invalid("expected bounded regular input file");
    }
    *length = st.st_size;
    uint8_t *data = g_malloc0(*length + 1);
    size_t done = 0;
    while (done < *length) {
        ssize_t n = read(fd, data + done, *length - done);
        if (n <= 0) {
            close(fd);
            invalid("incomplete input read");
        }
        done += n;
    }
    close(fd);
    return data;
}

static void emit(XT4State *s, QDict *event)
{
    g_autoptr(GString) text = qobject_to_json(QOBJECT(event));
    if (fprintf(s->events, "%s\n", text->str) < 0 || fflush(s->events)) {
        invalid("cannot record execution evidence");
    }
    qobject_unref(event);
}

void xt4_record_stop(XT4State *s, CPUState *cs, const XT4Stop *stop)
{
    if (s->stopped) {
        return;
    }
    s->stopped = true;
    QDict *event = qdict_new();
    qdict_put_str(event, "event", "stop");
    qdict_put_str(event, "reason", stop->reason);
    qdict_put_int(event, "core", cs->cpu_index);
    qdict_put_int(event, "architectural_pc", ARM_CPU(cs)->env.regs[15]);
    qdict_put_int(event, "pc", stop->has_origin_pc ? stop->origin_pc : ARM_CPU(cs)->env.regs[15]);
    qdict_put_str(event, "access", stop->access);
    if (stop->device) {
        qdict_put_str(event, "device", stop->device);
        qdict_put_str(event, "detail", stop->detail);
    }
    if (stop->has_virtual) {
        qdict_put_int(event, "virtual_address", stop->virtual_address);
    } else {
        qdict_put_null(event, "virtual_address");
    }
    if (stop->has_physical) {
        qdict_put_int(event, "physical_address", stop->physical_address);
    } else {
        qdict_put_null(event, "physical_address");
    }
    if (stop->has_value) {
        qdict_put(event, "value", qnum_from_uint(stop->value));
    } else {
        qdict_put_null(event, "value");
    }
    if (stop->width) {
        qdict_put_int(event, "width", stop->width);
    } else {
        qdict_put_null(event, "width");
    }
    qdict_put_int(event, "exception", stop->exception);
    emit(s, event);
}

static void port_event(void *opaque, const XT4PortEvent *access)
{
    XT4State *s = opaque;
    s->pending_write = !access->success && access->write;
    s->pending_value = access->value;
    s->pending_address = access->address;
    s->pending_width = access->width;
    s->pending_device = access->success ? NULL : access->model;
    s->pending_detail = access->detail;
    QDict *event = qdict_new();
    qdict_put_str(event, "event", "device-access");
    qdict_put_str(event, "device", access->model);
    qdict_put_str(event, "semantics", access->semantics);
    if (s->scenario_id) {
        qdict_put_str(event, "scenario", s->scenario_id);
    }
    qdict_put_str(event, "operation", access->write ? "write" : "read");
    qdict_put_int(event, "address", access->address);
    qdict_put_int(event, "width", access->width);
    qdict_put_bool(event, "global", access->global);
    qdict_put_int(event, "port", access->port);
    qdict_put_int(event, "selector", access->selector);
    qdict_put_bool(event, "success", access->success);
    if (access->write || access->success) {
        qdict_put(event, "value", qnum_from_uint(access->value));
    } else {
        qdict_put_null(event, "value");
    }
    qdict_put_int(event, "state", access->state);
    qdict_put_int(event, "known_mask", access->known_mask);
    if (access->detail) {
        qdict_put_str(event, "detail", access->detail);
    }
    emit(s, event);
}

static MemTxResult unknown_read(void *opaque, hwaddr addr, uint64_t *value,
                                unsigned size, MemTxAttrs attrs)
{
    XT4State *s = opaque;
    s->pending_device = s->pending_detail = NULL;
    s->pending_write = false;
    *value = 0; /* Required out parameter; MEMTX_ERROR prevents guest success. */
    return MEMTX_ERROR;
}

static MemTxResult unknown_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size, MemTxAttrs attrs)
{
    XT4State *s = opaque;
    s->pending_device = s->pending_detail = NULL;
    s->pending_write = true;
    s->pending_value = value;
    s->pending_address = addr;
    s->pending_width = size;
    return MEMTX_ERROR;
}

static const MemoryRegionOps unknown_ops = {
    .read_with_attrs = unknown_read,
    .write_with_attrs = unknown_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

static void reset(void *opaque)
{
    XT4State *s = opaque;
    ARMCPU *cpu = ARM_CPU(s->cpu);
    CPUARMState *env = &cpu->env;
    cpu_reset(CPU(cpu));
    if (s->ports) {
        device_cold_reset(DEVICE(s->ports));
    }
    for (int i = 0; i < s->profile->region_count; i++) {
        const XT4RegionDefinition *r = &s->profile->regions[i];
        uint8_t *ram = memory_region_get_ram_ptr(&s->ram[i]);
        memset(ram, 0, r->size);
        if (r->image_size) {
            memcpy(ram, s->images[i], s->image_sizes[i]);
        }
    }
    for (int i = 0; i < 13; i++) {
        env->regs[i] = 0;
    }
    cpsr_write(env, 0x1d3, UINT32_MAX, CPSRWriteRaw);
    env->cp15.sctlr_el[1] &= ~(SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_B | SCTLR_EE);
    env->regs[13] = s->profile->sp;
    env->regs[14] = 0xfffffff0;
    env->regs[15] = s->profile->entry;
    arm_rebuild_hflags(env);
    s->stopped = false;
    s->pending_write = false;
    s->pending_device = s->pending_detail = NULL;
    QDict *event = qdict_new();
    qdict_put_str(event, "event", "reset");
    qdict_put_bool(event, "camera_booted", false);
    qdict_put_str(event, "profile", s->profile->name);
    if (s->scenario_id) {
        qdict_put_str(event, "scenario", s->scenario_id);
        qdict_put_str(event, "scenario_sha256", s->scenario_sha256);
        qdict_put_str(event, "execution_class", "synthetic_exploration");
    }
    QDict *registers = qdict_new();
    for (int i = 0; i < 16; i++) {
        g_autofree char *name = g_strdup_printf("r%d", i);
        qdict_put_int(registers, name, env->regs[i]);
    }
    qdict_put_int(registers, "cpsr", cpsr_read(env));
    qdict_put_int(registers, "sctlr", env->cp15.sctlr_el[1]);
    qdict_put_int(registers, "ttbr0", env->cp15.ttbr0_el[1]);
    qdict_put_int(registers, "ttbr1", env->cp15.ttbr1_el[1]);
    qdict_put_int(registers, "vbar", env->cp15.vbar_el[1]);
    for (int i = 0; i < 8; i++) {
        g_autofree char *sp = g_strdup_printf("banked_sp_%d", i);
        g_autofree char *lr = g_strdup_printf("banked_lr_%d", i);
        g_autofree char *sr = g_strdup_printf("banked_spsr_%d", i);
        qdict_put_int(registers, sp, env->banked_r13[i]);
        qdict_put_int(registers, lr, env->banked_r14[i]);
        qdict_put_int(registers, sr, env->banked_spsr[i]);
    }
    qdict_put_obj(event, "registers", QOBJECT(registers));
    emit(s, event);
}

static void validate_scenario(QDict *p, const XT4ProfileDefinition *profile,
                              XT4State *s)
{
    QObject *obj = qdict_get(p, "scenario");
    if (!profile->synthetic_inputs) {
        if (obj && qobject_type(obj) != QTYPE_QNULL) {
            invalid("synthetic scenario not allowed on diagnostic profile");
        }
        return;
    }
    QDict *scenario = qobject_to(QDict, obj);
    const char *id = scenario ? qdict_get_try_str(scenario, "scenario_id") : NULL;
    if (!id || (strcmp(id, "zero-ports-v1") && strcmp(id, "one-ports-v1")) ||
        qdict_size(scenario) != 3) {
        invalid("explicit supported synthetic scenario required");
    }
    QList *words = qobject_to(QList, qdict_get(scenario, "port_inputs"));
    QObject *timer = qdict_get(scenario, "timer");
    if (!words || qlist_size(words) != 16 || !timer ||
        qobject_type(timer) != QTYPE_QNULL) {
        invalid("invalid synthetic scenario shape");
    }
    uint32_t expected = !strcmp(id, "zero-ports-v1") ? 0 : UINT32_MAX;
    g_autoptr(GString) canonical = g_string_new("{\"port_inputs\":[");
    const QListEntry *entry;
    int i = 0;
    QLIST_FOREACH_ENTRY(words, entry) {
        QNum *number = qobject_to(QNum, qlist_entry_obj(entry));
        uint64_t value;
        if (!number || !qnum_get_try_uint(number, &value) || value != expected) {
            invalid("scenario words differ from fixed synthetic identity");
        }
        s->port_inputs[i] = value;
        g_string_append_printf(canonical, "%s%" PRIu32, i ? "," : "", expected);
        i++;
    }
    g_string_append_printf(canonical, "],\"scenario_id\":\"%s\",\"timer\":null}", id);
    s->scenario_id = g_strdup(id);
    s->scenario_sha256 = g_compute_checksum_for_string(G_CHECKSUM_SHA256,
                                                       canonical->str, -1);
}

static const XT4ProfileDefinition *validate_profile(int dir, XT4State *s)
{
    size_t len;
    g_autofree uint8_t *text = read_file(dir, "profile.json", &len);
    g_autoptr(QDict) p = qobject_to(QDict, qobject_from_json((char *)text, NULL));
    const XT4ProfileDefinition *profile = NULL;
    for (int i = 0; p && i < G_N_ELEMENTS(profiles); i++) {
        if (!g_strcmp0(qdict_get_try_str(p, "name"), profiles[i].name)) {
            profile = &profiles[i];
            break;
        }
    }
    if (!profile || qdict_get_try_int(p, "schema_version", -1) != 1 ||
        g_strcmp0(qdict_get_try_str(p, "cpu_type"), TYPE_XT4_CPU) ||
        qdict_get_try_int(p, "entry", -1) != profile->entry) {
        invalid("unsupported or invalid profile");
    }
    QObject *models_obj = qdict_get(p, "models");
    QList *models = qobject_to(QList, models_obj);
    if ((models_obj && !models) ||
        (profile->port_config ? !models || qlist_size(models) != (profile->synthetic_inputs ? 2 : 1) :
                               models && qlist_size(models) != 0)) {
        invalid("unsupported device model set");
    }
    if (profile->port_config) {
        const char *expected[] = {XT4_PORT_CONFIG_MODEL, XT4_STATIC_PORT_INPUT_MODEL};
        const QListEntry *entry;
        int index = 0;
        QLIST_FOREACH_ENTRY(models, entry) {
            QString *name = qobject_to(QString, qlist_entry_obj(entry));
            if (!name || strcmp(qstring_get_str(name), expected[index++])) {
                invalid("unsupported device model identity");
            }
        }
    }
    validate_scenario(p, profile, s);
    QList *regions = qobject_to(QList, qdict_get(p, "regions"));
    QDict *regs = qobject_to(QDict, qdict_get(p, "registers"));
    if (!regions || qlist_size(regions) != profile->region_count ||
        !regs || qdict_size(regs) != 16) {
        invalid("invalid profile memory/register set");
    }
    for (int i = 0; i < 13; i++) {
        g_autofree char *name = g_strdup_printf("r%d", i);
        if (qdict_get_try_int(regs, name, -1) != 0) {
            invalid("unsupported starting register state");
        }
    }
    if (qdict_get_try_int(regs, "sp", -1) != profile->sp ||
        qdict_get_try_int(regs, "lr", -1) != 0xfffffff0 ||
        qdict_get_try_int(regs, "cpsr", -1) != 0x1d3) {
        invalid("unsupported starting register state");
    }
    const QListEntry *entry;
    int i = 0;
    QLIST_FOREACH_ENTRY(regions, entry) {
        QDict *r = qobject_to(QDict, qlist_entry_obj(entry));
        const XT4RegionDefinition *def = &profile->regions[i];
        g_autofree char *file = def->image_size ?
            g_strdup_printf("inputs/%s.bin", def->name) : NULL;
        if (!r || g_strcmp0(qdict_get_try_str(r, "name"), def->name) ||
            qdict_get_try_int(r, "base", -1) != def->base ||
            qdict_get_try_int(r, "size", -1) != def->size ||
            qdict_get_try_int(r, "file_size", -1) != def->image_size ||
            g_strcmp0(qdict_get_try_str(r, "file"), file) ||
            g_strcmp0(qdict_get_try_str(r, "sha256"), def->hash)) {
            invalid("unverified memory profile");
        }
        i++;
    }
    return profile;
}

static void init(MachineState *machine)
{
    XT4State *s = XT4_MACHINE(machine);
    char canonical[PATH_MAX];
    if (!s->run_dir || !realpath(s->run_dir, canonical) ||
        strcmp(canonical, s->run_dir)) {
        invalid("run-dir must be a canonical directory");
    }
    if (machine->ram_size != 0 || machine_usb(machine) ||
        machine->kernel_filename || machine->firmware || machine->initrd_filename ||
        blk_next(NULL) || semihosting_enabled(false)) {
        invalid("external boot images, drives and semihosting are disabled");
    }
    int dir = open(s->run_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir < 0) {
        invalid("cannot open run directory");
    }
    s->profile = validate_profile(dir, s);
    g_assert(s->profile->region_count <= G_N_ELEMENTS(s->ram));
    g_assert(s->profile->region_count <= G_N_ELEMENTS(s->images));
    int inputs = openat(dir, "inputs", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (inputs < 0) {
        invalid("missing regular input directory");
    }
    for (int i = 0; i < s->profile->region_count; i++) {
        const XT4RegionDefinition *def = &s->profile->regions[i];
        if (!def->image_size) {
            continue;
        }
        g_autofree char *file = g_strdup_printf("%s.bin", def->name);
        s->images[i] = read_file(inputs, file, &s->image_sizes[i]);
        g_autofree char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                               s->images[i], s->image_sizes[i]);
        if (s->image_sizes[i] != def->image_size || strcmp(hash, def->hash)) {
            invalid("decoded image fingerprint mismatch");
        }
    }
    close(inputs);
    int out = openat(dir, "engine-events.jsonl",
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (out < 0 || !(s->events = fdopen(out, "w"))) {
        invalid("event file already exists or cannot be created");
    }
    close(dir);
    memory_region_init_io(&s->unknown, OBJECT(machine), &unknown_ops,
                           s, "xt4-unsupported", UINT64_C(0x100000000));
    memory_region_add_subregion_overlap(get_system_memory(), 0, &s->unknown, -1);
    for (int i = 0; i < s->profile->region_count; i++) {
        const XT4RegionDefinition *def = &s->profile->regions[i];
        memory_region_init_ram_flags_nomigrate(&s->ram[i], OBJECT(machine),
                               def->name, def->size, 0, &error_fatal);
        memory_region_add_subregion(get_system_memory(), def->base, &s->ram[i]);
    }
    if (s->profile->port_config) {
        s->ports = XT4_PORT_CONFIG(qdev_new(TYPE_XT4_PORT_CONFIG));
        xt4_ports_set_event_callback(s->ports, port_event, s);
        if (s->profile->synthetic_inputs) {
            xt4_ports_set_inputs(s->ports, s->port_inputs);
        }
        sysbus_realize_and_unref(SYS_BUS_DEVICE(s->ports), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(s->ports), 0, 0xfff41000);
        sysbus_mmio_map(SYS_BUS_DEVICE(s->ports), 1, 0xfff41500);
    }
    s->cpu = XT4_CPU(object_new(TYPE_XT4_CPU));
    s->cpu->board = s;
    qdev_realize(DEVICE(s->cpu), NULL, &error_fatal);
    qemu_register_reset(reset, s);
}

static char *get_run_dir(Object *obj, Error **errp)
{
    return g_strdup(XT4_MACHINE(obj)->run_dir);
}

static void set_run_dir(Object *obj, const char *value, Error **errp)
{
    XT4State *s = XT4_MACHINE(obj);
    g_free(s->run_dir);
    s->run_dir = g_strdup(value);
}

static void class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char *const valid[] = { TYPE_XT4_CPU, NULL };
    mc->desc = "Fujifilm X-T4 offline diagnostic foundation (not a booted camera)";
    mc->init = init;
    mc->default_cpu_type = TYPE_XT4_CPU;
    mc->valid_cpu_types = valid;
    mc->default_ram_size = 0;
    mc->default_cpus = mc->min_cpus = mc->max_cpus = 1;
    mc->no_serial = mc->no_parallel = mc->no_floppy = mc->no_cdrom = true;
    mc->auto_create_sdcard = false;
    mc->ignore_memory_transaction_failures = false;
    object_class_property_add_str(oc, "run-dir", get_run_dir, set_run_dir);
}

static const TypeInfo xt4_types[] = {{
    .name = TYPE_XT4_MACHINE, .parent = TYPE_MACHINE,
    .instance_size = sizeof(XT4State), .class_init = class_init,
    .interfaces = arm_machine_interfaces,
}};
DEFINE_TYPES(xt4_types)
