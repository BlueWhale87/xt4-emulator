/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_GPIO_XT4_PORTS_H
#define HW_GPIO_XT4_PORTS_H

#include "hw/core/sysbus.h"

#define TYPE_XT4_PORT_CONFIG "xt4-port-config"
#define XT4_PORT_CONFIG_MODEL "xt4-port-config-v1"
#define XT4_STATIC_PORT_INPUT_MODEL "xt4-static-port-input-v1"
OBJECT_DECLARE_SIMPLE_TYPE(XT4PortsState, XT4_PORT_CONFIG)

typedef struct XT4PortEvent {
    hwaddr address;
    unsigned width, port, selector;
    bool write, global, success;
    uint64_t value;
    uint32_t state, known_mask;
    const char *detail;
    const char *model, *semantics;
} XT4PortEvent;

typedef void (*XT4PortEventFn)(void *opaque, const XT4PortEvent *event);

void xt4_ports_set_event_callback(XT4PortsState *s, XT4PortEventFn callback,
                                 void *opaque);
void xt4_ports_set_inputs(XT4PortsState *s, const uint32_t values[16]);

#endif
