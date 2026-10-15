/* SPDX-License-Identifier: GPL-2.0-or-later
 * Bounded virtual request/DMA/indirect path. No physical backend or generic DMA.
 */
#include "qemu/osdep.h"
#include "hw/dma/xt4-calibration-transfer.h"
#include "hw/core/resettable.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"

typedef struct CalibrationWindow {
    MemoryRegion mmio;
    XT4CalibrationTransferState *owner;
    XT4CalibrationComponent component;
    hwaddr base, size;
} CalibrationWindow;

struct XT4CalibrationTransferState {
    SysBusDevice parent_obj;
    CalibrationWindow windows[3];
    QEMUTimer *timer;
    XT4CalibrationTransferContract contract;
    XT4FlashState *flash;
    XT4PhyState *phy;
    XT4TransferState *globals;
    MemoryRegion *ram;
    uint64_t ram_base, ram_size;
    XT4CoveredNor media;
    uint32_t route, dma[7], indirect[3];
    uint8_t dma_known, indirect_known;
    bool route_known, busy, done;
    uint64_t epoch, pending_epoch, phy_epoch, global_epoch;
    XT4FlashArraySnapshot flash_snapshot;
    uint8_t pending_bytes[128];
    char *source_hash;
    int64_t started_ns, deadline_ns;
    XT4CalibrationTransferEventFn callback;
    void *callback_opaque;
    XT4CalibrationTransferGuard guard;
    void *guard_opaque;
};

static void notify(XT4CalibrationTransferState *s, XT4CalibrationTransferEvent *e)
{
    e->success = !e->detail;
    e->route_known = s->route_known;
    e->route = s->route;
    e->busy = e->active = s->busy;
    e->done = s->done;
    e->epoch = s->epoch;
    e->source = s->dma[1];
    e->destination = s->dma[2];
    e->count = s->dma[0];
    e->source_attr = s->dma[3];
    e->destination_attr = s->dma[4];
    e->descriptor = s->dma[5];
    e->control = s->dma[6];
    e->deadline_ns = s->deadline_ns;
    e->started_ns = s->started_ns;
    e->phase = s->flash_snapshot.controller_phase;
    if (e->transition && e->success) {
        e->source_sha256 = s->source_hash;
        e->source_length = sizeof(s->pending_bytes);
    }
    if (s->callback) {
        s->callback(s->callback_opaque, e);
    }
}

bool xt4_calibration_transfer_busy(XT4CalibrationTransferState *s)
{
    return s && s->busy;
}

void xt4_calibration_transfer_cancel_pending(XT4CalibrationTransferState *s)
{
    timer_del(s->timer);
    if (s->busy) {
        s->busy = s->done = false;
        s->deadline_ns = -1;
        s->epoch++;
        memset(s->pending_bytes, 0, sizeof(s->pending_bytes));
    }
}

static const char *check_request(XT4CalibrationTransferState *s,
                                 XT4FlashArraySnapshot *flash,
                                 uint64_t *phy_epoch, uint8_t bytes[128])
{
    if (!s->route_known || s->route != 0x01002a2c) {
        return "inactive_request_route";
    }
    if (!xt4_transfer_global_enabled(s->globals, 1)) {
        return "disabled_transfer_global";
    }
    if (s->dma_known != 0x7f) {
        return "unknown_dma_descriptor";
    }
    if (s->indirect_known != 7) {
        return "unknown_indirect_parameters";
    }
    if (s->dma[0] != 127 || s->dma[3] != 0x30f04 ||
        s->dma[4] != 0x30f00 || s->dma[5] || s->dma[6] != 0x10201011 ||
        s->indirect[0] || s->indirect[2] != sizeof(s->pending_bytes) ||
        s->dma[1] != s->indirect[1] || (s->dma[1] & 1)) {
        return "unsupported_calibration_descriptor";
    }
    const char *detail = NULL;
    if (!xt4_flash_array_snapshot(s->flash, flash, &detail)) {
        return detail;
    }
    if (flash->trigger != s->dma[1] || flash->partition != 0x707 ||
        flash->range != 0xf) {
        return "unsupported_calibration_flash_range";
    }
    if (!xt4_phy_array_ready(s->phy, phy_epoch)) {
        return "unready_calibration_phy";
    }
    uint64_t dest = s->dma[2];
    if (dest < s->ram_base || dest - s->ram_base > s->ram_size ||
        sizeof(s->pending_bytes) > s->ram_size - (dest - s->ram_base)) {
        return "unsupported_calibration_destination";
    }
    if (!xt4_nor_covered_read(&s->media, s->dma[1], bytes,
                             sizeof(s->pending_bytes), &detail)) {
        return detail;
    }
    return NULL;
}

static bool same_flash(const XT4FlashArraySnapshot *a,
                        const XT4FlashArraySnapshot *b)
{
    return a->config == b->config && a->read_instruction == b->read_instruction &&
        a->size == b->size && a->capture == b->capture &&
        a->partition == b->partition && a->trigger == b->trigger &&
        a->range == b->range && a->extensions == b->extensions &&
        a->variant == b->variant && a->nor_generation == b->nor_generation &&
        a->protocol == b->protocol && a->controller_phase == b->controller_phase;
}

static void complete(void *opaque)
{
    XT4CalibrationTransferState *s = opaque;
    if (!s->busy || (s->guard && !s->guard(s->guard_opaque))) {
        xt4_calibration_transfer_cancel_pending(s);
        return;
    }
    XT4FlashArraySnapshot flash;
    uint64_t phy_epoch;
    uint8_t bytes[128];
    XT4CalibrationTransferEvent e = {
        .component = XT4_CAL_INDIRECT, .address = 0xfffd8060,
        .transition = true, .virtual_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
    };
    e.detail = check_request(s, &flash, &phy_epoch, bytes);
    if (!e.detail && (s->pending_epoch != s->epoch || phy_epoch != s->phy_epoch ||
                      xt4_transfer_global_epoch(s->globals, 1) != s->global_epoch ||
                      !same_flash(&flash, &s->flash_snapshot) ||
                      memcmp(bytes, s->pending_bytes, sizeof(bytes)))) {
        e.detail = "changed_calibration_prerequisite";
    }
    if (e.detail) {
        xt4_calibration_transfer_cancel_pending(s);
        notify(s, &e);
        return;
    }
    /* The board links a single static RAM range; descriptor admission proves
     * the complete write is inside it. Use the normal DMA write path to retain
     * dirty tracking and translated-code invalidation. Never target MMIO. */
    if (address_space_write(&address_space_memory, s->dma[2],
                            MEMTXATTRS_UNSPECIFIED, s->pending_bytes,
                            sizeof(s->pending_bytes)) != MEMTX_OK) {
        e.detail = "calibration_ram_delivery_failed";
        xt4_calibration_transfer_cancel_pending(s);
        notify(s, &e);
        return;
    }
    s->busy = false;
    s->done = true;
    s->epoch++;
    for (unsigned i = 0; i < 3; i++) {
        e.component = i;
        e.address = s->windows[i].base;
        notify(s, &e);
    }
}

static const char *start_request(XT4CalibrationTransferState *s, int64_t now)
{
    if (s->done) {
        return "unacknowledged_indirect_done";
    }
    XT4FlashArraySnapshot flash;
    uint64_t phy_epoch;
    uint8_t bytes[128];
    const char *detail = check_request(s, &flash, &phy_epoch, bytes);
    if (detail) {
        return detail;
    }
    if (now < 0 || s->contract.transfer_latency_ns > INT64_MAX - now) {
        return "unsupported_calibration_deadline";
    }
    memcpy(s->pending_bytes, bytes, sizeof(bytes));
    s->flash_snapshot = flash;
    s->phy_epoch = phy_epoch;
    s->global_epoch = xt4_transfer_global_epoch(s->globals, 1);
    s->pending_epoch = s->epoch;
    s->started_ns = now;
    s->deadline_ns = now + s->contract.transfer_latency_ns;
    s->busy = true;
    timer_mod_ns(s->timer, s->deadline_ns);
    return NULL;
}

static MemTxResult access_path(CalibrationWindow *w, hwaddr off, uint64_t *value,
                               unsigned size, bool write)
{
    XT4CalibrationTransferState *s = w->owner;
    XT4CalibrationTransferEvent e = {
        .component = w->component, .address = w->base + off, .width = size,
        .write = write, .value = write ? *value : 0,
        .virtual_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
    };
    if (size != 4 || (off & 3) || off >= w->size ||
        (w->component == XT4_CAL_DMA && off == 0x1c)) {
        e.detail = "unsupported_transfer_path_access";
    } else if (write && s->busy) {
        e.detail = "busy_calibration_transfer";
    } else if (w->component == XT4_CAL_ROUTE) {
        if (write) {
            if (*value != 0x01002a2c && *value != 0x01002c2c) {
                e.detail = "unsupported_request_route";
            } else {
                s->route = *value;
                s->route_known = true;
                s->epoch++;
            }
        } else if (!s->route_known) {
            e.detail = "unknown_request_route";
        } else {
            e.value = s->route;
        }
    } else if (w->component == XT4_CAL_DMA) {
        unsigned index = off / 4;
        if (index == 8) {
            if (write && *value) {
                e.detail = "unsupported_dma_status_ack";
            } else {
                e.value = s->busy ? 0x10000 : 0;
            }
        } else if (write) {
            if (index == 6 && *value != 0x00201011 && *value != 0x10201011) {
                e.detail = "unsupported_dma_control";
            } else {
                s->dma[index] = *value;
                s->dma_known |= 1U << index;
                s->epoch++;
            }
        } else if (!(s->dma_known & (1U << index))) {
            e.detail = "unknown_dma_descriptor";
        } else {
            e.value = s->dma[index];
        }
    } else if (!off) {
        if (write) {
            if (*value == 1) {
                e.detail = start_request(s, e.virtual_ns);
            } else if (*value == 0x20) {
                s->done = false;
                s->epoch++;
            } else {
                e.detail = "unsupported_indirect_control";
            }
        } else {
            e.value = s->busy ? 1 : s->done ? 0x20 : 0;
        }
    } else {
        unsigned index = off / 4 - 1;
        if (write) {
            s->indirect[index] = *value;
            s->indirect_known |= 1U << index;
            s->epoch++;
        } else if (!(s->indirect_known & (1U << index))) {
            e.detail = "unknown_indirect_parameters";
        } else {
            e.value = s->indirect[index];
        }
    }
    notify(s, &e);
    if (!write) {
        *value = e.success ? e.value : 0;
    }
    return e.success ? MEMTX_OK : MEMTX_ERROR;
}

static MemTxResult path_read(void *opaque, hwaddr off, uint64_t *value,
                             unsigned size, MemTxAttrs attrs)
{
    return access_path(opaque, off, value, size, false);
}
static MemTxResult path_write(void *opaque, hwaddr off, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    return access_path(opaque, off, &value, size, true);
}
static const MemoryRegionOps path_ops = {
    .read_with_attrs = path_read, .write_with_attrs = path_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

void xt4_calibration_transfer_set_contract(XT4CalibrationTransferState *s,
                                          const XT4CalibrationTransferContract *c)
{
    g_assert_cmpuint(c->initial_status_word, ==, 0);
    g_assert_cmpuint(c->initial_indirect_control, ==, 0);
    g_assert_cmpuint(c->transfer_latency_ns, ==, 64);
    g_assert_cmpuint(c->cbar_base, ==, 0xf8f00000);
    g_assert_cmpstr(c->route_policy, ==, XT4_ROUTE_POLICY);
    g_assert_cmpstr(c->source_policy, ==, XT4_SOURCE_POLICY);
    g_assert_cmpstr(c->sample_policy, ==, XT4_SAMPLE_POLICY);
    g_assert_cmpstr(c->status_policy, ==, XT4_STATUS_POLICY);
    g_assert_cmpstr(c->terminal_address_policy, ==, XT4_ADDRESS_POLICY);
    s->contract = *c;
}
void xt4_calibration_transfer_set_links(XT4CalibrationTransferState *s,
        XT4FlashState *flash, XT4PhyState *phy, XT4TransferState *globals,
        MemoryRegion *ram, uint64_t ram_base, uint64_t ram_size,
        const XT4CoveredNor *media)
{
    g_assert(flash && phy && globals && ram && media);
    g_assert(memory_region_is_ram(ram));
    g_assert_cmpuint(memory_region_size(ram), ==, ram_size);
    g_assert(ram_size >= 128 && ram_base <= UINT32_MAX &&
             ram_size <= UINT64_C(0x100000000) - ram_base);
    s->flash = flash; s->phy = phy; s->globals = globals;
    s->ram = ram; s->ram_base = ram_base; s->ram_size = ram_size;
    s->media = *media;
    g_free(s->source_hash);
    s->source_hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                media->bytes, media->length);
}
void xt4_calibration_transfer_set_event_callback(XT4CalibrationTransferState *s,
                                                 XT4CalibrationTransferEventFn cb,
                                                 void *opaque)
{
    s->callback = cb; s->callback_opaque = opaque;
}
void xt4_calibration_transfer_set_completion_guard(XT4CalibrationTransferState *s,
                                                  XT4CalibrationTransferGuard cb,
                                                  void *opaque)
{
    s->guard = cb; s->guard_opaque = opaque;
}
static void path_reset(Object *obj, ResetType type)
{
    XT4CalibrationTransferState *s = XT4_CALIBRATION_TRANSFER(obj);
    xt4_calibration_transfer_cancel_pending(s);
    s->route = s->dma_known = s->indirect_known = 0;
    s->route_known = s->busy = s->done = false;
    memset(s->dma, 0, sizeof(s->dma));
    memset(s->indirect, 0, sizeof(s->indirect));
    memset(&s->flash_snapshot, 0, sizeof(s->flash_snapshot));
    memset(s->pending_bytes, 0, sizeof(s->pending_bytes));
    s->started_ns = s->deadline_ns = -1;
    s->epoch++;
}
static void path_init(Object *obj)
{
    XT4CalibrationTransferState *s = XT4_CALIBRATION_TRANSFER(obj);
    const hwaddr bases[] = {0xfff4a500, 0xfffe1010, 0xfffd8060};
    const hwaddr sizes[] = {4, 0x24, 0x10};
    s->timer = timer_new_full(NULL, QEMU_CLOCK_VIRTUAL, SCALE_NS,
                             QEMU_TIMER_ATTR_EXTERNAL, complete, s);
    for (unsigned i = 0; i < 3; i++) {
        CalibrationWindow *w = &s->windows[i];
        w->owner = s; w->component = i; w->base = bases[i]; w->size = sizes[i];
        memory_region_init_io(&w->mmio, obj, &path_ops, w,
                               TYPE_XT4_CALIBRATION_TRANSFER, sizes[i]);
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &w->mmio);
    }
}
static void path_finalize(Object *obj)
{
    XT4CalibrationTransferState *s = XT4_CALIBRATION_TRANSFER(obj);
    timer_del(s->timer); timer_free(s->timer); g_free(s->source_hash);
}
static void path_class_init(ObjectClass *klass, const void *data)
{
    DEVICE_CLASS(klass)->user_creatable = false;
    RESETTABLE_CLASS(klass)->phases.hold = path_reset;
}
static const TypeInfo path_type = {
    .name = TYPE_XT4_CALIBRATION_TRANSFER, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(XT4CalibrationTransferState), .instance_init = path_init,
    .instance_finalize = path_finalize, .class_init = path_class_init,
};
static void path_register_types(void)
{
    type_register_static(&path_type);
}
type_init(path_register_types)
