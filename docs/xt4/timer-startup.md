# Bounded timer startup exploration

The new `bootstrap-timer-exploration` profile adds `xt4-timer-exploration-v1` to the explicit static-input scenario. Device and runner tests pass; original-firmware continuation is being verified. It remains a single-core synthetic exploration profile, not authentic clock/reset evidence or a booted camera.

## Model contract

Eight channels use current/count words at `0xfff3c310 + 8*channel` and adjacent programmed words at `+4`. Single-channel masked enable/disable commands use `0xfff3c300`. Current and adjacent words are unknown until programmed. Control readback, reserved offsets, unsupported widths and masks fail before mutation.

The scenario explicitly assumes initially halted channels, immediate current-word loading, preserving the count on enable and a 54 MHz virtual rate. Precise QEMU icount maps an instruction to one virtual nanosecond. Reads do not advance time; elapsed fractional ticks accumulate from a stable phase. Disable freezes the integer count, re-enable starts a fresh phase with that count, repeated enable preserves the running phase, and current writes replace count/phase. Adjacent writes do not reload the current count.

Zero-versus-underflow hardware behavior is unknown. The model stops at the first transition to zero, without supplying an enabled zero, wrapping, reloading or delivering an IRQ. Enabling zero or programming zero while enabled is rejected. The terminal event has no originating MMIO width/value; reports preserve an architectural stop PC and the virtual-time boundary. Reset cancels callbacks and clears programmed-word knowledge, retaining only the declared synthetic assumptions.

Virtual timer callbacks can run in QEMU's vCPU thread. Their stop is deferred without synchronously disabling their own clock. Synthetic deadlines are excluded from icount idle warp while remaining normal instruction deadlines. This prevents a debugger pause from skipping directly to expiry; WFI wake/interrupt behavior remains unsupported.

The source wrapper's modes0–3 select software time units, not one-shot/periodic hardware modes. No IRQ mapping is inferred from software descriptor tags, and no automatic reload or interrupt acknowledgment is implemented.

## Next original-firmware checkpoint

Static evidence predicts ten writes: counter540000 three times, adjacent539999 three times, counter0xffffffff three times, then run command0x00800080. One channel-7 timestamp read follows; the next distinct device access is read32 `0xfffe0000` at PC `0x4f7c`. That controller's reset/read value remains unknown. This prediction is not a hardcoded device replay.

```sh
python3.13 -m xt4lab run \
  --research-root ../../research/2026-10-14-xt4-firmware \
  --out runs/timer-next-controller \
  --profile bootstrap-timer-exploration --scenario zero-ports-v1 --steps 30000
```

Select `one-ports-v1` explicitly for the other static input fixture. Neither represents a measured normal camera state. Both earlier diagnostics and input-only exploration retain their original boundaries. Generated reports include model/scenario identity, hashes, deterministic device times and terminal transitions; no physical camera is connected.
