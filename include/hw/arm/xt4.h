/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ARM_XT4_H
#define HW_ARM_XT4_H

#include "hw/core/boards.h"
#include "target/arm/cpu.h"
#include "system/memory.h"

#define TYPE_XT4_MACHINE MACHINE_TYPE_NAME("fujifilm-xt4")
#define TYPE_XT4_CPU ARM_CPU_TYPE_NAME("xt4-cortex-a7")
OBJECT_DECLARE_SIMPLE_TYPE(XT4State, XT4_MACHINE)
OBJECT_DECLARE_SIMPLE_TYPE(XT4CPU, XT4_CPU)

typedef struct XT4Stop {
    const char *reason;
    const char *access;
    uint64_t virtual_address, physical_address, value;
    unsigned width;
    int exception;
    bool has_virtual, has_physical, has_value;
    bool has_origin_pc;
    uint32_t origin_pc;
} XT4Stop;

struct XT4CPU {
    ARMCPU parent_obj;
    XT4State *board;
};

typedef struct XT4ProfileDefinition XT4ProfileDefinition;

struct XT4State {
    MachineState parent_obj;
    XT4CPU *cpu;
    char *run_dir;
    const XT4ProfileDefinition *profile;
    MemoryRegion ram[6], unknown;
    uint8_t *images[6];
    size_t image_sizes[6];
    FILE *events;
    bool stopped, pending_write;
    uint64_t pending_value;
    uint64_t pending_address;
    unsigned pending_width;
};

void xt4_record_stop(XT4State *s, CPUState *cpu, const XT4Stop *stop);

#endif
