/* SPDX-License-Identifier: GPL-2.0-or-later
 * Causal virtual PHY lock handshake. No electrical or passing-tap model.
 */
#include "qemu/osdep.h"
#include "hw/ssi/xt4-phy.h"
#include "hw/core/resettable.h"
#include "qemu/module.h"
#include "qemu/timer.h"

typedef enum PhyPhase {
    PHY_DISABLED, PHY_ENABLED, PHY_RESET, PHY_MASTER_PENDING,
    PHY_MASTER_READY, PHY_DLL_PENDING, PHY_DLL_LOCKED, PHY_CANCELLED,
} PhyPhase;

struct XT4PhyState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    QEMUTimer *timer;
    XT4PhyContract contract;
    uint32_t configuration, master_control, observable;
    bool controller_known, enabled, configuration_known, master_known;
    bool fresh_master, busy;
    PhyPhase phase;
    uint64_t epoch, pending_epoch;
    int64_t deadline_ns;
    XT4PhyEventFn callback;
    void *callback_opaque;
    XT4PhyCompletionGuard guard;
    void *guard_opaque;
};

static const char *phase_name(PhyPhase phase)
{
    static const char *const names[] = {
        "disabled", "enabled", "reset", "master-pending", "master-ready",
        "dll-pending", "dll-locked", "cancelled",
    };
    return names[phase];
}

static void notify(XT4PhyState *s, XT4PhyEvent *e)
{
    e->success = e->detail == NULL;
    e->busy = s->busy;
    e->controller_known = s->controller_known;
    e->enabled = s->enabled;
    e->configuration_known = s->configuration_known;
    e->master_known = s->master_known;
    e->fresh_master = s->fresh_master;
    e->configuration = s->configuration;
    e->master_control = s->master_control;
    e->observable = s->observable;
    e->epoch = s->epoch;
    e->deadline_ns = s->deadline_ns;
    e->phase = phase_name(s->phase);
    if (s->callback) {
        s->callback(s->callback_opaque, e);
    }
}

void xt4_phy_cancel_pending(XT4PhyState *s)
{
    timer_del(s->timer);
    if (s->busy) {
        s->busy = false;
        s->phase = PHY_CANCELLED;
        s->deadline_ns = -1;
        s->epoch++;
    }
}

static void phy_complete(void *opaque)
{
    XT4PhyState *s = opaque;
    if (!s->busy || (s->guard && !s->guard(s->guard_opaque))) {
        xt4_phy_cancel_pending(s);
        return;
    }
    XT4PhyEvent e = {
        .address = 0xfffd80bc, .transition = true,
        .virtual_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
    };
    if (s->pending_epoch != s->epoch || !s->enabled ||
        (s->phase != PHY_MASTER_PENDING && s->phase != PHY_DLL_PENDING)) {
        e.detail = "stale_phy_transition";
        xt4_phy_cancel_pending(s);
    } else {
        s->observable |= s->phase == PHY_MASTER_PENDING ? 0x8000 : 1;
        s->phase = s->phase == PHY_MASTER_PENDING ? PHY_MASTER_READY : PHY_DLL_LOCKED;
        s->busy = false;
        s->epoch++;
    }
    notify(s, &e);
}

bool xt4_phy_set_controller_config(XT4PhyState *s, bool enabled,
                                    const char **detail)
{
    if (s->busy) {
        *detail = "busy_phy_controller_config";
        return false;
    }
    s->controller_known = true;
    if (!enabled || !s->enabled) {
        s->observable = s->contract.initial_observable_word;
        s->fresh_master = false;
        s->phase = enabled ? PHY_ENABLED : PHY_DISABLED;
        s->deadline_ns = -1;
        s->epoch++;
    }
    s->enabled = enabled;
    *detail = NULL;
    return true;
}

static const char *phy_write_value(XT4PhyState *s, hwaddr off, uint32_t value,
                                   int64_t now)
{
    if (off == 8) {
        return "readonly_phy_observable";
    }
    if (!s->controller_known) {
        return "unknown_phy_controller_config";
    }
    if (!s->enabled) {
        if (off != 0) {
            return "disabled_phy";
        }
        if (value != 0x80200020 && value != 0) {
            return "unsupported_phy_configuration";
        }
        s->configuration = value;
        s->configuration_known = true;
        return NULL;
    }
    if (off == 0 && value == 0) {
        xt4_phy_cancel_pending(s);
        s->configuration = 0;
        s->configuration_known = true;
        s->observable = s->contract.initial_observable_word;
        s->fresh_master = false;
        s->phase = PHY_RESET;
        s->deadline_ns = -1;
        s->epoch++;
        return NULL;
    }
    if (s->busy) {
        return "busy_phy";
    }
    if (off == 4) {
        if (value != 30 && value != 40 && value != 50) {
            return "unsupported_phy_master";
        }
        if (s->phase != PHY_RESET) {
            return "unstaged_phy_master";
        }
        s->master_control = value;
        s->master_known = s->fresh_master = true;
        return NULL;
    }
    PhyPhase pending;
    uint64_t latency;
    if (value == 0x40000000) {
        if (s->phase != PHY_RESET || !s->fresh_master) {
            return "unstaged_phy_master";
        }
        pending = PHY_MASTER_PENDING;
        latency = s->contract.master_lock_latency_ns;
    } else if ((value & ~0x7fU) == 0xc01f0000) {
        if (s->phase != PHY_MASTER_READY && s->phase != PHY_DLL_LOCKED) {
            return "unstaged_phy_resync";
        }
        pending = PHY_DLL_PENDING;
        latency = s->contract.dll_lock_latency_ns;
    } else {
        return "unsupported_phy_configuration";
    }
    if (now < 0 || latency > INT64_MAX - now) {
        return "unsupported_phy_deadline";
    }
    s->configuration = value;
    s->configuration_known = true;
    s->fresh_master = false;
    s->phase = pending;
    s->busy = true;
    s->pending_epoch = s->epoch;
    s->deadline_ns = now + latency;
    if (pending == PHY_DLL_PENDING) {
        s->observable &= ~1U;
    }
    timer_mod_ns(s->timer, s->deadline_ns);
    return NULL;
}

static MemTxResult phy_access(XT4PhyState *s, hwaddr off, uint64_t *value,
                              unsigned size, bool write)
{
    XT4PhyEvent e = {
        .address = 0xfffd80b4 + off, .width = size,
        .write = write, .value = write ? *value : 0,
        .virtual_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
    };
    if (size != 4 || (off != 0 && off != 4 && off != 8)) {
        e.detail = "unsupported_phy_access";
    } else if (write) {
        e.detail = phy_write_value(s, off, *value, e.virtual_ns);
    } else if ((off == 0 && !s->configuration_known) ||
               (off == 4 && !s->master_known)) {
        e.detail = "unknown_phy_register";
    } else {
        e.value = off == 0 ? s->configuration : off == 4 ? s->master_control : s->observable;
    }
    notify(s, &e);
    if (!write) {
        *value = e.success ? e.value : 0;
    }
    return e.success ? MEMTX_OK : MEMTX_ERROR;
}

static MemTxResult phy_read(void *opaque, hwaddr off, uint64_t *value,
                            unsigned size, MemTxAttrs attrs)
{
    return phy_access(opaque, off, value, size, false);
}

static MemTxResult phy_write(void *opaque, hwaddr off, uint64_t value,
                             unsigned size, MemTxAttrs attrs)
{
    return phy_access(opaque, off, &value, size, true);
}

static const MemoryRegionOps phy_ops = {
    .read_with_attrs = phy_read, .write_with_attrs = phy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

void xt4_phy_set_contract(XT4PhyState *s, const XT4PhyContract *c)
{
    g_assert_cmpuint(c->master_lock_latency_ns, ==, 4);
    g_assert_cmpuint(c->dll_lock_latency_ns, ==, 4);
    g_assert_cmpuint(c->initial_observable_word, ==, 0);
    g_assert_cmpstr(c->reset_policy, ==, XT4_PHY_RESET_POLICY);
    s->contract = *c;
}

void xt4_phy_set_event_callback(XT4PhyState *s, XT4PhyEventFn cb, void *opaque)
{
    s->callback = cb;
    s->callback_opaque = opaque;
}

void xt4_phy_set_completion_guard(XT4PhyState *s, XT4PhyCompletionGuard cb, void *opaque)
{
    s->guard = cb;
    s->guard_opaque = opaque;
}

static void phy_reset(Object *obj, ResetType type)
{
    XT4PhyState *s = XT4_PHY(obj);
    xt4_phy_cancel_pending(s);
    s->configuration = s->master_control = 0;
    s->observable = s->contract.initial_observable_word;
    s->controller_known = s->enabled = s->configuration_known = s->master_known = false;
    s->fresh_master = false;
    s->phase = PHY_DISABLED;
    s->deadline_ns = -1;
    s->epoch++;
}

static void phy_init(Object *obj)
{
    XT4PhyState *s = XT4_PHY(obj);
    s->timer = timer_new_full(NULL, QEMU_CLOCK_VIRTUAL, SCALE_NS,
                            QEMU_TIMER_ATTR_EXTERNAL, phy_complete, s);
    memory_region_init_io(&s->mmio, obj, &phy_ops, s, TYPE_XT4_PHY, 12);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
}

static void phy_finalize(Object *obj)
{
    XT4PhyState *s = XT4_PHY(obj);
    timer_del(s->timer);
    timer_free(s->timer);
}

static void phy_class_init(ObjectClass *klass, const void *data)
{
    DEVICE_CLASS(klass)->user_creatable = false;
    RESETTABLE_CLASS(klass)->phases.hold = phy_reset;
}

static const TypeInfo phy_type = {
    .name = TYPE_XT4_PHY, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(XT4PhyState), .instance_init = phy_init,
    .instance_finalize = phy_finalize, .class_init = phy_class_init,
};

static void phy_register_types(void)
{
    type_register_static(&phy_type);
}
type_init(phy_register_types)
