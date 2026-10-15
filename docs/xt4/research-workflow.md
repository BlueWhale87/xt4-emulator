# Research workflow

The active priority is original full boot. Work from the first reproducible unsupported dependency toward original loading, core coordination, runtime services, storage and display. Instruction counts are useful trace identifiers, not a percentage of the remaining work.

## Investigation loop

1. Keep one active boot dependency. Write the question, existing evidence and expected observable effect in the current task brief. Reuse the approved architecture and previous analysis snapshots.
2. Run unchanged firmware with bounded continuation and compact device events. Use instruction stepping only when the question needs individual instructions, register changes or a complete instruction trace. Never infer that compact execution recorded all PCs.
3. Inspect the failing access and relevant caller/helper. Decompile selected addresses from the latest retained snapshot; avoid repeating completed whole-image discovery or unrelated analysis.
4. Add a causal model supported by recovered software behavior and clearly declared virtual assumptions. Related loader operations can share one coherent checkpoint. Unknown data, unsupported hardware and core waits still stop; speed does not justify success stubs.
5. Run focused regression checks while iterating. Keep source edits, dependent probes and builds sequential; batch independent reads. Preserve the first useful trace instead of repeatedly collecting the same boundary.

For the current interrupt path:

```sh
build/pyvenv/bin/python3 -m unittest discover -s tests/xt4lab -p test_gic_run.py -v
build/pyvenv/bin/meson test -C build --no-rebuild --print-errorlogs \
  qtest-arm/xt4-test --test-args='-p /arm/xt4/gic'
```

Select the corresponding test module and board-test prefix for another device. These focused checks do not replace the complete checkpoint gate.

## Checkpoint gate

- Freeze a coherent candidate after its focused checks pass. Record its commit, then build that exact revision so the final engine and source manifest already identify the candidate.
- Run `build/pyvenv/bin/python3 scripts/test-xt4.py` once against that build. Run the decoder checks when decoder inputs, extraction or decoding changed. Review the immutable candidate range once; validate any accepted fixes with reproducing tests and appropriate regression checks.
- A later source behavior change invalidates affected verification. A documentation-only change or commit/version rebuild calls for identity checks and relevant focused checks, rather than automatically repeating an unchanged complete suite.
- Publish only after the required checks and review are green and publication is authorized. Audit the diff for raw firmware/captures and private paths. Keep one hash-verified execution archive and one current status/decision record; avoid duplicating the same large evidence bundle at every step.

## Fast integration checks and detailed oracles

Port/input integration checks use bounded continuation while retaining the original device-event hashes, memory assertions and unsupported-stop assertions. Their counters are raw engine instruction counts; `completed_steps` and complete PC lists remain empty. The port reset/report test and bootstrap/timer instruction-oracle tests retain single stepping because their assertions require it.

On16October2026, the same three representative port/input runs took63.851s with single stepping and0.456s with bounded continuation on the same engine. Device-event hashes, checked input-cache bytes, operation counts and semantic stop fields matched. This measures that subset on one host, not the speed of the complete suite or the reverse-engineering project.

## Evidence and communication

- Record the latest supported boundary, unresolved assumptions and next concrete question. Keep progress updates about findings and changed decisions; do not narrate unchanged polls.
- Use the camera only for an explicitly authorized, targeted probe that resolves a named uncertainty. Existing camera access is not blanket authorization for further operations.
- Keep firmware, decoded images, camera captures and raw run data private and excluded from commits. The emulator remains isolated from physical devices.
- Reuse the current checkout and existing evidence. Add parallel workers only when authorized and when their questions are independent; avoid several workers editing or rediscovering the same path.
