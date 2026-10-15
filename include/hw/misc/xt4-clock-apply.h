/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_XT4_CLOCK_APPLY_H
#define HW_MISC_XT4_CLOCK_APPLY_H
#include "hw/core/sysbus.h"

#define TYPE_XT4_CLOCK_APPLY "xt4-clock-apply-exploration"
#define XT4_CLOCK_APPLY_MODEL "xt4-clock-apply-exploration-v1"
OBJECT_DECLARE_SIMPLE_TYPE(XT4ClockApplyState, XT4_CLOCK_APPLY)

typedef struct XT4ClockApplyEvent {
    hwaddr address;
    unsigned width, shadow_known, active_known, staged;
    uint64_t value;
    uint32_t shadow[2], active[2], command;
    int64_t virtual_ns, deadline_ns;
    bool write, transition, success, busy, command_known;
    const char *detail;
} XT4ClockApplyEvent;
typedef void (*XT4ClockApplyEventFn)(void *opaque, const XT4ClockApplyEvent *event);
void xt4_clock_apply_set_latency(XT4ClockApplyState *s, uint64_t latency_ns);
void xt4_clock_apply_set_event_callback(XT4ClockApplyState *s,
                                       XT4ClockApplyEventFn cb, void *opaque);
#endif
