/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_INTC_XT4_GIC_H
#define HW_INTC_XT4_GIC_H
#include "hw/core/sysbus.h"

#define TYPE_XT4_GIC "xt4-gic-core0-exploration"
#define XT4_GIC_MODEL "xt4-gicv2-core0-exploration-v1"
OBJECT_DECLARE_SIMPLE_TYPE(XT4GicState, XT4_GIC)

typedef struct XT4GicEvent {
    hwaddr address;
    uint64_t value;
    unsigned width;
    bool write, success, transition, irq_level;
    const char *detail;
} XT4GicEvent;
typedef void (*XT4GicEventFn)(void *, const XT4GicEvent *);
void xt4_gic_set_event_callback(XT4GicState *, XT4GicEventFn, void *);
#endif
