/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SSI_XT4_PHY_H
#define HW_SSI_XT4_PHY_H
#include "hw/core/sysbus.h"

#define TYPE_XT4_PHY "xt4-cadence-phy-exploration"
#define XT4_PHY_MODEL "xt4-cadence-phy-exploration-v1"
#define XT4_PHY_RESET_POLICY "reset-disable-immediate-clear-v1"
OBJECT_DECLARE_SIMPLE_TYPE(XT4PhyState, XT4_PHY)

typedef struct XT4PhyContract {
    uint64_t master_lock_latency_ns, dll_lock_latency_ns;
    uint32_t initial_observable_word;
    const char *reset_policy;
} XT4PhyContract;
typedef struct XT4PhyEvent {
    hwaddr address;
    uint64_t value, epoch;
    unsigned width;
    int64_t virtual_ns, deadline_ns;
    uint32_t configuration, master_control, observable;
    bool write, transition, success, busy;
    bool controller_known, enabled, configuration_known, master_known, fresh_master;
    const char *detail, *phase;
} XT4PhyEvent;
typedef void (*XT4PhyEventFn)(void *, const XT4PhyEvent *);
typedef bool (*XT4PhyCompletionGuard)(void *);
void xt4_phy_set_contract(XT4PhyState *, const XT4PhyContract *);
void xt4_phy_set_event_callback(XT4PhyState *, XT4PhyEventFn, void *);
void xt4_phy_set_completion_guard(XT4PhyState *, XT4PhyCompletionGuard, void *);
bool xt4_phy_set_controller_config(XT4PhyState *, bool, const char **);
bool xt4_phy_array_ready(XT4PhyState *, uint64_t *epoch);
void xt4_phy_cancel_pending(XT4PhyState *);
#endif
