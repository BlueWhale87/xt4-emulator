# Bounded timer startup exploration

The new `bootstrap-timer-exploration` profile adds `xt4-timer-exploration-v1` to the explicit static-input scenario. Unchanged original firmware completes **20,680 instructions**, **107 modeled writes** and **17 modeled reads** under either fixed scenario, then stops on read32 `0xfffe0000` at PC `0x4f7c`. It remains a single-core synthetic exploration profile, not authentic clock/reset evidence or a booted camera.

## Model contract

Eight channels use current/count words at `0xfff3c310 + 8*channel` and adjacent programmed words at `+4`. Single-channel masked enable/disable commands use `0xfff3c300`. Current and adjacent words are unknown until programmed. Control readback, reserved offsets, unsupported widths and masks fail before mutation.

The scenario explicitly assumes initially halted channels, immediate current-word loading, preserving the count on enable and a 54 MHz virtual rate. Precise QEMU icount maps an instruction to one virtual nanosecond. Reads do not advance time; elapsed fractional ticks accumulate from a stable phase. Disable freezes the integer count, re-enable starts a fresh phase with that count, repeated enable preserves the running phase, and current writes replace count/phase. Adjacent writes do not reload the current count.

Zero-versus-underflow hardware behavior is unknown. The model stops at the first transition to zero, without supplying an enabled zero, wrapping, reloading or delivering an IRQ. Enabling zero or programming zero while enabled is rejected. The terminal event has no originating MMIO width/value; reports preserve an architectural stop PC and the virtual-time boundary. Reset cancels callbacks and clears programmed-word knowledge, retaining only the declared synthetic assumptions.

Virtual timer callbacks can run in QEMU's vCPU thread. Their stop is deferred without synchronously disabling their own clock. Synthetic deadlines are excluded from icount idle warp while remaining normal instruction deadlines. This prevents a debugger pause from skipping directly to expiry; WFI wake/interrupt behavior remains unsupported.

The source wrapper's modes 0–3 select software time units, not one-shot/periodic hardware modes. No IRQ mapping is inferred from software descriptor tags, and no automatic reload or interrupt acknowledgment is implemented.

## Observed original-firmware checkpoint

The independent static oracle and execution agree on ten timer writes: counter 540000 three times, adjacent 539999 three times, counter 0xffffffff three times, then run command 0x00800080. One channel-7 timestamp read follows at virtual time 20,643 ns: value `0xfffffffb`, four ticks below the programmed count after 76 ns enabled. The next distinct device access is read32 `0xfffe0000` at PC `0x4f7c`. That controller's reset/read value remains unknown. The original code chooses these accesses; the model does not replay a hardcoded sequence.

| Model | Successful writes | Successful reads |
| --- | ---: | ---: |
| Port configuration | 97 | 0 |
| Static synthetic inputs | 0 | 16 |
| Bounded synthetic timer | 10 | 1 |

The controller stop carries no stale port/timer label and returns no guessed value. Same-scenario repetitions and a virtual reset are checked for matching device hashes/timestamps; reports count only the latest segment while retaining earlier evidence. Tests compare staged and executed instruction bytes to the recovered original bootstrap.

```sh
python3.13 -m xt4lab run \
  --research-root ../../research/2026-10-14-xt4-firmware \
  --out runs/timer-next-controller \
  --profile bootstrap-timer-exploration --scenario zero-ports-v1 --steps 30000
```

Select `one-ports-v1` explicitly for the other static input fixture. Neither represents a measured normal camera state. Both earlier diagnostics and input-only exploration retain their original boundaries. Generated reports include model/scenario identity, hashes, deterministic device times and terminal transitions; no physical camera is connected.
