/* SPDX-License-Identifier: GPL-2.0-or-later
 * Synthetic selection-latch handshake. No CPU or peripheral clock is changed.
 */
#include "qemu/osdep.h"
#include "hw/misc/xt4-clock-apply.h"
#include "hw/core/resettable.h"
#include "qemu/module.h"
#include "qemu/timer.h"

struct XT4ClockApplyState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    QEMUTimer *timer;
    uint64_t latency_ns;
    uint32_t shadow[2], active[2], pending[2], command;
    unsigned shadow_known, active_known, staged, pending_mask;
    bool busy, command_known;
    int64_t deadline_ns;
    XT4ClockApplyEventFn callback;
    void *callback_opaque;
};

static void notify(XT4ClockApplyState *s, XT4ClockApplyEvent *event)
{
    memcpy(event->shadow, s->shadow, sizeof(event->shadow));
    memcpy(event->active, s->active, sizeof(event->active));
    event->shadow_known = s->shadow_known;
    event->active_known = s->active_known;
    event->staged = s->staged;
    event->busy = s->busy;
    event->command_known = s->command_known;
    event->command = s->command;
    event->deadline_ns = s->deadline_ns;
    event->success = event->detail == NULL;
    if (s->callback) {
        s->callback(s->callback_opaque, event);
    }
}

static void apply_complete(void *opaque)
{
    XT4ClockApplyState *s = opaque;
    if (!s->busy) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (s->pending_mask & (1 << i)) {
            s->active[i] = s->pending[i];
        }
    }
    s->active_known |= s->pending_mask;
    s->pending_mask = 0;
    s->busy = false;
    s->command = 0;
    XT4ClockApplyEvent event = {
        .address = 0xff701080, .transition = true,
        .virtual_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
    };
    notify(s, &event);
}

static MemTxResult clock_access(XT4ClockApplyState *s, hwaddr offset,
                                uint64_t *value, unsigned size, bool write)
{
    XT4ClockApplyEvent event = {
        .address = 0xff701000 + offset, .width = size,
        .write = write, .value = write ? *value : 0,
        .virtual_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
    };
    unsigned index = offset == 0x48 ? 1 : 0;
    if (size != 4 || (offset != 0x40 && offset != 0x48 && offset != 0x80)) {
        event.detail = "unsupported_clock_access";
    } else if (!write && offset == 0x80) {
        if (!s->command_known) {
            event.detail = "unknown_clock_command_readback";
        } else {
            event.value = s->command;
        }
    } else if (!write) {
        if (!(s->shadow_known & (1 << index))) {
            event.detail = "unknown_clock_selection_readback";
        } else {
            event.value = s->shadow[index];
        }
    } else if (s->busy) {
        /* No queueing or modification of an in-flight latch is established. */
        event.detail = "busy_clock_apply";
    } else if (offset != 0x80) {
        if (*value != (index ? 0x107 : 0x101)) {
            event.detail = "unsupported_clock_selection";
        } else {
            s->shadow[index] = *value;
            s->shadow_known |= 1 << index;
            s->staged |= 1 << index;
        }
    } else if (*value != 2) {
        event.detail = "unsupported_clock_command";
    } else if (!s->staged) {
        event.detail = "unstaged_clock_apply";
    } else if (event.virtual_ns < 0 || !s->latency_ns ||
               s->latency_ns > INT64_MAX - event.virtual_ns) {
        event.detail = "unsupported_clock_deadline";
    } else {
        memcpy(s->pending, s->shadow, sizeof(s->pending));
        s->pending_mask = s->staged;
        s->staged = 0;
        s->busy = s->command_known = true;
        s->command = 2;
        s->deadline_ns = event.virtual_ns + s->latency_ns;
        timer_mod_ns(s->timer, s->deadline_ns);
    }
    notify(s, &event);
    if (!write) {
        *value = event.success ? event.value : 0;
    }
    return event.success ? MEMTX_OK : MEMTX_ERROR;
}

static MemTxResult clock_read(void *opaque, hwaddr offset, uint64_t *value,
                              unsigned size, MemTxAttrs attrs)
{
    return clock_access(opaque, offset, value, size, false);
}

static MemTxResult clock_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size, MemTxAttrs attrs)
{
    return clock_access(opaque, offset, &value, size, true);
}

static const MemoryRegionOps clock_ops = {
    .read_with_attrs = clock_read, .write_with_attrs = clock_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

void xt4_clock_apply_set_latency(XT4ClockApplyState *s, uint64_t latency_ns)
{
    g_assert_cmpuint(latency_ns, ==, 1);
    s->latency_ns = latency_ns;
}

void xt4_clock_apply_set_event_callback(XT4ClockApplyState *s,
                                       XT4ClockApplyEventFn cb, void *opaque)
{
    s->callback = cb;
    s->callback_opaque = opaque;
}

static void clock_reset(Object *obj, ResetType type)
{
    XT4ClockApplyState *s = XT4_CLOCK_APPLY(obj);
    timer_del(s->timer);
    memset(s->shadow, 0, sizeof(s->shadow));
    memset(s->active, 0, sizeof(s->active));
    memset(s->pending, 0, sizeof(s->pending));
    s->shadow_known = s->active_known = s->staged = s->pending_mask = 0;
    s->busy = s->command_known = false;
    s->command = 0;
    s->deadline_ns = -1;
}

static void clock_init(Object *obj)
{
    XT4ClockApplyState *s = XT4_CLOCK_APPLY(obj);
    s->timer = timer_new_full(NULL, QEMU_CLOCK_VIRTUAL, SCALE_NS,
        QEMU_TIMER_ATTR_EXTERNAL, apply_complete, s);
    memory_region_init_io(&s->mmio, obj, &clock_ops, s, TYPE_XT4_CLOCK_APPLY, 0x84);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
}

static void clock_finalize(Object *obj)
{
    XT4ClockApplyState *s = XT4_CLOCK_APPLY(obj);
    timer_del(s->timer);
    timer_free(s->timer);
}

static void clock_class_init(ObjectClass *klass, const void *data)
{
    DEVICE_CLASS(klass)->user_creatable = false;
    RESETTABLE_CLASS(klass)->phases.hold = clock_reset;
}

static const TypeInfo clock_type = {
    .name = TYPE_XT4_CLOCK_APPLY, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(XT4ClockApplyState), .instance_init = clock_init,
    .instance_finalize = clock_finalize, .class_init = clock_class_init,
};

static void clock_register_types(void)
{
    type_register_static(&clock_type);
}
type_init(clock_register_types)
