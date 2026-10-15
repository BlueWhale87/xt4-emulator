# Bounded execution policy

The engine has an opt-in instruction-budget policy for efficient continuation. The Python fast launcher is the next connected task; the existing CLI and single-step evidence remain unchanged at this engine checkpoint. Menus have not booted.

A nonzero `instruction-budget` machine property admits 1–1,000,000,000 QEMU engine-accounted instructions under precise single-thread TCG icount, one virtual nanosecond per instruction. Zero disables this policy and preserves legacy behavior. The property is initialization-only. An external virtual timer bounds execution without causing the idle clock to jump to its own deadline.

Raw icount is a QEMU accounting quantity, not a retirement count or a list of debugger-confirmed completed instructions. The board samples it after vCPUs stop, before the debugger's stop reply, and records `execution-state` with its sequence, PC, virtual time, raw count and budget. It never reads raw icount from arbitrary translated fault hooks. Known memory/exception stops remain authoritative and prevent later sentinel writes.

The deadline callback compares raw count with the requested budget. Exact equality records `instruction_budget`; a shorter count after idle warp rearms only the remaining count; overshoot is an explicit invariant failure. Signed deadline overflow records `unsupported_execution_deadline`. No enabled zero timer, ready status, IRQ or physical timing is fabricated.

An enabled architectural timer with no wired interrupt can advance the idle clock to its signed maximum and then continually rearm at that same time. At this unsupported terminal boundary the board cancels CPU generic-timer host callbacks to let the VM stop; guest-visible timer registers and IRQ results remain unmodified. Recovery requires a fresh engine, as `RunSession.reset()` already provides. A same-process warm reset does not rewind QEMU's global clock. Normal in-range reset separately refreshes the budget baseline.

Focused real-TCG checks cover exact budgets around translated-block boundaries and beyond one million instructions, all existing strict fault classes, first-stop authority versus the modeled timer, pause/count freezing, reset and idle-warp exhaustion. Test programs are confined to anonymous guest RAM; source firmware stays unchanged. These checks establish execution controls, not camera boot or hardware fidelity.
