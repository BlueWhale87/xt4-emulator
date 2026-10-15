# Causal PHY initialization exploration

`bootstrap-phy-exploration` executes the original first PHY reset/lock sequence,
then stops at its first unsupported DMA request-routing write. The observed
boundary is **PC `0x4f34`, W32 `0xfff4a500 = 0x01002a2c`**, after **23,842 QEMU raw
engine-counted instructions** under either explicit input fixture. No transfer
has occurred: the128-byte destination buffer remains untouched. Calibration,
menus, display, controls, SD/settings, CPU interrupts and full platform/SMP boot
remain incomplete.

```sh
python3.13 -m xt4lab run \
  --research-root ../../research/2026-10-14-xt4-firmware \
  --out runs/phy-startup-zero \
  --profile bootstrap-phy-exploration --scenario zero-ports-v1 \
  --steps 100000 --fast
```

Use a fresh output directory and your own pinned update, following
[public setup](public-setup.md). The repository supplies no vendor firmware,
decoded media, disassembly or private runs. Execution has no camera, USB,
physical-storage or host-network backend. Previous profiles, including the
[flash checkpoint](flash-startup.md), retain their earlier boundaries.

## What ran

The original flash initializer still completes its five NOR commands and selects
its cached descriptor. It then enables PHY, programs read instruction `140333ee`
for its first probing phase, and performs this sequence:

| Original operation | Virtual result |
| --- | --- |
| B4=0, B8=30 | Reset clears lock flags; stage master initial delay |
| Read BC | Loopback bit15 is clear |
| B4=`40000000` | Schedule master readiness |
| Read BC after completion | Bit15 set; DLL bit0 still clear |
| B4=`c01f0000` | Schedule DLL lock for TXtap31/RXtap0 |
| Read BC after completion | Bits15 and0 set |

Each scheduled transition completes4 virtual nanoseconds after its accepted
trigger. Status reads never advance the state. The next original transfer helper
attempts to configure request routing; that unknown write stops execution with
its complete attempted value. The model does not skip the transfer, prefill its
destination, manufacture a passing delay window, or report calibration success.

This profile records204 successful writes,128 reads, two clock transitions,
five NOR completions and two PHY transitions. Both fixtures reach the same
instruction count; repeats compare event hashes and timestamps. Raw execution
counts differ from retired instructions and completed single steps; see
[execution semantics](execution.md).

## Evidence and explicit assumptions

The public [Xilinx OSPI header](https://github.com/Xilinx/embeddedsw/blob/97f2baf7f69e9d5f447650110af0d8841e501277/XilinxProcessorIPLib/drivers/ospipsv/src/xospipsv_hw.h)
defines Config PHY enable, B4 reset/resync and tap fields, B8 master initial
delay, and the separate BC loopback/DLL lock bits. Its
[tuning implementation](https://github.com/Xilinx/embeddedsw/blob/97f2baf7f69e9d5f447650110af0d8841e501277/XilinxProcessorIPLib/drivers/ospipsv/src/xospipsv_control.c)
uses related dummy-phase/tap/resync operations. These corroborate the register
family and mechanism, not the exact camera integration or electrical timing.

The fixed scenario explicitly selects initial observation0, immediate lock
clearing on reset/disable,4ns master readiness and4ns DLL lock. These are virtual
timing/reset choices. DLL lock does not establish a passing data eye or correct
NOR sampling. Array addressing, actual source transfers and valid taps require
additional models and evidence. The fitted camera NOR/SoC identity and F0 bit4
meaning remain unestablished.

## Device boundaries

The PHY has sole authority over B4/B8/BC in this profile through a higher-priority
three-register mapping. Earlier profiles keep their previous flash model and
unknown PHY boundaries. Unknown B4/B8 readback, unsupported widths/fields and
invalid sequences reject atomically. BC is read-only. Every reset requires a
fresh master stage before release; master readiness precedes resync/DLL lock.

Flash Config and PHY admission are connected: enabling PHY with its controller
disabled rejects, and a Config write during pending PHY work leaves both
devices unchanged. No pending transition survives reset or a terminal stop.
Instruction budgets take priority at tied deadlines. Ordinary debugger pauses
freeze virtual time; a fresh runner segment cannot receive an older callback.
The source tests exercise these boundaries, exact typed C/Python identity and
report binding, real debugger pauses, original register order and unchanged
firmware bytes. Results remain explicitly synthetic exploration.
