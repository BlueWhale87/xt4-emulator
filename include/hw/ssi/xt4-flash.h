/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SSI_XT4_FLASH_H
#define HW_SSI_XT4_FLASH_H
#include "hw/core/sysbus.h"
#include "hw/block/xt4-nor-boot.h"
#include "hw/ssi/xt4-phy.h"

#define TYPE_XT4_FLASH "xt4-cadence-stig-exploration"
#define XT4_FLASH_MODEL "xt4-cadence-stig-exploration-v1"
#define XT4_NOR_MODEL "mx25um51245gxdi00-boot-v1"
#define XT4_FLASH_PROTOCOL_POLICY "dual-opcode-8d-exploration-v1"
#define XT4_NOR_MODEL_2C "selector2c-spi-boot-v1"
#define XT4_FLASH_PROTOCOL_POLICY_2C "spi-selector2c-exploration-v1"
OBJECT_DECLARE_SIMPLE_TYPE(XT4FlashState, XT4_FLASH)
typedef struct XT4CalibrationTransferState XT4CalibrationTransferState;

typedef struct XT4FlashArraySnapshot {
    uint32_t config, read_instruction, size, capture, partition, trigger, range;
    uint32_t extensions, variant;
    uint64_t nor_generation;
    XT4NorProtocol protocol;
    unsigned controller_phase;
} XT4FlashArraySnapshot;

typedef struct XT4FlashContract {
    XT4NorPart part;
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
void xt4_flash_set_phy(XT4FlashState *, XT4PhyState *);
void xt4_flash_set_calibration_transfer(XT4FlashState *, XT4CalibrationTransferState *);
bool xt4_flash_array_snapshot(XT4FlashState *, XT4FlashArraySnapshot *, const char **);
void xt4_flash_cancel_pending(XT4FlashState *);
#endif
