/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ARM_XT4_H
#define HW_ARM_XT4_H

#include "hw/core/boards.h"
#include "hw/gpio/xt4-ports.h"
#include "hw/timer/xt4-timer.h"
#include "hw/dma/xt4-transfer.h"
#include "hw/misc/xt4-clock-apply.h"
#include "hw/ssi/xt4-flash.h"
#include "target/arm/cpu.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "qemu/timer.h"

#define TYPE_XT4_MACHINE MACHINE_TYPE_NAME("fujifilm-xt4")
#define TYPE_XT4_CPU ARM_CPU_TYPE_NAME("xt4-cortex-a7")
OBJECT_DECLARE_SIMPLE_TYPE(XT4State, XT4_MACHINE)
OBJECT_DECLARE_SIMPLE_TYPE(XT4CPU, XT4_CPU)

typedef struct XT4Stop {
    const char *reason;
    const char *access;
    const char *device, *detail;
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
    XT4PortsState *ports;
    XT4TimerState *timer;
    XT4TransferState *transfer;
    XT4ClockApplyState *clock_apply;
    XT4FlashState *flash;
    XT4PhyState *phy;
    XT4PhyContract phy_contract;
    XT4FlashContract flash_contract;
    const char *pending_device, *pending_detail;
    char *run_dir;
    char *scenario_id, *scenario_sha256;
    uint32_t port_inputs[16];
    uint32_t transfer_globals[16];
    uint64_t clock_apply_latency_ns;
    const XT4ProfileDefinition *profile;
    MemoryRegion ram[6], unknown;
    uint8_t *images[6];
    size_t image_sizes[6];
    FILE *events;
    bool stopped, pending_write;
    uint64_t pending_value;
    uint64_t pending_address;
    unsigned pending_width;
    uint64_t instruction_budget;
    int64_t instruction_baseline;
    uint64_t execution_sequence;
    QEMUTimer *budget_timer;
    VMChangeStateEntry *execution_handler;
};

void xt4_record_stop(XT4State *s, CPUState *cpu, const XT4Stop *stop);

#endif
