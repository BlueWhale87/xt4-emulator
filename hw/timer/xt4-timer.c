/* SPDX-License-Identifier: GPL-2.0-or-later
 * Synthetic pre-terminal timer exploration. No IRQ or reload is invented.
 */
#include "qemu/osdep.h"
#include "hw/timer/xt4-timer.h"
#include "hw/core/resettable.h"
#include "qemu/host-utils.h"
#include "qemu/module.h"
#include "qemu/timer.h"

typedef struct XT4TimerChannel {
    XT4TimerState *owner;
    QEMUTimer *timer;
    unsigned index;
    uint32_t current, adjacent;
    bool current_known, adjacent_known, enabled, terminal_unknown;
    int64_t epoch_ns, deadline_ns;
} XT4TimerChannel;

struct XT4TimerState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    XT4TimerChannel channels[8];
    XT4TimerEventFn callback;
    void *callback_opaque;
};

static bool current_at(XT4TimerChannel *ch, int64_t now, uint32_t *value)
{
    if (!ch->current_known || ch->terminal_unknown || now < ch->epoch_ns) {
        return false;
    }
    uint64_t elapsed = ch->enabled ?
        muldiv64(now - ch->epoch_ns, 54000000, 1000000000) : 0;
    if (ch->enabled && elapsed >= ch->current) {
        return false; /* Zero/expiry behavior is outside this model. */
    }
    *value = ch->current - elapsed;
    return true;
}

static bool deadline_at(int64_t now, uint32_t count, int64_t *deadline)
{
    uint64_t duration = muldiv64_round_up(count, 1000000000, 54000000);
    if (now < 0 || duration > INT64_MAX - now) {
        return false;
    }
    *deadline = now + duration;
    return true;
}

static void snapshot(XT4TimerChannel *ch, XT4TimerEvent *event)
{
    event->channel = ch->index;
    event->current_known = current_at(ch, event->virtual_ns, &event->current);
    event->adjacent = ch->adjacent;
    event->adjacent_known = ch->adjacent_known;
    event->enabled = ch->enabled;
    event->terminal_unknown = ch->terminal_unknown;
    event->deadline_ns = ch->deadline_ns;
}

static void terminal_count(void *opaque)
{
    XT4TimerChannel *ch = opaque;
    if (!ch->enabled || ch->terminal_unknown) {
        return;
    }
    ch->terminal_unknown = true;
    XT4TimerEvent event = {
        .address = 0xfff3c310 + ch->index * 8,
        .virtual_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        .transition = true, .detail = "unknown_timer_terminal_count",
    };
    snapshot(ch, &event);
    if (ch->owner->callback) {
        ch->owner->callback(ch->owner->callback_opaque, &event);
    }
}

static MemTxResult timer_access(XT4TimerState *s, hwaddr offset,
                                uint64_t *value, unsigned size, bool write)
{
    XT4TimerEvent event = {
        .address = 0xfff3c300 + offset, .channel = -1,
        .width = size, .value = write ? *value : 0,
        .virtual_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        .deadline_ns = -1, .write = write,
    };
    XT4TimerChannel *ch = NULL;
    if (size != 4 || offset & 3 || offset >= 0x50 ||
        (offset != 0 && offset < 0x10)) {
        event.detail = "unsupported_timer_access";
    } else if (offset == 0) {
        uint32_t mask = write ? *value >> 16 : 0;
        uint32_t data = write ? *value & 0xffff : 0;
        if (!write) {
            event.detail = "unknown_timer_control_readback";
        } else if (!mask || mask > 0x80 || (mask & (mask - 1)) ||
                   (data != 0 && data != mask)) {
            event.detail = "unsupported_timer_control";
        } else {
            ch = &s->channels[ctz32(mask)];
            uint32_t current;
            int64_t deadline;
            if (ch->terminal_unknown) {
                event.detail = "unknown_timer_terminal_count";
            } else if (data && !ch->current_known) {
                event.detail = "unknown_timer_count";
            } else if (data && !ch->current) {
                event.detail = "unknown_timer_terminal_count";
            } else if (data && !ch->enabled &&
                       !deadline_at(event.virtual_ns, ch->current, &deadline)) {
                event.detail = "unsupported_timer_deadline";
            } else if (data && !ch->enabled) {
                ch->enabled = true;
                ch->epoch_ns = event.virtual_ns;
                ch->deadline_ns = deadline;
                timer_mod_ns(ch->timer, deadline);
            } else if (!data && ch->enabled) {
                if (!current_at(ch, event.virtual_ns, &current)) {
                    event.detail = "unknown_timer_terminal_count";
                } else {
                    ch->current = current;
                    ch->enabled = false;
                    ch->epoch_ns = event.virtual_ns;
                    ch->deadline_ns = -1;
                    timer_del(ch->timer);
                }
            }
        }
    } else {
        ch = &s->channels[(offset - 0x10) / 8];
        bool adjacent = (offset & 4) != 0;
        int64_t deadline;
        uint32_t current;
        if (ch->terminal_unknown) {
            event.detail = "unknown_timer_terminal_count";
        } else if (write && !adjacent && ch->enabled && !*value) {
            event.detail = "unknown_timer_terminal_count";
        } else if (write && !adjacent && ch->enabled &&
                   !deadline_at(event.virtual_ns, *value, &deadline)) {
            event.detail = "unsupported_timer_deadline";
        } else if (write && adjacent) {
            ch->adjacent = *value;
            ch->adjacent_known = true;
        } else if (write) {
            ch->current = *value;
            ch->current_known = true;
            ch->epoch_ns = event.virtual_ns;
            if (ch->enabled) {
                ch->deadline_ns = deadline;
                timer_mod_ns(ch->timer, deadline);
            }
        } else if (adjacent && ch->adjacent_known) {
            event.value = ch->adjacent;
        } else if (!adjacent && current_at(ch, event.virtual_ns, &current)) {
            event.value = current;
        } else {
            event.detail = "unknown_timer_readback";
        }
    }
    if (ch) {
        snapshot(ch, &event);
    }
    event.success = event.detail == NULL;
    if (!write) {
        *value = event.success ? event.value : 0;
    }
    if (s->callback) {
        s->callback(s->callback_opaque, &event);
    }
    return event.success ? MEMTX_OK : MEMTX_ERROR;
}

static MemTxResult timer_read(void *opaque, hwaddr offset, uint64_t *value,
                              unsigned size, MemTxAttrs attrs)
{
    return timer_access(opaque, offset, value, size, false);
}

static MemTxResult timer_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size, MemTxAttrs attrs)
{
    return timer_access(opaque, offset, &value, size, true);
}

static const MemoryRegionOps timer_ops = {
    .read_with_attrs = timer_read, .write_with_attrs = timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

void xt4_timer_set_event_callback(XT4TimerState *s, XT4TimerEventFn callback,
                                 void *opaque)
{
    s->callback = callback;
    s->callback_opaque = opaque;
}

static void timer_reset(Object *obj, ResetType type)
{
    XT4TimerState *s = XT4_TIMER_EXPLORATION(obj);
    for (int i = 0; i < 8; i++) {
        XT4TimerChannel *ch = &s->channels[i];
        timer_del(ch->timer);
        ch->current = ch->adjacent = 0;
        ch->current_known = ch->adjacent_known = false;
        ch->enabled = ch->terminal_unknown = false;
        ch->epoch_ns = 0;
        ch->deadline_ns = -1;
    }
}

static void xt4_timer_init(Object *obj)
{
    XT4TimerState *s = XT4_TIMER_EXPLORATION(obj);
    for (int i = 0; i < 8; i++) {
        XT4TimerChannel *ch = &s->channels[i];
        ch->owner = s;
        ch->index = i;
        /* Exclude this synthetic deadline from icount's idle-time warp.
         * Normal instruction deadlines and qtest clock advancement still run
         * it; debugger pauses must not skip to terminal count. */
        ch->timer = timer_new_full(NULL, QEMU_CLOCK_VIRTUAL, SCALE_NS,
                                   QEMU_TIMER_ATTR_EXTERNAL, terminal_count, ch);
    }
    memory_region_init_io(&s->mmio, obj, &timer_ops, s,
                           TYPE_XT4_TIMER_EXPLORATION, 0x50);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
}

static void timer_finalize(Object *obj)
{
    XT4TimerState *s = XT4_TIMER_EXPLORATION(obj);
    for (int i = 0; i < 8; i++) {
        timer_del(s->channels[i].timer);
        timer_free(s->channels[i].timer);
    }
}

static void timer_class_init(ObjectClass *klass, const void *data)
{
    DEVICE_CLASS(klass)->user_creatable = false;
    RESETTABLE_CLASS(klass)->phases.hold = timer_reset;
}

static const TypeInfo timer_type = {
    .name = TYPE_XT4_TIMER_EXPLORATION, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(XT4TimerState), .instance_init = xt4_timer_init,
    .instance_finalize = timer_finalize, .class_init = timer_class_init,
};

static void timer_register_types(void)
{
    type_register_static(&timer_type);
}
type_init(timer_register_types)
