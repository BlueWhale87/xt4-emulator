# X-T4 offline emulation foundation

This custom QEMU machine runs original X-T4 2.12 ARM instructions under an explicit diagnostic starting state. It is the first milestone toward full system emulation. Camera boot, menus, display, SD storage, settings persistence, multi-core startup and the recipe manager remain future work.

The original DAT and decoded firmware are external, read-only research inputs. No vendor firmware is committed to this repository. The launcher creates checked private copies, loads anonymous guest RAM, and exposes no camera discovery, USB, physical storage or network devices. It never imports the earlier camera-probe scripts.

## Build and test

The tested host uses Python 3.13. Install the build prerequisites and prepare the external firmware inputs as described in [public checkout setup](public-setup.md). From this checkout:

```sh
python3.13 scripts/build-xt4.py
python3.13 scripts/test-xt4.py
```

The build records its QEMU pin, compiler/dependency versions, source-file hashes and executable hash in `build/xt4-build.json`. QEMU is built locally and never installed globally. Initial setup requires GLib, pkgconf and Ninja; Python build packages are confined to QEMU's build environment. Dependency acquisition may use the Internet; firmware execution uses only local files and private Unix sockets.

## Run original firmware

Use a new output directory each time:

```sh
python3.13 -m xt4lab run \
  --research-root ../../research/2026-10-14-xt4-firmware \
  --out runs/first-firmware-trace \
  --profile module-entry-diagnostic --steps 1000
```

The CLI resolves the research root to its canonical project path. It writes an execution report and evidence in the named run directory. Re-render a retained run with `python3.13 -m xt4lab report runs/first-firmware-trace`.

Add `--interactive` for `step`, `continue N`, `pause`, `reset`, and `quit`. Ctrl-C during a run requests a pause. Reset launches a fresh engine and evidence segment without discarding earlier records. The default instruction budget is 1000, maximum 1000000; response waits are at most two seconds and each execution segment has a 30-second deadline. A terminal dependency or failure requires reset before further execution.

Pausing interrupts any running debugger step and consumes its reply before resuming. Interrupts during packet receipt wait for that packet to finish. An interrupted execution attempt is recorded with uncertain completion and excluded from the confirmed instruction count; it is never silently counted as a completed step.

`--profile validation` intentionally refuses to execute with `missing_boot_state`: a real cold-boot/loader state has not been recovered. The diagnostic profile supplies a synthetic stack at `0x7fff0000`, SP=`0x80000000`, provisional starting registers and a single Cortex-A7 CPU. Zero initialization, RAM permissions and CPU reset system registers remain assumptions. GIC helper code is linked to satisfy QEMU's Arm TCG dependencies; no interrupt-controller device is instantiated or claimed as X-T4 hardware.

## Evidence and next work

- `report.md`: readable outcome and assumptions.
- `summary.json`: structured results, canonical trace fingerprint and starting states.
- `trace.jsonl`: attempted and completed instructions, controls and stops.
- `segments/NNN/engine-events.jsonl`: engine-side reset register state and precise first dependency.
- `dependencies.json`: observed unresolved accesses/exceptions with source evidence.
- `run-info.json`, `engine-build.json`: input/profile and build provenance.

Two firmware-backed test runs on 15 October 2026 each completed eight original instructions, then stopped on their ninth attempt: a byte read at `0x00e20008`, from PC `0x01291024`. Their canonical traces matched and the original input hashes remained unchanged. Its association with loader-provided state is a research lead; the supplying object has not been recovered. Do not return a guessed byte, skip the call or fabricate a boot-ROM implementation to get past it.

The [bootstrap diagnostic](startup.md) now executes a recovered earlier startup boundary for 14,731 instructions, stopping at its first unknown hardware write. Remaining platform dependencies precede native framebuffer menus and controls, then virtual SD/settings operations. The recipe manager follows that working firmware/storage foundation. Emulator behavior applies to modeled paths and cannot establish physical-camera recovery or firmware safety.

The roadmap is summarized in the [project README](../../README.md). Recovered startup dependencies and their remaining unknowns are tracked in the [startup investigation](startup.md).

## Port configuration continuation

The new fixed `bootstrap-port-config-diagnostic` profile enables only `xt4-port-config-v1`. It preserves the same original bootstrap and incoming CPU assumptions, executes 97 inferred logical configuration writes, then stops at an unknown external pin read after 19,613 completed instructions. See [the port-bank guide](port-bank.md). No pin fixture, IRQ, timer, electrical behavior or camera connection is added.

Reports include the selected model, successful write/read counts, `device-events.json` and a separate canonical modeled-event hash. Old schema-1 reports without a model list remain readable. The original default profile and both earlier dependency checkpoints remain available.
