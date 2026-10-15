/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SSI_XT4_FLASH_H
#define HW_SSI_XT4_FLASH_H
#include "hw/core/sysbus.h"
#include "hw/block/xt4-nor-boot.h"

#define TYPE_XT4_FLASH "xt4-cadence-stig-exploration"
#define XT4_FLASH_MODEL "xt4-cadence-stig-exploration-v1"
#define XT4_NOR_MODEL "mx25um51245gxdi00-boot-v1"
#define XT4_FLASH_PROTOCOL_POLICY "dual-opcode-8d-exploration-v1"
OBJECT_DECLARE_SIMPLE_TYPE(XT4FlashState, XT4_FLASH)

typedef struct XT4FlashContract {
    uint64_t transaction_latency_ns, cr2_typical_settle_ns;
    uint32_t initial_command_word, initial_opcode_extensions;
    uint32_t initial_write_completion, initial_variant_word;
    const char *stig_protocol_policy;
} XT4FlashContract;

typedef struct XT4FlashEvent {
    hwaddr address;
    uint64_t value;
    unsigned width;
    int64_t virtual_ns, deadline_ns;
    bool write, transition, success, busy, irq, ignored_write;
    uint32_t status, enable, command;
    XT4NorRequest request;
    XT4NorState nor;
    const char *detail;
} XT4FlashEvent;
typedef void (*XT4FlashEventFn)(void *, const XT4FlashEvent *);
typedef bool (*XT4FlashCompletionGuard)(void *);
void xt4_flash_set_contract(XT4FlashState *, const XT4FlashContract *);
void xt4_flash_set_event_callback(XT4FlashState *, XT4FlashEventFn, void *);
void xt4_flash_set_completion_guard(XT4FlashState *, XT4FlashCompletionGuard, void *);
void xt4_flash_cancel_pending(XT4FlashState *);
#endif
