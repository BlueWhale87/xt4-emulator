/* SPDX-License-Identifier: GPL-2.0-or-later
 * Diagnostic observation, not an emulated Fujifilm peripheral.
 */
#include "qemu/osdep.h"
#include "hw/arm/xt4.h"
#include "accel/tcg/cpu-ops.h"
#include "accel/tcg/cpu-loop.h"
#include "qemu/module.h"
#include "system/runstate.h"

typedef struct XT4CPUClass {
    ARMCPUClass parent_class;
    TCGCPUOps ops;
} XT4CPUClass;

static void xt4_transaction_failed(CPUState *cs, hwaddr physical, vaddr virtual,
                                   unsigned size, MMUAccessType access,
                                   int mmu_idx, MemTxAttrs attrs,
                                   MemTxResult response, uintptr_t ra)
{
    XT4State *s = XT4_CPU(cs)->board;
    cpu_restore_state(cs, ra);
    XT4Stop stop = {
        .reason = "unsupported_access",
        .access = access == MMU_INST_FETCH ? "fetch" :
                  access == MMU_DATA_STORE ? "write" : "read",
        .virtual_address = virtual, .physical_address = physical,
        .has_virtual = true, .has_physical = true, .width = size,
        .has_value = access == MMU_DATA_STORE && s->pending_write &&
                     s->pending_address == physical,
        .value = s->pending_value,
    };
    xt4_record_stop(s, cs, &stop);
    /* A machine stop must also work without an initialized GDB server. */
    cs->stopped = true;
    qemu_system_debug_request();
    cs->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cs);
}

static void xt4_interrupt(CPUState *cs)
{
    ARMCPU *cpu = ARM_CPU(cs);
    XT4Stop stop = {
        .reason = "unexpected_exception", .access = "exception",
        .virtual_address = cpu->env.exception.vaddress,
        .has_virtual = cs->exception_index == EXCP_DATA_ABORT ||
                       cs->exception_index == EXCP_PREFETCH_ABORT,
        .exception = cs->exception_index,
    };
    /* SVC/HVC/SMC enter the dispatcher with the architectural return PC.
     * Report the trapping instruction separately; never change guest state.
     */
    if (cs->exception_index == EXCP_SWI || cs->exception_index == EXCP_HVC ||
        cs->exception_index == EXCP_SMC) {
        unsigned length = cs->exception_index == EXCP_SWI && cpu->env.thumb ? 2 : 4;
        stop.has_origin_pc = true;
        stop.origin_pc = cpu->env.regs[15] - length;
    }
    xt4_record_stop(XT4_CPU(cs)->board, cs, &stop);
    cs->stopped = true;
    qemu_system_debug_request();
    cs->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cs);
}

static void xt4_cpu_class_init(ObjectClass *oc, const void *data)
{
    CPUClass *cc = CPU_CLASS(oc);
    XT4CPUClass *xc = (XT4CPUClass *)oc;
    /* Parent table is shared by every ordinary Arm CPU. Never modify it. */
    xc->ops = *cc->tcg_ops;
    xc->ops.do_transaction_failed = xt4_transaction_failed;
    xc->ops.do_interrupt = xt4_interrupt;
    cc->tcg_ops = &xc->ops;
}

static const TypeInfo xt4_cpu_type = {
    .name = TYPE_XT4_CPU,
    .parent = ARM_CPU_TYPE_NAME("cortex-a7"),
    .instance_size = sizeof(XT4CPU),
    .class_size = sizeof(XT4CPUClass),
    .class_init = xt4_cpu_class_init,
};

static void xt4_cpu_register(void)
{
    type_register_static(&xt4_cpu_type);
}
type_init(xt4_cpu_register)
