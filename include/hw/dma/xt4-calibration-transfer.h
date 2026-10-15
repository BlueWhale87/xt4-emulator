/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_DMA_XT4_CALIBRATION_TRANSFER_H
#define HW_DMA_XT4_CALIBRATION_TRANSFER_H
#include "hw/core/sysbus.h"
#include "hw/ssi/xt4-flash.h"
#include "hw/dma/xt4-transfer.h"
#include "hw/block/xt4-nor-covered.h"

#define TYPE_XT4_CALIBRATION_TRANSFER "xt4-calibration-transfer-exploration"
#define XT4_ROUTE_MODEL "xt4-request-route-exploration-v1"
#define XT4_DMA_MODEL "xt4-dma-channel1-exploration-v1"
#define XT4_INDIRECT_MODEL "xt4-indirect-read-exploration-v1"
#define XT4_MEDIA_MODEL "xt4-covered-nor-media-v1"
#define XT4_ROUTE_POLICY "request2a-channel1-v1"
#define XT4_SOURCE_POLICY "f0-covered-span-v1"
#define XT4_SAMPLE_POLICY "ideal-taps0-127-phases20-21-v1"
#define XT4_SAMPLE_POLICY_2C "ideal-taps0-127-phases16-17-v1"
#define XT4_STATUS_POLICY "request-active-bit16-v1"
#define XT4_ADDRESS_POLICY "retain-programmed-v1"
#define XT4_CALIBRATION_SHA256 "10fa7918107458a1b72e18ef91b9bfc27b6eb76e3b562aa6566c5561dcf4929a"
OBJECT_DECLARE_SIMPLE_TYPE(XT4CalibrationTransferState, XT4_CALIBRATION_TRANSFER)

typedef struct XT4CalibrationTransferContract {
    uint32_t initial_status_word, initial_indirect_control;
    uint64_t transfer_latency_ns, cbar_base;
    const char *route_policy, *source_policy, *sample_policy;
    const char *status_policy, *terminal_address_policy;
} XT4CalibrationTransferContract;
typedef enum XT4CalibrationComponent {
    XT4_CAL_ROUTE, XT4_CAL_DMA, XT4_CAL_INDIRECT,
} XT4CalibrationComponent;
typedef struct XT4CalibrationTransferEvent {
    XT4CalibrationComponent component;
    hwaddr address;
    uint64_t value, epoch;
    unsigned width, source_length, phase;
    int64_t virtual_ns, deadline_ns, started_ns;
    bool write, transition, success, route_known, active, done, busy;
    uint32_t route, source, destination, count, source_attr, destination_attr;
    uint32_t descriptor, control;
    const char *detail, *source_sha256;
} XT4CalibrationTransferEvent;
typedef void (*XT4CalibrationTransferEventFn)(void *, const XT4CalibrationTransferEvent *);
typedef bool (*XT4CalibrationTransferGuard)(void *);
void xt4_calibration_transfer_set_contract(XT4CalibrationTransferState *, const XT4CalibrationTransferContract *);
void xt4_calibration_transfer_set_links(XT4CalibrationTransferState *, XT4FlashState *, XT4PhyState *, XT4TransferState *, MemoryRegion *, uint64_t, uint64_t, const XT4CoveredNor *);
void xt4_calibration_transfer_set_event_callback(XT4CalibrationTransferState *, XT4CalibrationTransferEventFn, void *);
void xt4_calibration_transfer_set_completion_guard(XT4CalibrationTransferState *, XT4CalibrationTransferGuard, void *);
bool xt4_calibration_transfer_busy(XT4CalibrationTransferState *);
void xt4_calibration_transfer_cancel_pending(XT4CalibrationTransferState *);
#endif
