/* SPDX-License-Identifier: GPL-2.0-or-later
 * Inferred global enable bit over explicit synthetic initial words.
 * No transfer, descriptor, status, completion or IRQ is implemented here.
 */
#include "qemu/osdep.h"
#include "hw/dma/xt4-transfer.h"
#include "hw/core/resettable.h"
#include "qemu/module.h"

struct XT4TransferState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t initial[16], globals[16];
    XT4TransferEventFn callback;
    void *callback_opaque;
};

static MemTxResult transfer_access(XT4TransferState *s, hwaddr offset,
                                   uint64_t *value, unsigned size, bool write)
{
    unsigned bank = offset / 0x1000;
    XT4TransferEvent event = {
        .address = 0xfffe0000 + offset, .width = size, .bank = bank,
        .write = write, .value = write ? *value : 0,
    };
    if (bank < 16) {
        event.old_state = s->globals[bank];
    }
    if (size != 4 || offset % 0x1000 || bank >= 16) {
        event.detail = "unsupported_transfer_access";
    } else if (write && ((*value ^ s->globals[bank]) & ~UINT64_C(0x10000000))) {
        event.detail = "unsupported_transfer_global_write";
    } else if (write) {
        s->globals[bank] = *value;
    } else {
        event.value = s->globals[bank];
    }
    event.state = bank < 16 ? s->globals[bank] : 0;
    event.success = event.detail == NULL;
    if (!write) {
        *value = event.success ? event.value : 0;
    }
    if (s->callback) {
        s->callback(s->callback_opaque, &event);
    }
    return event.success ? MEMTX_OK : MEMTX_ERROR;
}

static MemTxResult transfer_read(void *opaque, hwaddr offset, uint64_t *value,
                                 unsigned size, MemTxAttrs attrs)
{
    return transfer_access(opaque, offset, value, size, false);
}

static MemTxResult transfer_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size, MemTxAttrs attrs)
{
    return transfer_access(opaque, offset, &value, size, true);
}

static const MemoryRegionOps transfer_ops = {
    .read_with_attrs = transfer_read, .write_with_attrs = transfer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

void xt4_transfer_set_initial(XT4TransferState *s, const uint32_t words[16])
{
    memcpy(s->initial, words, sizeof(s->initial));
}

void xt4_transfer_set_event_callback(XT4TransferState *s, XT4TransferEventFn cb,
                                      void *opaque)
{
    s->callback = cb;
    s->callback_opaque = opaque;
}

static void transfer_reset(Object *obj, ResetType type)
{
    XT4TransferState *s = XT4_TRANSFER(obj);
    memcpy(s->globals, s->initial, sizeof(s->globals));
}

static void transfer_init(Object *obj)
{
    XT4TransferState *s = XT4_TRANSFER(obj);
    memory_region_init_io(&s->mmio, obj, &transfer_ops, s, TYPE_XT4_TRANSFER, 0x10000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
}

static void transfer_class_init(ObjectClass *klass, const void *data)
{
    DEVICE_CLASS(klass)->user_creatable = false;
    RESETTABLE_CLASS(klass)->phases.hold = transfer_reset;
}

static const TypeInfo transfer_type = {
    .name = TYPE_XT4_TRANSFER, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(XT4TransferState), .instance_init = transfer_init,
    .class_init = transfer_class_init,
};

static void transfer_register_types(void)
{
    type_register_static(&transfer_type);
}
type_init(transfer_register_types)
