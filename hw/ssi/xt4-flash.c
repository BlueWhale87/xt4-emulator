/* SPDX-License-Identifier: GPL-2.0-or-later
 * Strict synthetic Cadence STIG adapter and named virtual NOR boot subset.
 * No array backend, host I/O, DMA, analog PHY, or CPU IRQ routing.
 */
#include "qemu/osdep.h"
#include "hw/ssi/xt4-flash.h"
#include "hw/dma/xt4-calibration-transfer.h"
#include "hw/core/resettable.h"
#include "qemu/module.h"
#include "qemu/timer.h"

struct XT4FlashState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    QEMUTimer *timer;
    XT4FlashContract contract;
    XT4PhyState *phy;
    XT4CalibrationTransferState *calibration;
    uint32_t reg[64];
    uint64_t known;
    bool busy;
    unsigned data_len;
    int64_t deadline_ns;
    XT4NorState nor;
    XT4NorRequest request;
    XT4NorEffect effect;
    XT4FlashEventFn callback;
    void *callback_opaque;
    XT4FlashCompletionGuard guard;
    void *guard_opaque;
};

#define REG(s, off) ((s)->reg[(off) / 4])
#define KNOWN(off) (UINT64_C(1) << ((off) / 4))
#define READY (1U << 14)
#define CONFIG_MASK ((1U << 30) | (1U << 24) | (0xfU << 19) | \
                     (0xfU << 10) | (1U << 7) | (1U << 6) | 1)
#define COMMAND_MASK (0xff000000U | (1U << 23) | (7U << 20) | \
                      (1U << 19) | (3U << 16) | (1U << 15) | \
                      (7U << 12) | (31U << 7) | 1)

static void notify(XT4FlashState *s, XT4FlashEvent *e)
{
    e->success = e->detail == NULL;
    e->busy = s->busy;
    e->status = REG(s, 0x40);
    e->enable = REG(s, 0x44);
    e->irq = (e->status & e->enable) != 0;
    e->command = REG(s, 0x90) | (s->busy ? 2 : 0);
    e->deadline_ns = s->deadline_ns;
    e->nor = s->nor;
    if (s->callback) {
        s->callback(s->callback_opaque, e);
    }
}

void xt4_flash_cancel_pending(XT4FlashState *s)
{
    timer_del(s->timer);
    s->busy = false;
    s->data_len = 0;
    s->deadline_ns = -1;
    memset(&s->effect, 0, sizeof(s->effect));
}

static void flash_complete(void *opaque)
{
    XT4FlashState *s = opaque;
    if (!s->busy || (s->guard && !s->guard(s->guard_opaque))) {
        xt4_flash_cancel_pending(s);
        return;
    }
    XT4FlashEvent e = {
        .address = 0xfffd8090, .transition = true,
        .virtual_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        .request = s->request, .ignored_write = s->effect.ignored_write,
    };
    if (!xt4_nor_boot_commit(&s->nor, &s->effect, &e.detail)) {
        xt4_flash_cancel_pending(s);
        notify(s, &e);
        return;
    }
    s->data_len = s->effect.rx_len;
    REG(s, 0xa0) = REG(s, 0xa4) = 0;
    for (int i = 0; i < s->data_len; i++) {
        REG(s, i < 4 ? 0xa0 : 0xa4) |= (uint32_t)s->effect.rx[i] << ((i % 4) * 8);
    }
    s->busy = false;
    if (REG(s, 0x44) & READY) {
        REG(s, 0x40) |= READY;
    }
    notify(s, &e);
}

static bool decode_request(XT4FlashState *s, uint32_t command,
                           XT4NorRequest *r, const char **detail)
{
    bool read = command & (1U << 23), write = command & (1U << 15);
    bool address = command & (1U << 19);
    if ((command & ~COMMAND_MASK) || (read && write) ||
        (!read && (command & (7U << 20))) ||
        (!write && (command & (7U << 12))) ||
        (!address && (command & (3U << 16)))) {
        *detail = "unsupported_flash_command_fields";
        return false;
    }
    *r = (XT4NorRequest) {
        .protocol = REG(s, 0) & (1U << 30) ? XT4_NOR_OPI_DTR : XT4_NOR_SPI,
        .opcode = command >> 24, .extension = REG(s, 0xe0),
        .has_extension = (REG(s, 0) & (1U << 30)) != 0,
        .address_bytes = address ? ((command >> 16) & 3) + 1 : 0,
        .dummy_cycles = (command >> 7) & 31,
        .rx_len = read ? ((command >> 20) & 7) + 1 : 0,
        .tx_len = write ? ((command >> 12) & 7) + 1 : 0,
    };
    if (address && !(s->known & KNOWN(0x94))) {
        *detail = "unknown_flash_address";
        return false;
    }
    r->address = address ? REG(s, 0x94) : 0;
    if ((r->tx_len && !(s->known & KNOWN(0xa8))) ||
        (r->tx_len > 4 && !(s->known & KNOWN(0xac)))) {
        *detail = "unknown_flash_write_data";
        return false;
    }
    for (int i = 0; i < r->tx_len; i++) {
        r->tx[i] = REG(s, i < 4 ? 0xa8 : 0xac) >> ((i % 4) * 8);
    }
    return true;
}

static const char *write_register(XT4FlashState *s, hwaddr off, uint32_t value,
                                   XT4FlashEvent *e)
{
    if (xt4_calibration_transfer_busy(s->calibration)) {
        return "busy_calibration_transfer";
    }
    if (s->busy) {
        return "busy_flash_controller";
    }
    switch (off) {
    case 0:
        if ((value & ~(CONFIG_MASK | (1U << 31) | (s->phy ? 8U : 0) |
                       (s->calibration ? (1U << 15) : 0))) ||
            (value & (0xfU << 10))) {
            return "unsupported_flash_config";
        }
        if (s->phy) {
            const char *detail = NULL;
            if ((value & 8) && !(value & 1)) {
                return "disabled_phy_controller";
            }
            if (!xt4_phy_set_controller_config(s->phy, (value & 9) == 9, &detail)) {
                return detail;
            }
        }
        value &= ~(1U << 31);
        break;
    case 4: case 8: case 0xc: case 0x10: case 0x14: case 0x20:
    case 0x94: case 0xa8: case 0xac:
        break;
    case 0x1c: case 0x80:
        if (!s->calibration) {
            return "unsupported_flash_register";
        }
        break;
    case 0xb4:
        if (!(s->known & KNOWN(0))) {
            return "unknown_flash_config";
        }
        /* This profile rejects enabling PHY in Config, so retaining its
         * configuration word cannot imply analog lock or training. */
        break;
    case 0x38:
        if ((value & ~0x40ffU) != (s->contract.initial_write_completion & ~0x40ffU)) {
            return "unsupported_flash_write_completion";
        }
        break;
    case 0x40: case 0x44:
        if (value & ~READY) {
            return "unsupported_flash_irq_bits";
        }
        if (off == 0x40) {
            value = REG(s, off) & ~value;
        }
        break;
    case 0x90: {
        XT4NorRequest request;
        XT4NorEffect effect;
        const char *detail = NULL;
        if (!decode_request(s, value, &request, &detail)) {
            return detail;
        }
        if (value & 1) {
            if (!(s->known & KNOWN(0))) {
                return "unknown_flash_config";
            }
            if (!(REG(s, 0) & 1)) {
                return "disabled_flash_controller";
            }
            if (!xt4_nor_boot_prepare(&s->nor, &request, &effect, &detail)) {
                return detail;
            }
            uint64_t delay = s->contract.transaction_latency_ns + effect.settle_ns;
            if (e->virtual_ns < 0 || delay > INT64_MAX - e->virtual_ns) {
                return "unsupported_flash_deadline";
            }
            s->request = request;
            s->effect = effect;
            s->busy = true;
            s->data_len = 0;
            s->deadline_ns = e->virtual_ns + delay;
            timer_mod_ns(s->timer, s->deadline_ns);
        }
        e->request = request;
        value &= ~1U;
        break;
    }
    case 0xe0:
        if (s->contract.part == XT4_NOR_PART_SELECTOR2C ?
            ((value & 0xffffff00U) != 0x13edfa00 && value != 0xfa00) :
            ((value & 0xffffff00U) != 0x13edfa00 &&
             (value & 0xffffff00U) != 0x11edfa00)) {
            return "unsupported_flash_extensions";
        }
        break;
    case 0xf0:
        if ((value & ~0x10U) != (s->contract.initial_variant_word & ~0x10U)) {
            return "unsupported_flash_variant";
        }
        break;
    default:
        return "unsupported_flash_register";
    }
    REG(s, off) = value;
    s->known |= KNOWN(off);
    return NULL;
}

static MemTxResult flash_access(XT4FlashState *s, hwaddr off, uint64_t *value,
                                unsigned size, bool write)
{
    XT4FlashEvent e = {
        .address = 0xfffd8000 + off, .width = size, .write = write,
        .value = write ? *value : 0,
        .virtual_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
    };
    if (size != 4 || (off & 3) || off >= 0x100) {
        e.detail = "unsupported_flash_access";
    } else if (write) {
        e.detail = write_register(s, off, *value, &e);
    } else if (off == 0xa0 || off == 0xa4) {
        if (s->data_len <= (off == 0xa0 ? 0 : 4)) {
            e.detail = "unavailable_flash_data";
        } else {
            e.value = REG(s, off);
        }
    } else if (!(s->known & KNOWN(off))) {
        e.detail = "unknown_flash_register";
    } else {
        e.value = REG(s, off);
        if (off == 0) {
            e.value |= (s->busy || xt4_calibration_transfer_busy(s->calibration)) ?
                       0 : (1U << 31);
        } else if (off == 0x90) {
            e.value |= s->busy ? 2 : 0;
        }
    }
    notify(s, &e);
    if (!write) {
        *value = e.success ? e.value : 0;
    }
    return e.success ? MEMTX_OK : MEMTX_ERROR;
}

static MemTxResult flash_read(void *opaque, hwaddr off, uint64_t *value,
                              unsigned size, MemTxAttrs attrs)
{
    return flash_access(opaque, off, value, size, false);
}

static MemTxResult flash_write(void *opaque, hwaddr off, uint64_t value,
                               unsigned size, MemTxAttrs attrs)
{
    return flash_access(opaque, off, &value, size, true);
}

static const MemoryRegionOps flash_ops = {
    .read_with_attrs = flash_read, .write_with_attrs = flash_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

void xt4_flash_set_phy(XT4FlashState *s, XT4PhyState *phy)
{
    s->phy = phy;
}

void xt4_flash_set_calibration_transfer(XT4FlashState *s,
                                        XT4CalibrationTransferState *path)
{
    s->calibration = path;
}

bool xt4_flash_array_snapshot(XT4FlashState *s, XT4FlashArraySnapshot *out,
                              const char **detail)
{
    const uint64_t required = KNOWN(0) | KNOWN(4) | KNOWN(0x10) |
        KNOWN(0x14) | KNOWN(0x1c) | KNOWN(0x20) | KNOWN(0x80) |
        KNOWN(0xe0) | KNOWN(0xf0);
    const char *why = NULL;
    if (!s || !out || !s->calibration || s->busy ||
        (s->known & required) != required) {
        why = "unknown_calibration_flash_state";
    } else if (s->contract.part == XT4_NOR_PART_SELECTOR2C) {
        if ((REG(s, 0) & 0x41008009) != 0x8009 ||
            (REG(s, 4) != 0x100334fd && REG(s, 4) != 0x110334fd) ||
            REG(s, 0x14) != 0x111003 || REG(s, 0x10) != 1 ||
            REG(s, 0xe0) != 0xfa00 || REG(s, 0xf0) ||
            s->nor.protocol != XT4_NOR_SPI) {
            why = "unsupported_calibration_flash_state";
        }
    } else if ((REG(s, 0) & 0x41008009) != 0x41008009 ||
               (REG(s, 4) != 0x140333ee && REG(s, 4) != 0x150333ee) ||
               REG(s, 0x14) != 0x101003 || REG(s, 0x10) != 1 ||
               (REG(s, 0xe0) >> 24) != 0x11 || REG(s, 0xf0) != 0x10 ||
               s->nor.protocol != XT4_NOR_OPI_DTR) {
        why = "unsupported_calibration_flash_state";
    }
    if (detail) {
        *detail = why;
    }
    if (why) {
        return false;
    }
    *out = (XT4FlashArraySnapshot) {
        .config = REG(s, 0), .read_instruction = REG(s, 4),
        .size = REG(s, 0x14), .capture = REG(s, 0x10),
        .partition = REG(s, 0x20), .trigger = REG(s, 0x1c),
        .range = REG(s, 0x80), .extensions = REG(s, 0xe0),
        .variant = REG(s, 0xf0), .nor_generation = s->nor.generation,
        .protocol = s->nor.protocol, .controller_phase = REG(s, 4) >> 24,
    };
    return true;
}

void xt4_flash_set_contract(XT4FlashState *s, const XT4FlashContract *c)
{
    g_assert_cmpuint(c->transaction_latency_ns, ==, 64);
    g_assert(c->part == XT4_NOR_PART_MX25 || c->part == XT4_NOR_PART_SELECTOR2C);
    bool selector2c = c->part == XT4_NOR_PART_SELECTOR2C;
    g_assert_cmpuint(c->cr2_typical_settle_ns, ==, selector2c ? 0 : 40);
    g_assert_cmpuint(c->initial_command_word, ==, 0);
    g_assert_cmpuint(c->initial_opcode_extensions, ==, 0x13edfa00);
    g_assert_cmpuint(c->initial_write_completion, ==, 0x10005);
    g_assert_cmpuint(c->initial_variant_word, ==, 0);
    g_assert_cmpstr(c->stig_protocol_policy, ==, selector2c ?
                    XT4_FLASH_PROTOCOL_POLICY_2C : XT4_FLASH_PROTOCOL_POLICY);
    s->contract = *c;
    s->nor.part = c->part;
}

void xt4_flash_set_event_callback(XT4FlashState *s, XT4FlashEventFn cb, void *opaque)
{
    s->callback = cb;
    s->callback_opaque = opaque;
}

void xt4_flash_set_completion_guard(XT4FlashState *s, XT4FlashCompletionGuard cb,
                                    void *opaque)
{
    s->guard = cb;
    s->guard_opaque = opaque;
}

static void flash_reset(Object *obj, ResetType type)
{
    XT4FlashState *s = XT4_FLASH(obj);
    xt4_flash_cancel_pending(s);
    memset(s->reg, 0, sizeof(s->reg));
    s->known = KNOWN(0x38) | KNOWN(0x40) | KNOWN(0x44) |
               KNOWN(0x90) | KNOWN(0xe0) | KNOWN(0xf0);
    REG(s, 0x38) = s->contract.initial_write_completion;
    REG(s, 0x90) = s->contract.initial_command_word;
    REG(s, 0xe0) = s->contract.initial_opcode_extensions;
    REG(s, 0xf0) = s->contract.initial_variant_word;
    memset(&s->request, 0, sizeof(s->request));
    xt4_nor_boot_reset(&s->nor);
}

static void flash_init(Object *obj)
{
    XT4FlashState *s = XT4_FLASH(obj);
    s->timer = timer_new_full(NULL, QEMU_CLOCK_VIRTUAL, SCALE_NS,
                            QEMU_TIMER_ATTR_EXTERNAL, flash_complete, s);
    memory_region_init_io(&s->mmio, obj, &flash_ops, s, TYPE_XT4_FLASH, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
}

static void flash_finalize(Object *obj)
{
    XT4FlashState *s = XT4_FLASH(obj);
    timer_del(s->timer);
    timer_free(s->timer);
}

static void flash_class_init(ObjectClass *klass, const void *data)
{
    DEVICE_CLASS(klass)->user_creatable = false;
    RESETTABLE_CLASS(klass)->phases.hold = flash_reset;
}

static const TypeInfo flash_type = {
    .name = TYPE_XT4_FLASH, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(XT4FlashState), .instance_init = flash_init,
    .instance_finalize = flash_finalize, .class_init = flash_class_init,
};

static void flash_register_types(void)
{
    type_register_static(&flash_type);
}
type_init(flash_register_types)
