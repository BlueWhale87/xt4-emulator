# Bounded execution policy

The engine and offline runner have an opt-in instruction-budget policy for efficient continuation. The default CLI and single-step evidence remain unchanged. Menus have not booted.

A nonzero `instruction-budget` machine property admits 1–1,000,000,000 QEMU engine-accounted instructions under precise single-thread TCG icount, one virtual nanosecond per instruction. Zero disables this policy and preserves legacy behavior. The property is initialization-only. An external virtual timer bounds execution without causing the idle clock to jump to its own deadline.

Raw icount is a QEMU accounting quantity, not a retirement count or a list of debugger-confirmed completed instructions. The board samples it after vCPUs stop, before the debugger's stop reply, and records `execution-state` with its sequence, PC, virtual time, raw count and budget. It never reads raw icount from arbitrary translated fault hooks. Known memory/exception stops remain authoritative and prevent later sentinel writes.

The deadline callback compares raw count with the requested budget. Exact equality records `instruction_budget`; a shorter count after idle warp rearms only the remaining count; overshoot is an explicit invariant failure. Signed deadline overflow records `unsupported_execution_deadline`. No enabled zero timer, ready status, IRQ or physical timing is fabricated.

An enabled architectural timer with no wired interrupt can advance the idle clock to its signed maximum and then continually rearm at that same time. At this unsupported terminal boundary the board cancels CPU generic-timer host callbacks to let the VM stop; guest-visible timer registers and IRQ results remain unmodified. Recovery requires a fresh engine, as `RunSession.reset()` already provides. A same-process warm reset does not rewind QEMU's global clock. Normal in-range reset separately refreshes the budget baseline.

Focused real-TCG checks cover exact budgets around translated-block boundaries and beyond one million instructions, all existing strict fault classes, first-stop authority versus the modeled timer, pause/count freezing, reset and idle-warp exhaustion. Test programs are confined to anonymous guest RAM; source firmware stays unchanged. These checks establish execution controls, not camera boot or hardware fidelity.

## Offline runner

Add `--fast` to a noninteractive `python3.13 -m xt4lab run` command. `--steps` then limits total engine accounting, up to one billion, rather than requesting individual debugger steps. A timer exploration still requires an explicit `--scenario`. The Python API uses `RunSession(..., execution_mode='bounded-continue')` and `continue_fast()`. Mixing this with `step()` or `continue_steps()` is rejected. The initial fast CLI rejects `--interactive` before staging a run; API pause, resume and reset remain available.

Fast mode uses a compact boundary/device trace. Reports retain `completed_steps=0` and `completed_pcs=[]`, and label `engine_instruction_count` separately as raw QEMU accounting. Fresh stopped snapshots are required after each continue. A crash, disconnect or missing snapshot cannot reuse an earlier pause count. Reports bind execution metadata to the engine policy, including the budget and count semantics. Older reports without execution metadata retain their single-step interpretation.

Ordinary protocol acknowledgements and frames remain bounded to two seconds. A continue can wait until the segment's absolute execution deadline, at most thirty seconds, while checking for a pause every twenty milliseconds. It sends one interrupt while awaiting a stop prefix, preserving any partially received frame. Wall expiry permits at most two further seconds to drain a stop. The runner terminates and reaps the child on timeout or protocol failure. Reset starts a fresh engine and evidence segment.

Both original timer-bootstrap scenarios reach the same strict read at `0xfffe0000`, PC `0x4f7c`, with the same 107 successful writes, 17 reads, cache contents, timestamps and canonical device-event hashes as single stepping. This demonstrates parity through the established diagnostic boundary; it does not establish menus or full boot.
