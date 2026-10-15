/* SPDX-License-Identifier: GPL-2.0-or-later
 * Firmware-derived logical configuration model, not electrical GPIO behavior.
 * Reset/readback bits and external samples remain unknown until justified.
 */
#include "qemu/osdep.h"
#include "hw/gpio/xt4-ports.h"
#include "hw/core/resettable.h"
#include "qemu/module.h"

typedef struct XT4PortRegister {
    uint32_t value, known;
} XT4PortRegister;

struct XT4PortsState {
    SysBusDevice parent_obj;
    MemoryRegion ports_mmio, global_mmio;
    XT4PortRegister ports[16][8], global;
    uint32_t inputs[16];
    bool has_inputs;
    XT4PortEventFn callback;
    void *callback_opaque;
};

static MemTxResult access_register(XT4PortsState *s, hwaddr offset,
                                   uint64_t *value, unsigned size,
                                   bool write, bool global)
{
    XT4PortEvent event = {
        .address = (global ? 0xfff41500 : 0xfff41000) + offset,
        .width = size, .write = write, .global = global,
        .value = write ? *value : 0,
        .port = global ? 0 : offset / 0x40,
        .selector = global ? 0 : (offset % 0x40) / 4,
        .model = XT4_PORT_CONFIG_MODEL,
        .semantics = "inferred logical configuration; not electrical state",
    };
    XT4PortRegister *reg = NULL;
    if (size != 4 || (offset & 3) ||
        (global ? offset != 0 : event.port >= 16 || event.selector >= 8)) {
        event.detail = "unsupported_port_access";
    } else {
        reg = global ? &s->global : &s->ports[event.port][event.selector];
        if (!global && event.selector == 5) {
            if (!write && s->has_inputs) {
                event.value = event.state = s->inputs[event.port];
                event.known_mask = UINT32_MAX;
                event.model = XT4_STATIC_PORT_INPUT_MODEL;
                event.semantics = "synthetic static input fixture; not physical camera state";
                reg = NULL;
            } else {
                event.detail = write ? "unsupported_port_write" : "unknown_port_input";
            }
        } else if (write) {
            if (global || event.selector <= 4) {
                uint32_t mask = *value >> 16;
                uint32_t data = *value & 0xffff;
                if (!global && (event.selector == 3 || event.selector == 4) &&
                    (mask & 0x5555) != ((mask >> 1) & 0x5555)) {
                    event.detail = "unsupported_port_mask";
                } else {
                    reg->value = (reg->value & ~mask) | (data & mask);
                    reg->known |= mask;
                }
            } else {
                reg->value = *value;
                reg->known = UINT32_MAX;
            }
        } else if (reg->known != UINT32_MAX) {
            event.detail = "unknown_port_readback";
        } else {
            event.value = reg->value;
        }
    }
    if (reg) {
        event.state = reg->value;
        event.known_mask = reg->known;
    }
    event.success = event.detail == NULL;
    if (!write) {
        /* Required output argument; failed reads are never guest data. */
        *value = event.success ? event.value : 0;
    }
    if (s->callback) {
        s->callback(s->callback_opaque, &event);
    }
    return event.success ? MEMTX_OK : MEMTX_ERROR;
}

static MemTxResult port_read(void *opaque, hwaddr addr, uint64_t *value,
                             unsigned size, MemTxAttrs attrs)
{
    return access_register(opaque, addr, value, size, false, false);
}

static MemTxResult port_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    return access_register(opaque, addr, &value, size, true, false);
}

static MemTxResult global_read(void *opaque, hwaddr addr, uint64_t *value,
                               unsigned size, MemTxAttrs attrs)
{
    return access_register(opaque, addr, value, size, false, true);
}

static MemTxResult global_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size, MemTxAttrs attrs)
{
    return access_register(opaque, addr, &value, size, true, true);
}

/* Let the callback reject unsupported widths without silently splitting them. */
#define XT4_ACCESS_RULES \
    .endianness = DEVICE_LITTLE_ENDIAN, \
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true }, \
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true }

static const MemoryRegionOps port_ops = {
    .read_with_attrs = port_read, .write_with_attrs = port_write,
    XT4_ACCESS_RULES,
};
static const MemoryRegionOps global_ops = {
    .read_with_attrs = global_read, .write_with_attrs = global_write,
    XT4_ACCESS_RULES,
};

void xt4_ports_set_event_callback(XT4PortsState *s, XT4PortEventFn callback,
                                 void *opaque)
{
    s->callback = callback;
    s->callback_opaque = opaque;
}

void xt4_ports_set_inputs(XT4PortsState *s, const uint32_t values[16])
{
    memcpy(s->inputs, values, sizeof(s->inputs));
    s->has_inputs = true;
}

static void ports_reset(Object *obj, ResetType type)
{
    XT4PortsState *s = XT4_PORT_CONFIG(obj);
    /* Internal placeholders are zero, but all guest-visible bits are unknown. */
    memset(s->ports, 0, sizeof(s->ports));
    memset(&s->global, 0, sizeof(s->global));
}

static void ports_init(Object *obj)
{
    XT4PortsState *s = XT4_PORT_CONFIG(obj);
    memory_region_init_io(&s->ports_mmio, obj, &port_ops, s, TYPE_XT4_PORT_CONFIG,
                           0x400);
    memory_region_init_io(&s->global_mmio, obj, &global_ops, s,
                           TYPE_XT4_PORT_CONFIG "-global", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->ports_mmio);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->global_mmio);
}

static void ports_class_init(ObjectClass *klass, const void *data)
{
    DEVICE_CLASS(klass)->user_creatable = false;
    RESETTABLE_CLASS(klass)->phases.hold = ports_reset;
}

static const TypeInfo ports_type = {
    .name = TYPE_XT4_PORT_CONFIG, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(XT4PortsState), .instance_init = ports_init,
    .class_init = ports_class_init,
};

static void ports_register_types(void)
{
    type_register_static(&ports_type);
}
type_init(ports_register_types)
