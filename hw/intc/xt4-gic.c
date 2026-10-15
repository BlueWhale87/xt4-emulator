/* SPDX-License-Identifier: GPL-2.0-or-later
 * Bounded core-0 adapter to the actual QEMU GICv2 implementation.
 * Layout, revision, reset and inventory are declared virtual choices.
 */
#include "qemu/osdep.h"
#include "hw/intc/xt4-gic.h"
#include "hw/intc/arm_gic.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/core/irq.h"
#include "qemu/module.h"

struct XT4GicState {
    SysBusDevice parent_obj;
    GICState gic;
    MemoryRegion mmio;
    qemu_irq irq;
    bool irq_level;
    XT4GicEventFn callback;
    void *callback_opaque;
};

static void notify(XT4GicState *s, XT4GicEvent *e)
{
    e->success = e->detail == NULL;
    e->irq_level = s->irq_level;
    if (s->callback) {
        s->callback(s->callback_opaque, e);
    }
}

static void irq_changed(void *opaque, int n, int level)
{
    XT4GicState *s = opaque;
    if (s->irq_level != !!level) {
        s->irq_level = !!level;
        XT4GicEvent e = { .address = 0xf8f02000, .transition = true };
        notify(s, &e);
    }
    qemu_set_irq(s->irq, level);
}

static const char *admit(hwaddr off, unsigned size, bool write, uint64_t value)
{
    if (size != 4 || (off & 3)) {
        return "unsupported_gic_width_alignment";
    }
    if (off >= 0x1000) {
        switch (off - 0x1000) {
        case 0:
            return write && (value & ~1U) ? "unsupported_gic_control" : NULL;
        case 4:
            return write && value > 255 ? "unsupported_gic_priority_mask" : NULL;
        case 8:
            return write && value > 7 ? "unsupported_gic_binary_point" : NULL;
        case 0xc: case 0x14: case 0x18:
            return write ? "readonly_gic_register" : NULL;
        case 0x10:
            if (!write) {
                return "writeonly_gic_register";
            }
            return (value & ~0x1fffU) ||
                   ((value & 0x3ff) >= 64 && (value & 0x3ff) != 1023) ?
                   "unsupported_gic_eoi" : NULL;
        default:
            return "unsupported_gic_register";
        }
    }
    switch (off) {
    case 0:
        return write && (value & ~1U) ? "unsupported_gic_control" : NULL;
    case 4: case 8:
        return write ? "readonly_gic_register" : NULL;
    case 0xf00:
        if (!write) {
            return "writeonly_gic_register";
        }
        return (value & ~0x03ff000fU) || ((value >> 24) & 3) == 3 ?
               "unsupported_gic_sgir" : NULL;
    }
    if ((off >= 0x100 && off <= 0x104) ||
        (off >= 0x180 && off <= 0x184) ||
        (off >= 0x200 && off <= 0x204) ||
        (off >= 0x280 && off <= 0x284) ||
        (off >= 0x400 && off <= 0x43c)) {
        return NULL;
    }
    if ((off >= 0x300 && off <= 0x304) ||
        (off >= 0x800 && off <= 0x83c) ||
        (off >= 0xc00 && off <= 0xc0c)) {
        return write ? "readonly_gic_register" : NULL;
    }
    return "unsupported_gic_register";
}

static MemTxResult access_gic(XT4GicState *s, hwaddr off, uint64_t *value,
                               unsigned size, bool write, MemTxAttrs attrs)
{
    XT4GicEvent e = { .address = 0xf8f01000 + off, .width = size,
                      .write = write, .value = write ? *value : 0 };
    e.detail = admit(off, size, write, *value);
    MemTxResult result = MEMTX_ERROR;
    if (!e.detail) {
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->gic),
                                                 off >= 0x1000);
        result = write ? memory_region_dispatch_write(mr, off & 0xfff,
                                                       *value, MO_LEUL, attrs) :
                         memory_region_dispatch_read(mr, off & 0xfff,
                                                      &e.value, MO_LEUL, attrs);
        if (result != MEMTX_OK) {
            e.detail = "gic_backend_rejected";
        }
    }
    notify(s, &e);
    if (!write) {
        *value = e.success ? e.value : 0;
    }
    return result;
}

static MemTxResult read_gic(void *opaque, hwaddr off, uint64_t *value,
                             unsigned size, MemTxAttrs attrs)
{
    *value = 0;
    return access_gic(opaque, off, value, size, false, attrs);
}
static MemTxResult write_gic(void *opaque, hwaddr off, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    return access_gic(opaque, off, &value, size, true, attrs);
}
static const MemoryRegionOps ops = {
    .read_with_attrs = read_gic, .write_with_attrs = write_gic,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

void xt4_gic_set_event_callback(XT4GicState *s, XT4GicEventFn cb, void *opaque)
{
    s->callback = cb;
    s->callback_opaque = opaque;
}
static void realize(DeviceState *dev, Error **errp)
{
    XT4GicState *s = XT4_GIC(dev);
    qdev_prop_set_uint32(DEVICE(&s->gic), "revision", 2);
    qdev_prop_set_uint32(DEVICE(&s->gic), "num-cpu", 1);
    qdev_prop_set_uint32(DEVICE(&s->gic), "num-irq", 64);
    qdev_prop_set_uint32(DEVICE(&s->gic), "num-priority-bits", 5);
    qdev_prop_set_bit(DEVICE(&s->gic), "has-security-extensions", false);
    qdev_prop_set_bit(DEVICE(&s->gic), "has-virtualization-extensions", false);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gic), errp)) {
        return;
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), 0, qdev_get_gpio_in(dev, 0));
}
static void reset(Object *obj, ResetType type)
{
    XT4GicState *s = XT4_GIC(obj);

    device_cold_reset(DEVICE(&s->gic));
    /* The pinned backend leaves active priorities and its output untouched. */
    memset(s->gic.apr, 0, sizeof(s->gic.apr));
    memset(s->gic.nsapr, 0, sizeof(s->gic.nsapr));
    irq_changed(s, 0, 0);
}
static void init(Object *obj)
{
    XT4GicState *s = XT4_GIC(obj);
    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GIC);
    memory_region_init_io(&s->mmio, obj, &ops, s, TYPE_XT4_GIC, 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    qdev_init_gpio_in(DEVICE(s), irq_changed, 1);
}
static void class_init(ObjectClass *klass, const void *data)
{
    DEVICE_CLASS(klass)->user_creatable = false;
    DEVICE_CLASS(klass)->realize = realize;
    RESETTABLE_CLASS(klass)->phases.hold = reset;
}
static const TypeInfo type = {
    .name = TYPE_XT4_GIC, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(XT4GicState), .instance_init = init,
    .class_init = class_init,
};
static void register_type(void) { type_register_static(&type); }
type_init(register_type)
