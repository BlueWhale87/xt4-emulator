/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_DMA_XT4_TRANSFER_H
#define HW_DMA_XT4_TRANSFER_H
#include "hw/core/sysbus.h"

#define TYPE_XT4_TRANSFER "xt4-transfer-enable-exploration"
#define XT4_TRANSFER_MODEL "xt4-transfer-enable-exploration-v1"
OBJECT_DECLARE_SIMPLE_TYPE(XT4TransferState, XT4_TRANSFER)

typedef struct XT4TransferEvent {
    hwaddr address;
    unsigned width, bank;
    uint64_t value;
    uint32_t old_state, state;
    bool write, success;
    const char *detail;
} XT4TransferEvent;
typedef void (*XT4TransferEventFn)(void *opaque, const XT4TransferEvent *event);
void xt4_transfer_set_initial(XT4TransferState *s, const uint32_t words[16]);
bool xt4_transfer_global_enabled(XT4TransferState *, unsigned bank);
uint64_t xt4_transfer_global_epoch(XT4TransferState *, unsigned bank);
void xt4_transfer_set_event_callback(XT4TransferState *s, XT4TransferEventFn cb,
                                      void *opaque);
#endif
