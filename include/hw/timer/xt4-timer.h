/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_TIMER_XT4_TIMER_H
#define HW_TIMER_XT4_TIMER_H

#include "hw/core/sysbus.h"

#define TYPE_XT4_TIMER_EXPLORATION "xt4-timer-exploration"
#define XT4_TIMER_MODEL "xt4-timer-exploration-v1"
OBJECT_DECLARE_SIMPLE_TYPE(XT4TimerState, XT4_TIMER_EXPLORATION)

typedef struct XT4TimerEvent {
    hwaddr address;
    int channel;
    unsigned width;
    uint64_t value;
    int64_t virtual_ns, deadline_ns;
    uint32_t current, adjacent;
    bool transition, write, success;
    bool current_known, adjacent_known, enabled, terminal_unknown;
    const char *detail;
} XT4TimerEvent;

typedef void (*XT4TimerEventFn)(void *opaque, const XT4TimerEvent *event);
void xt4_timer_set_event_callback(XT4TimerState *s, XT4TimerEventFn callback,
                                 void *opaque);
#endif
