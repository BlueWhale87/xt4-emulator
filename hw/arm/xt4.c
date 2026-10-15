/* SPDX-License-Identifier: GPL-2.0-or-later
 * Offline X-T4 module-entry diagnostics. No physical device backends.
 */
#include "qemu/osdep.h"
#include "hw/arm/xt4.h"
#include "hw/arm/machines-qom.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qjson.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"
#include "qobject/qbool.h"
#include "exec/icount.h"
#include "system/cpus.h"
#include "system/qtest.h"
#include "system/runstate.h"
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
    bool port_config, synthetic_inputs, timer_model, platform_model, flash_model, phy_model;
    bool calibration_model;
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
      module_regions, G_N_ELEMENTS(module_regions), false, false, false, false },
    { "bootstrap-entry-diagnostic", 0, 0,
      bootstrap_regions, G_N_ELEMENTS(bootstrap_regions), false, false, false, false },
    { "bootstrap-port-config-diagnostic", 0, 0,
      bootstrap_regions, G_N_ELEMENTS(bootstrap_regions), true, false, false, false },
    { "bootstrap-input-exploration", 0, 0,
      bootstrap_regions, G_N_ELEMENTS(bootstrap_regions), true, true, false, false },
    { "bootstrap-timer-exploration", 0, 0,
      bootstrap_regions, G_N_ELEMENTS(bootstrap_regions), true, true, true, false },
    { "bootstrap-controller-exploration", 0, 0,
      bootstrap_regions, G_N_ELEMENTS(bootstrap_regions), true, true, true, true },
    { "bootstrap-flash-exploration", 0, 0,
      bootstrap_regions, G_N_ELEMENTS(bootstrap_regions), true, true, true, true, true },
    { "bootstrap-phy-exploration", 0, 0,
      bootstrap_regions, G_N_ELEMENTS(bootstrap_regions), true, true, true, true, true, true },
    { "bootstrap-calibration-exploration", 0, 0,
      bootstrap_regions, G_N_ELEMENTS(bootstrap_regions), true, true, true, true, true, true, true },
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
    if (s->calibration) {
        xt4_calibration_transfer_cancel_pending(s->calibration);
    }
    if (s->phy) {
        xt4_phy_cancel_pending(s->phy);
    }
    if (s->flash) {
        xt4_flash_cancel_pending(s->flash);
    }
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

static void execution_stop(XT4State *s, const char *reason)
{
    XT4Stop stop = { .reason = reason, .access = "execution" };
    xt4_record_stop(s, CPU(s->cpu), &stop);
    qemu_system_debug_request();
    cpu_stop_current();
}

static void arm_execution_budget(XT4State *s, int64_t remaining)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now < 0 || remaining <= 0 || remaining > INT64_MAX - now) {
        /* An enabled, unwired architectural timer can warp idle time all the
         * way to INT64_MAX, then rearm itself at that same timestamp forever.
         * Quiesce host timer callbacks at this terminal unsupported boundary;
         * no guest-visible timer register or IRQ result is supplied. Reset is
         * required before any supported continuation. */
        ARMCPU *cpu = ARM_CPU(s->cpu);
        for (int i = 0; i < G_N_ELEMENTS(cpu->gt_timer); i++) {
            timer_del(cpu->gt_timer[i]);
        }
        execution_stop(s, "unsupported_execution_deadline");
        return;
    }
    timer_mod_ns(s->budget_timer, now + remaining);
}

static void execution_budget(void *opaque)
{
    XT4State *s = opaque;
    if (s->stopped) {
        return;
    }
    /* Timer callbacks are an I/O-safe boundary. Arbitrary translated-fault
     * hooks are not: raw accounting for those stops is sampled below. */
    int64_t used = icount_get_raw() - s->instruction_baseline;
    if (used < 0 || used > s->instruction_budget) {
        execution_stop(s, "execution_budget_invariant");
    } else if (used == s->instruction_budget) {
        execution_stop(s, "instruction_budget");
    } else {
        /* A non-external device deadline may have driven idle-time warp.
         * Virtual nanoseconds do not constitute executed instructions. */
        QDict *event = qdict_new();
        qdict_put_str(event, "event", "execution-budget-rearm");
        qdict_put_int(event, "engine_instruction_count", used);
        qdict_put_int(event, "virtual_ns", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        qdict_put_int(event, "remaining", s->instruction_budget - used);
        emit(s, event);
        arm_execution_budget(s, s->instruction_budget - used);
    }
}

static void execution_state(void *opaque, bool running, RunState state)
{
    XT4State *s = opaque;
    if (running) {
        return;
    }
    /* VM state callbacks run after vCPUs pause and TCG unwinds its count.
     * Priority1 runs before the default-priority GDB stop reply. */
    int64_t raw = icount_get_raw();
    QDict *event = qdict_new();
    qdict_put_str(event, "event", "execution-state");
    qdict_put_str(event, "state", RunState_str(state));
    qdict_put_bool(event, "running", false);
    qdict_put_int(event, "sequence", ++s->execution_sequence);
    qdict_put_int(event, "engine_instruction_count", raw - s->instruction_baseline);
    qdict_put_int(event, "raw_icount", raw);
    qdict_put_int(event, "virtual_ns", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    qdict_put_int(event, "pc", ARM_CPU(s->cpu)->env.regs[15]);
    qdict_put_int(event, "instruction_budget", s->instruction_budget);
    qdict_put_str(event, "count_semantics", "qemu-raw-icount-not-retired");
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

static void timer_event(void *opaque, const XT4TimerEvent *access)
{
    XT4State *s = opaque;
    s->pending_write = !access->transition && !access->success && access->write;
    s->pending_value = access->value;
    s->pending_address = access->address;
    s->pending_width = access->width;
    s->pending_device = access->success ? NULL : XT4_TIMER_MODEL;
    s->pending_detail = access->detail;
    QDict *event = qdict_new();
    qdict_put_str(event, "event", access->transition ? "device-transition" : "device-access");
    qdict_put_str(event, "device", XT4_TIMER_MODEL);
    qdict_put_str(event, "semantics", "synthetic pre-terminal countdown; clock/reset/enable assumptions");
    qdict_put_str(event, "scenario", s->scenario_id);
    qdict_put_str(event, "operation", access->transition ? "terminal-count" : access->write ? "write" : "read");
    qdict_put_int(event, "address", access->address);
    qdict_put_int(event, "channel", access->channel);
    qdict_put_int(event, "virtual_ns", access->virtual_ns);
    qdict_put_int(event, "deadline_ns", access->deadline_ns);
    qdict_put_bool(event, "success", access->success);
    qdict_put_bool(event, "current_known", access->current_known);
    qdict_put_bool(event, "adjacent_known", access->adjacent_known);
    qdict_put_bool(event, "enabled", access->enabled);
    qdict_put_bool(event, "terminal_unknown", access->terminal_unknown);
    qdict_put_int(event, "current", access->current);
    qdict_put_int(event, "adjacent", access->adjacent);
    if (access->transition) {
        qdict_put_null(event, "width");
        qdict_put_null(event, "value");
    } else {
        qdict_put_int(event, "width", access->width);
        if (access->write || access->success) {
            qdict_put(event, "value", qnum_from_uint(access->value));
        } else {
            qdict_put_null(event, "value");
        }
    }
    if (access->detail) {
        qdict_put_str(event, "detail", access->detail);
    }
    emit(s, event);
    if (access->transition && !s->stopped) {
        /* Precise icount can run virtual timers in the vCPU thread. Never
         * synchronously disable a clock from one of its own callbacks. */
        XT4Stop stop = {
            .reason = "unsupported_device_transition", .access = "timer",
            .physical_address = access->address, .has_physical = true,
            .device = XT4_TIMER_MODEL, .detail = access->detail,
        };
        xt4_record_stop(s, CPU(s->cpu), &stop);
        qemu_system_debug_request();
        cpu_stop_current(); /* No-op outside a vCPU; never exits the CPU loop. */
    }
}

static QDict *platform_event(XT4State *s, const char *model, const char *semantics,
                             hwaddr address, unsigned width, uint64_t value,
                             bool write, bool success, const char *detail,
                             bool transition, int64_t virtual_ns)
{
    if (!transition) {
        s->pending_write = !success && write;
        s->pending_value = value;
        s->pending_address = address;
        s->pending_width = width;
        s->pending_device = success ? NULL : model;
        s->pending_detail = detail;
    }
    QDict *event = qdict_new();
    qdict_put_str(event, "event", transition ? "device-transition" : "device-access");
    qdict_put_str(event, "device", model);
    qdict_put_str(event, "semantics", semantics);
    qdict_put_str(event, "scenario", s->scenario_id);
    qdict_put_str(event, "operation", transition ? "apply-complete" : write ? "write" : "read");
    qdict_put_int(event, "address", address);
    qdict_put_int(event, "virtual_ns", virtual_ns);
    qdict_put_bool(event, "success", success);
    if (transition) {
        qdict_put_null(event, "width");
        qdict_put_null(event, "value");
    } else {
        qdict_put_int(event, "width", width);
        if (write || success) {
            qdict_put(event, "value", qnum_from_uint(value));
        } else {
            qdict_put_null(event, "value");
        }
    }
    if (detail) {
        qdict_put_str(event, "detail", detail);
    }
    return event;
}

static void transfer_event(void *opaque, const XT4TransferEvent *access)
{
    XT4State *s = opaque;
    QDict *event = platform_event(s, XT4_TRANSFER_MODEL,
        "inferred enable bit; explicit synthetic initial words; no DMA/status/IRQ",
        access->address, access->width, access->value, access->write,
        access->success, access->detail, false, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    qdict_put_int(event, "bank", access->bank);
    qdict_put_int(event, "old_state", access->old_state);
    qdict_put_int(event, "state", access->state);
    emit(s, event);
}

static void clock_apply_event(void *opaque, const XT4ClockApplyEvent *access)
{
    XT4State *s = opaque;
    QDict *event = platform_event(s, XT4_CLOCK_APPLY_MODEL,
        "synthetic selection latch and latency; no CPU/peripheral frequency change",
        access->address, access->width, access->value, access->write,
        access->success, access->detail, access->transition, access->virtual_ns);
    qdict_put_bool(event, "busy", access->busy);
    qdict_put_bool(event, "command_known", access->command_known);
    qdict_put_int(event, "command", access->command);
    qdict_put_int(event, "deadline_ns", access->deadline_ns);
    qdict_put_int(event, "shadow_known", access->shadow_known);
    qdict_put_int(event, "active_known", access->active_known);
    qdict_put_int(event, "staged", access->staged);
    qdict_put_int(event, "shadow0", access->shadow[0]);
    qdict_put_int(event, "shadow1", access->shadow[1]);
    qdict_put_int(event, "active0", access->active[0]);
    qdict_put_int(event, "active1", access->active[1]);
    emit(s, event);
}

/* Budget completion has priority even when inserted after a flash timer
 * with the same timestamp. No persistent effect may cross a terminal stop. */
static bool flash_can_complete(void *opaque)
{
    XT4State *s = opaque;
    if (s->stopped) {
        return false;
    }
    if (s->instruction_budget &&
        icount_get_raw() - s->instruction_baseline >= s->instruction_budget) {
        execution_budget(s);
        return false;
    }
    return true;
}

static void flash_event(void *opaque, const XT4FlashEvent *access)
{
    XT4State *s = opaque;
    QDict *event = platform_event(s, access->transition ? XT4_NOR_MODEL : XT4_FLASH_MODEL,
        "named virtual NOR; synthetic reset/timing/dual-opcode adapter; no array/DMA/PHY/CPU IRQ",
        access->address, access->width, access->value, access->write,
        access->success, access->detail, access->transition, access->virtual_ns);
    if (access->transition) {
        qdict_put_str(event, "operation", "nor-complete");
    }
    qdict_put_str(event, "virtual_part", XT4_NOR_MODEL);
    qdict_put_str(event, "stig_protocol_policy", XT4_FLASH_PROTOCOL_POLICY);
    qdict_put_int(event, "transaction_latency_ns", s->flash_contract.transaction_latency_ns);
    qdict_put_int(event, "cr2_typical_settle_ns", s->flash_contract.cr2_typical_settle_ns);
    qdict_put_bool(event, "busy", access->busy);
    qdict_put_bool(event, "irq", access->irq);
    qdict_put_int(event, "status", access->status);
    qdict_put_int(event, "enable", access->enable);
    qdict_put_int(event, "command", access->command);
    qdict_put_int(event, "deadline_ns", access->deadline_ns);
    qdict_put_int(event, "opcode", access->request.opcode);
    qdict_put_int(event, "extension", access->request.extension);
    qdict_put_int(event, "rx_len", access->request.rx_len);
    qdict_put_int(event, "tx_len", access->request.tx_len);
    qdict_put_int(event, "generation", access->nor.generation);
    qdict_put_bool(event, "write_enabled", access->nor.write_enabled);
    qdict_put_bool(event, "ignored_write", access->ignored_write);
    qdict_put_str(event, "protocol", access->nor.protocol == XT4_NOR_SPI ? "spi" : "opi-dtr");
    emit(s, event);
    if (access->transition && !access->success) {
        XT4Stop stop = {
            .reason = "unsupported_device_transition", .access = "timer",
            .physical_address = access->address, .has_physical = true,
            .device = XT4_NOR_MODEL, .detail = access->detail,
        };
        xt4_record_stop(s, CPU(s->cpu), &stop);
        qemu_system_debug_request();
        cpu_stop_current();
    }
}

static void phy_event(void *opaque, const XT4PhyEvent *access)
{
    XT4State *s = opaque;
    QDict *event = platform_event(s, XT4_PHY_MODEL,
        "synthetic digital PHY locks/reset/deadlines; no electrical/data-eye/calibration result",
        access->address, access->width, access->value, access->write,
        access->success, access->detail, access->transition, access->virtual_ns);
    if (access->transition) {
        qdict_put_str(event, "operation", access->phase);
    }
    qdict_put_str(event, "phase", access->phase);
    qdict_put_bool(event, "busy", access->busy);
    qdict_put_bool(event, "controller_known", access->controller_known);
    qdict_put_bool(event, "enabled", access->enabled);
    qdict_put_bool(event, "configuration_known", access->configuration_known);
    qdict_put_bool(event, "master_known", access->master_known);
    qdict_put_bool(event, "fresh_master", access->fresh_master);
    qdict_put_int(event, "configuration", access->configuration);
    qdict_put_int(event, "master_control", access->master_control);
    qdict_put_int(event, "observable", access->observable);
    qdict_put_int(event, "epoch", access->epoch);
    qdict_put_int(event, "deadline_ns", access->deadline_ns);
    qdict_put_int(event, "master_lock_latency_ns", s->phy_contract.master_lock_latency_ns);
    qdict_put_int(event, "dll_lock_latency_ns", s->phy_contract.dll_lock_latency_ns);
    qdict_put_str(event, "reset_policy", XT4_PHY_RESET_POLICY);
    emit(s, event);
    if (access->transition && !access->success) {
        XT4Stop stop = {
            .reason = "unsupported_device_transition", .access = "timer",
            .physical_address = access->address, .has_physical = true,
            .device = XT4_PHY_MODEL, .detail = access->detail,
        };
        xt4_record_stop(s, CPU(s->cpu), &stop);
        qemu_system_debug_request();
        cpu_stop_current();
    }
}

static void calibration_event(void *opaque, const XT4CalibrationTransferEvent *a)
{
    XT4State *s = opaque;
    const char *models[] = {XT4_ROUTE_MODEL, XT4_DMA_MODEL, XT4_INDIRECT_MODEL};
    const char *model = models[a->component];
    QDict *event = platform_event(s, model,
        "bounded virtual covered transfer; ideal sampling and declared timing/status/address policies",
        a->address, a->width, a->value, a->write, a->success, a->detail,
        a->transition, a->virtual_ns);
    if (a->transition) {
        qdict_put_str(event, "operation", "transfer-complete");
    }
    qdict_put_bool(event, "route_known", a->route_known);
    qdict_put_int(event, "route", a->route);
    qdict_put_int(event, "source", a->source);
    qdict_put_int(event, "destination", a->destination);
    qdict_put_int(event, "count", a->count);
    qdict_put_int(event, "source_attr", a->source_attr);
    qdict_put_int(event, "destination_attr", a->destination_attr);
    qdict_put_int(event, "descriptor", a->descriptor);
    qdict_put_int(event, "control", a->control);
    qdict_put_bool(event, "active", a->active);
    qdict_put_bool(event, "done", a->done);
    qdict_put_bool(event, "busy", a->busy);
    qdict_put_int(event, "epoch", a->epoch);
    qdict_put_int(event, "started_ns", a->started_ns);
    qdict_put_int(event, "deadline_ns", a->deadline_ns);
    qdict_put_int(event, "phase", a->phase);
    if (a->source_sha256) {
        qdict_put_str(event, "source_sha256", a->source_sha256);
        qdict_put_int(event, "source_length", a->source_length);
    }
    emit(s, event);
    if (a->transition && !a->success) {
        XT4Stop stop = {
            .reason = "unsupported_device_transition", .access = "timer",
            .physical_address = a->address, .has_physical = true,
            .device = model, .detail = a->detail,
        };
        xt4_record_stop(s, CPU(s->cpu), &stop);
        qemu_system_debug_request();
        cpu_stop_current();
    }
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
    if (s->calibration) {
        device_cold_reset(DEVICE(s->calibration));
    }
    if (s->ports) {
        device_cold_reset(DEVICE(s->ports));
    }
    if (s->timer) {
        device_cold_reset(DEVICE(s->timer));
    }
    if (s->transfer) {
        device_cold_reset(DEVICE(s->transfer));
        device_cold_reset(DEVICE(s->clock_apply));
    }
    if (s->flash) {
        device_cold_reset(DEVICE(s->flash));
    }
    if (s->phy) {
        device_cold_reset(DEVICE(s->phy));
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
    if (s->calibration) {
        qdict_put_str(event, "media_sha256", XT4_CALIBRATION_SHA256);
        qdict_put_int(event, "cbar_base", cpu->reset_cbar);
    }
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
    if (s->instruction_budget) {
        timer_del(s->budget_timer);
        s->instruction_baseline = icount_get_raw();
        s->execution_sequence = 0;
        QDict *policy = qdict_new();
        qdict_put_str(policy, "event", "execution-policy");
        qdict_put_str(policy, "mode", "bounded-continue");
        qdict_put_int(policy, "instruction_budget", s->instruction_budget);
        qdict_put_int(policy, "raw_icount_baseline", s->instruction_baseline);
        qdict_put_str(policy, "count_semantics", "qemu-raw-icount-not-retired");
        emit(s, policy);
        arm_execution_budget(s, s->instruction_budget);
    }
}

static bool scenario_number(QDict *dict, const char *name, uint64_t expected)
{
    QNum *number = qobject_to(QNum, qdict_get(dict, name));
    uint64_t value;
    return number && qnum_get_try_uint(number, &value) && value == expected;
}

static bool scenario_true(QDict *dict, const char *name)
{
    QBool *value = qobject_to(QBool, qdict_get(dict, name));
    return value && qbool_get_bool(value);
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
        qdict_size(scenario) != (3 + profile->platform_model + profile->flash_model + profile->phy_model + profile->calibration_model)) {
        invalid("explicit supported synthetic scenario required");
    }
    QList *words = qobject_to(QList, qdict_get(scenario, "port_inputs"));
    QObject *timer = qdict_get(scenario, "timer");
    if (!words || qlist_size(words) != 16 || !timer) {
        invalid("invalid synthetic scenario shape");
    }
    const char *timer_json = "null";
    if (profile->timer_model) {
        QDict *clock = qobject_to(QDict, timer);
        if (!clock || qdict_size(clock) != 5 ||
            !scenario_number(clock, "frequency_hz", 54000000) ||
            !scenario_number(clock, "icount_shift", 0) ||
            !scenario_true(clock, "initial_halted") ||
            !scenario_true(clock, "enable_preserves_count") ||
            !scenario_true(clock, "stop_at_terminal_count")) {
            invalid("unsupported synthetic timer assumptions");
        }
        timer_json = "{\"enable_preserves_count\":true,\"frequency_hz\":54000000,"
                     "\"icount_shift\":0,\"initial_halted\":true,"
                     "\"stop_at_terminal_count\":true}";
        if (!qtest_enabled() &&
            (icount_enabled() != ICOUNT_PRECISE || icount_to_ns(1) != 1)) {
            invalid("timer exploration requires precise one-nanosecond icount");
        }
    } else if (qobject_type(timer) != QTYPE_QNULL) {
        invalid("timer assumptions not allowed on input-only profile");
    }
    uint32_t expected = !strcmp(id, "zero-ports-v1") ? 0 : UINT32_MAX;
    g_autoptr(GString) canonical = g_string_new("{");
    const QListEntry *entry;
    if (profile->flash_model) {
        QDict *flash = qobject_to(QDict, qdict_get(scenario, "flash"));
        if (!flash || qdict_size(flash) != 9 ||
            g_strcmp0(qdict_get_try_str(flash, "virtual_part"), XT4_NOR_MODEL) ||
            g_strcmp0(qdict_get_try_str(flash, "stig_protocol_policy"), XT4_FLASH_PROTOCOL_POLICY) ||
            !scenario_true(flash, "powered_settled") ||
            !scenario_number(flash, "transaction_latency_ns", 64) ||
            !scenario_number(flash, "cr2_typical_settle_ns", 40) ||
            !scenario_number(flash, "initial_command_word", 0) ||
            !scenario_number(flash, "initial_opcode_extensions", 0x13edfa00) ||
            !scenario_number(flash, "initial_write_completion", 0x10005) ||
            !scenario_number(flash, "initial_variant_word", 0)) {
            invalid("unsupported synthetic flash contract");
        }
        s->flash_contract = (XT4FlashContract) {
            .transaction_latency_ns = 64, .cr2_typical_settle_ns = 40,
            .initial_opcode_extensions = 0x13edfa00,
            .initial_write_completion = 0x10005,
            .stig_protocol_policy = XT4_FLASH_PROTOCOL_POLICY,
        };
        g_string_append(canonical,
            "\"flash\":{\"cr2_typical_settle_ns\":40,\"initial_command_word\":0,"
            "\"initial_opcode_extensions\":334363136,\"initial_variant_word\":0,"
            "\"initial_write_completion\":65541,\"powered_settled\":true,"
            "\"stig_protocol_policy\":\"" XT4_FLASH_PROTOCOL_POLICY "\","
            "\"transaction_latency_ns\":64,\"virtual_part\":\"" XT4_NOR_MODEL "\"},");
    } else if (qdict_haskey(scenario, "flash")) {
        invalid("flash assumptions not allowed on older profile");
    }
    if (profile->phy_model) {
        QDict *phy = qobject_to(QDict, qdict_get(scenario, "phy"));
        if (!phy || qdict_size(phy) != 4 ||
            !scenario_number(phy, "master_lock_latency_ns", 4) ||
            !scenario_number(phy, "dll_lock_latency_ns", 4) ||
            !scenario_number(phy, "initial_observable_word", 0) ||
            g_strcmp0(qdict_get_try_str(phy, "reset_policy"), XT4_PHY_RESET_POLICY)) {
            invalid("unsupported synthetic PHY contract");
        }
        s->phy_contract = (XT4PhyContract) {
            .master_lock_latency_ns = 4, .dll_lock_latency_ns = 4,
            .reset_policy = XT4_PHY_RESET_POLICY,
        };
        g_string_append(canonical,
            "\"phy\":{\"dll_lock_latency_ns\":4,\"initial_observable_word\":0,"
            "\"master_lock_latency_ns\":4,\"reset_policy\":\"" XT4_PHY_RESET_POLICY "\"},");
    } else if (qdict_haskey(scenario, "phy")) {
        invalid("PHY assumptions not allowed on older profile");
    }
    if (profile->platform_model) {
        QDict *platform = qobject_to(QDict, qdict_get(scenario, "platform"));
        QList *globals = platform ? qobject_to(QList, qdict_get(platform, "transfer_globals")) : NULL;
        if (!platform || qdict_size(platform) != 2 || !globals || qlist_size(globals) != 16 ||
            !scenario_number(platform, "clock_apply_latency_ns", 1)) {
            invalid("unsupported synthetic platform assumptions");
        }
        s->clock_apply_latency_ns = 1;
        g_string_append(canonical, "\"platform\":{\"clock_apply_latency_ns\":1,\"transfer_globals\":[");
        int index = 0;
        QLIST_FOREACH_ENTRY(globals, entry) {
            QNum *number = qobject_to(QNum, qlist_entry_obj(entry));
            uint64_t value;
            if (!number || !qnum_get_try_uint(number, &value) || value != expected) {
                invalid("platform words differ from fixed synthetic identity");
            }
            s->transfer_globals[index] = value;
            g_string_append_printf(canonical, "%s%" PRIu32, index ? "," : "", expected);
            index++;
        }
        g_string_append(canonical, "]},");
    } else if (qdict_haskey(scenario, "platform")) {
        invalid("platform assumptions not allowed on older profile");
    }
    g_string_append(canonical, "\"port_inputs\":[");
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
    g_string_append_printf(canonical, "],\"scenario_id\":\"%s\",\"timer\":%s", id, timer_json);
    if (profile->calibration_model) {
        QDict *t = qobject_to(QDict, qdict_get(scenario, "transfer"));
        if (!t || qdict_size(t) != 9 ||
            !scenario_number(t, "initial_status_word", 0) ||
            !scenario_number(t, "initial_indirect_control", 0) ||
            !scenario_number(t, "transfer_latency_ns", 64) ||
            !scenario_number(t, "cbar_base", 0xf8f00000) ||
            g_strcmp0(qdict_get_try_str(t, "route_policy"), XT4_ROUTE_POLICY) ||
            g_strcmp0(qdict_get_try_str(t, "source_policy"), XT4_SOURCE_POLICY) ||
            g_strcmp0(qdict_get_try_str(t, "sample_policy"), XT4_SAMPLE_POLICY) ||
            g_strcmp0(qdict_get_try_str(t, "status_policy"), XT4_STATUS_POLICY) ||
            g_strcmp0(qdict_get_try_str(t, "terminal_address_policy"), XT4_ADDRESS_POLICY)) {
            invalid("unsupported synthetic calibration contract");
        }
        s->calibration_contract = (XT4CalibrationTransferContract) {
            .transfer_latency_ns = 64, .cbar_base = 0xf8f00000,
            .route_policy = XT4_ROUTE_POLICY, .source_policy = XT4_SOURCE_POLICY,
            .sample_policy = XT4_SAMPLE_POLICY, .status_policy = XT4_STATUS_POLICY,
            .terminal_address_policy = XT4_ADDRESS_POLICY,
        };
        g_string_append(canonical,
            ",\"transfer\":{\"cbar_base\":4176478208,\"initial_indirect_control\":0,"
            "\"initial_status_word\":0,\"route_policy\":\"" XT4_ROUTE_POLICY "\","
            "\"sample_policy\":\"" XT4_SAMPLE_POLICY "\","
            "\"source_policy\":\"" XT4_SOURCE_POLICY "\","
            "\"status_policy\":\"" XT4_STATUS_POLICY "\","
            "\"terminal_address_policy\":\"" XT4_ADDRESS_POLICY "\",\"transfer_latency_ns\":64}");
    } else if (qdict_haskey(scenario, "transfer")) {
        invalid("transfer assumptions not allowed on older profile");
    }
    g_string_append_c(canonical, '}');
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
        (profile->port_config ? !models || qlist_size(models) != (1 + profile->synthetic_inputs + profile->timer_model + 2 * profile->platform_model + 2 * profile->flash_model + profile->phy_model + 4 * profile->calibration_model) :
                               models && qlist_size(models) != 0)) {
        invalid("unsupported device model set");
    }
    if (profile->port_config) {
        const char *expected[] = {XT4_PORT_CONFIG_MODEL, XT4_STATIC_PORT_INPUT_MODEL, XT4_TIMER_MODEL,
                                  XT4_TRANSFER_MODEL, XT4_CLOCK_APPLY_MODEL, XT4_FLASH_MODEL, XT4_NOR_MODEL, XT4_PHY_MODEL,
                                  XT4_ROUTE_MODEL, XT4_DMA_MODEL, XT4_INDIRECT_MODEL, XT4_MEDIA_MODEL};
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
    if (profile->calibration_model) {
        QDict *m = qobject_to(QDict, qdict_get(p, "media"));
        if (!m || qdict_size(m) != 7 ||
            g_strcmp0(qdict_get_try_str(m, "file"), "inputs/calibration.bin") ||
            g_strcmp0(qdict_get_try_str(m, "sha256"), XT4_CALIBRATION_SHA256) ||
            g_strcmp0(qdict_get_try_str(m, "policy"), XT4_SOURCE_POLICY) ||
            !scenario_number(m, "file_size", 128) ||
            !scenario_number(m, "flash_offset", 0x2a080) ||
            !scenario_number(m, "capacity", 0x4000000) ||
            !scenario_number(m, "virtual_base", 0xf0000000)) {
            invalid("unsupported calibration media");
        }
    } else if (qdict_haskey(p, "media")) {
        invalid("media not allowed on older profile");
    }
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
    if (s->instruction_budget && (qtest_enabled() ||
        icount_enabled() != ICOUNT_PRECISE || icount_to_ns(1) != 1)) {
        invalid("bounded execution requires precise one-nanosecond icount and TCG");
    }
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
    if (s->profile->calibration_model) {
        size_t size;
        s->media_bytes = read_file(inputs, "calibration.bin", &size);
        g_autofree char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                         s->media_bytes, size);
        if (size != 128 || strcmp(hash, XT4_CALIBRATION_SHA256)) {
            invalid("calibration media fingerprint mismatch");
        }
        s->media = (XT4CoveredNor) {
            .bytes = s->media_bytes, .offset = 0x2a080, .length = 128,
            .capacity = 0x4000000, .virtual_base = 0xf0000000,
        };
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
    if (s->profile->timer_model) {
        s->timer = XT4_TIMER_EXPLORATION(qdev_new(TYPE_XT4_TIMER_EXPLORATION));
        xt4_timer_set_event_callback(s->timer, timer_event, s);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(s->timer), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(s->timer), 0, 0xfff3c300);
    }
    if (s->profile->platform_model) {
        s->transfer = XT4_TRANSFER(qdev_new(TYPE_XT4_TRANSFER));
        xt4_transfer_set_initial(s->transfer, s->transfer_globals);
        xt4_transfer_set_event_callback(s->transfer, transfer_event, s);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(s->transfer), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(s->transfer), 0, 0xfffe0000);
        s->clock_apply = XT4_CLOCK_APPLY(qdev_new(TYPE_XT4_CLOCK_APPLY));
        xt4_clock_apply_set_latency(s->clock_apply, s->clock_apply_latency_ns);
        xt4_clock_apply_set_event_callback(s->clock_apply, clock_apply_event, s);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(s->clock_apply), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(s->clock_apply), 0, 0xff701000);
    }
    if (s->profile->flash_model) {
        s->flash = XT4_FLASH(qdev_new(TYPE_XT4_FLASH));
        xt4_flash_set_contract(s->flash, &s->flash_contract);
        if (s->profile->phy_model) {
            s->phy = XT4_PHY(qdev_new(TYPE_XT4_PHY));
            xt4_phy_set_contract(s->phy, &s->phy_contract);
            xt4_phy_set_event_callback(s->phy, phy_event, s);
            xt4_phy_set_completion_guard(s->phy, flash_can_complete, s);
            xt4_flash_set_phy(s->flash, s->phy);
        }
        xt4_flash_set_event_callback(s->flash, flash_event, s);
        xt4_flash_set_completion_guard(s->flash, flash_can_complete, s);
        if (s->profile->calibration_model) {
            s->calibration = XT4_CALIBRATION_TRANSFER(qdev_new(TYPE_XT4_CALIBRATION_TRANSFER));
            xt4_calibration_transfer_set_contract(s->calibration, &s->calibration_contract);
            xt4_calibration_transfer_set_links(s->calibration, s->flash, s->phy,
                s->transfer, &s->ram[0], bootstrap_regions[0].base,
                bootstrap_regions[0].size, &s->media);
            xt4_calibration_transfer_set_event_callback(s->calibration, calibration_event, s);
            xt4_calibration_transfer_set_completion_guard(s->calibration, flash_can_complete, s);
            xt4_flash_set_calibration_transfer(s->flash, s->calibration);
        }
        sysbus_realize_and_unref(SYS_BUS_DEVICE(s->flash), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(s->flash), 0, 0xfffd8000);
        if (s->phy) {
            sysbus_realize_and_unref(SYS_BUS_DEVICE(s->phy), &error_fatal);
            sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->phy), 0, 0xfffd80b4, 1);
        }
        if (s->calibration) {
            const hwaddr bases[] = {0xfff4a500, 0xfffe1010, 0xfffd8060};
            sysbus_realize_and_unref(SYS_BUS_DEVICE(s->calibration), &error_fatal);
            for (unsigned i = 0; i < G_N_ELEMENTS(bases); i++) {
                sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->calibration), i, bases[i], 1);
            }
        }
    }
    s->cpu = XT4_CPU(object_new(TYPE_XT4_CPU));
    s->cpu->board = s;
    if (s->calibration) {
        object_property_set_uint(OBJECT(s->cpu), "reset-cbar",
                                 s->calibration_contract.cbar_base, &error_fatal);
    }
    qdev_realize(DEVICE(s->cpu), NULL, &error_fatal);
    if (s->instruction_budget) {
        s->budget_timer = timer_new_full(NULL, QEMU_CLOCK_VIRTUAL, SCALE_NS,
            QEMU_TIMER_ATTR_EXTERNAL, execution_budget, s);
        s->execution_handler = qemu_add_vm_change_state_handler_prio(
            execution_state, s, 1);
    }
    qemu_register_reset(reset, s);
}

static void get_instruction_budget(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    uint64_t value = XT4_MACHINE(obj)->instruction_budget;
    visit_type_uint64(v, name, &value, errp);
}

static void set_instruction_budget(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    XT4State *s = XT4_MACHINE(obj);
    uint64_t value;
    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (s->cpu || value > 1000000000) {
        error_setg(errp, "instruction budget is initialization-only, range0..1000000000");
        return;
    }
    s->instruction_budget = value;
}

static void xt4_finalize(Object *obj)
{
    XT4State *s = XT4_MACHINE(obj);
    if (s->execution_handler) {
        qemu_del_vm_change_state_handler(s->execution_handler);
    }
    if (s->budget_timer) {
        timer_del(s->budget_timer);
        timer_free(s->budget_timer);
    }
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
    object_class_property_add(oc, "instruction-budget", "uint64",
        get_instruction_budget, set_instruction_budget, NULL, NULL);
}

static const TypeInfo xt4_types[] = {{
    .name = TYPE_XT4_MACHINE, .parent = TYPE_MACHINE,
    .instance_size = sizeof(XT4State), .class_init = class_init,
    .instance_finalize = xt4_finalize,
    .interfaces = arm_machine_interfaces,
}};
DEFINE_TYPES(xt4_types)
