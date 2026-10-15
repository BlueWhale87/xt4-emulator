# X-T4 emulator handover — 15 October 2026

Development is paused at the user's request so it can resume in a new chat. The target remains original firmware booting into its own UI, usable virtual controls, virtual SD/settings persistence, and core workflows for testing enhancements. **Original camera menus are not running yet.**

## Verified public checkpoint

- Repository: <https://github.com/BlueWhale87/xt4-emulator>.
- Last verified public **code** revision: `5d16d9cdbff5756afc931baf3ecf213fd4e86135`. This handover is a documentation-only successor; it does not publish unfinished code.
- QEMU executes the unchanged X-T4 2.12 bootstrap on the existing Cortex-A7 diagnostic machine. Port configuration/input, timer, transfer-global enable, clock apply, early NOR commands and a causal digital PHY handshake are modeled under explicit virtual assumptions.
- The reproducible boundary is a 32-bit write to `0xfff4a500`, value `0x01002a2c`, PC `0x4f34`, after **23,842 raw QEMU-counted instructions**. This is an execution count, not a retired-instruction claim or a progress percentage.
- Both synthetic input fixtures and repeats/fresh reset reproduce 204 modeled writes,128 reads and9 transitions:5 NOR,2 clock,2 PHY. The calibration destination `[0xca00,0xca80)` remains untouched.
- Verification at that revision:7 NOR unit groups,109 Python tests,41 board checks and5 decoder tests passed. A fresh independent review found no Critical/Important issues.
- SMP/full platform boot, interrupts, main loader continuation, display, interactive controls, installed settings, virtual SD and sensor/image-processing behavior remain unfinished.

## Local work preserved for resumption

The existing isolated feature checkout remains on `codex/xt4-emulator-foundation`. Its local HEAD is **`1b045fbe546fdaed59be2ab41c67a5b61881a257`**, ahead of the verified public code revision. This commit is not published by this documentation update.

Task1 of the active calibration-transfer plan is complete: a pure covered-NOR-span reader and checked extraction of the128-byte calibration source. Its full gate passed7 NOR groups,3 covered-media groups,112 Python tests,41 board checks and5 decoder tests. Preserve this commit and do not reimplement Task1.

Task2 is **unfinished and uncommitted**. A tracked patch, untracked source copies, the local Task1 commit bundle and execution workspace have been backed up and hash-verified locally. The separate local handover gives their exact locations. Preserve the working tree; do not reset, clean or overwrite it.

Written so far:

- Optional typed transfer/media metadata, an explicit calibration profile, immutable staging/copying, report identity checks and CLI selection.
- Python admission/staging/report tests. The typed-contract case and previous-PHY identity pins pass; the full new integration file has not passed against a new engine.
- Three new qtest groups for transfer lifecycle/delivery, rejected descriptors/accesses, cancellation/lost prerequisites/overflow. Their observed RED result was the existing engine rejecting the unsupported new C profile.
- A route/DMA/indirect coordinator and typed header, plus flash/PHY/global-enable accessors and pending-transfer exclusion in the parent flash controller.

**The C coordinator is not wired to board creation, C metadata admission, Kconfig or Meson, and has not been compiled or runtime-tested.** Do not infer a working transfer from the presence of source. Pending-budget/timeout Python fixtures, the original-code calibration oracle, full verification and final independent review still need completion.

## Immediate implementation work

Resume Task2 from its existing brief and ledger, using Native execution. Read the local handover before broad searches or repeated analysis.

1. Finish independent C admission for the exact media and transfer scenario, load/check the immutable128-byte input before starting evidence, and preserve older scenario/profile hashes.
2. Wire the coordinator to existing RAM, flash, PHY and global enable. Add event accounting, reset/first-stop cancellation and the shared completion guard. Map the three priority MMIO windows. Link sources through Kconfig/Meson.
3. Apply the new profile's declared virtual `reset-cbar=0xf8f00000` before CPU realization; bind actual reset/media identity in reports. Do not change older profiles or instantiate a pretend GIC.
4. Finish focused negative and pending-transaction tests, including invalid source/destination coverage, missing prerequisites, real pause/resume, wall timeout, earlier/tied instruction budgets and absence of late RAM writes.
5. Execute the untouched bootstrap for both fixtures/repeat/fresh reset on the final recorded executable. Retain actual results; do not adjust an oracle merely to force progress.
6. Run the full owned suite and decoder checks, then one fresh whole-range review. Fix Important/Critical findings in one verified pass and record deferred Minors. Publish only tested, reviewed source checkpoints.

The next intended transaction is128 sequential bytes from CPU address `0xf002a080` into guest RAM through the programmed request/DMA/indirect path. Only this evidenced source span is covered. The original firmware must perform its own comparisons and compute calibration results; the model must not prefill comparison RAM or fabricate a successful result.

The declared virtual assumptions include strict F0000000-window translation,64ns whole transfer latency, request-active status bit16, retained programmed terminal addresses and ideal taps0..127 in programmed phases20/21. These are not measured camera electrical/DMA behavior. The predicted next unmodeled dependency after calibration is a GIC read at virtual CBAR+0x1000 (`0xf8f01000`, PC `0x34b0`); **that continuation is not yet demonstrated**. Preserve any earlier real dependency instead of bypassing it.

## Separately authorized camera checks

Later in the session the user explicitly allowed normal USB information capture, service-mode information capture, and one bounded debug-flag experiment. These were separate from the emulator runtime.

- Normal PTP confirmed modelX-T4/public firmware2.12. Photos initially held the connection; the query worked after the user closed Photos. Apple ImageCaptureCore assigns its own wire transaction IDs, so the adapter must retain those rather than require an echo of the caller's proposed number.
- Service USB confirmed internal software revision2.16 and debug byteF7 initially0.
- With explicit approval, the bounded probe verifiedF7=0, changed it to1, verified1, read64bytes at the known executing service routine `0x0116674c`, and restored0/read it back. The64bytes exactly matched the fixed unpacked primary image.
- A separate new service session independently recheckedF7=0 and a responsive matching model/revision. All our USB sessions were closed.
- No configuration-save command, firmware flash, code upload, arbitrary memory write, MMIO read or photo transfer was requested. **Current RAM restoration is verified; absence of automatic persistence or restoration after reboot is not.** No general X-T4 recovery guarantee is established.

The experiment is complete. Debugging was not left enabled. Do not automatically repeat the toggle, broaden memory reads or assume live writes are authorized in a new chat. Obtain the user's current instruction for any further physical testing. Raw code bytes, serial/device-specific data and capture logs remain private and must not be committed.

## Research conclusions and retained issues

The live FujiHack main revision checked was `754acff9aa0322df19446cf7eca699b221c3330d`, unchanged from the prior retained snapshot. Its X-T4 header is a model-name placeholder. Its current/legacy Unicorn experiments start injected functions; no complete X-T4 boot/UI emulator was found in the checked trees. The wiki still lists a QEMU hardware emulator as future work. Its task, I/O and rasterizer headers are useful later search seeds, with X-T4 compatibility requiring verification. The screenshot author's inspected public reply said his own emulator was not yet on GitHub; he described using live camera experiments to inform it.

Four previous Minors remain deferred:

1. Report model-list consistency is not reconciled against the fixed profile, although normal metadata and actual event counts remain correct.
2. Idle terminal NOR cancellation clears the completed response, limiting raw debugger inspection.
3. The NOR-core guide still describes integration as future work; newer guides give the actual status.
4. Direct pending-PHY timeout/DLL-budget regression coverage is incomplete; no execution defect was demonstrated.

Keep all existing ledger rulings and their costs. Do not silently fix Minors or claim full camera fidelity.

## Operating boundaries

- Keep the emulator free of physical camera/USB passthrough and runtime host networking.
- Synced `sources/` are read-only. Preserve original firmware, private research, runs/builds and sibling workspaces.
- Use the existing pinned QEMU checkout and Python3.13. The build wrapper records the exact engine; Ninja alone does not update that record. Do not edit source during a full build/test gate.
- Approved design, implementation plan and Native method already exist; resumption does not require repeating their routine approval process.
- Publish only to the `public` Git remote for this repository. Never push to upstream QEMU `origin`.
- Do not publish firmware, extracted vendor bytes, disassembly, private captures, credentials or host-specific paths.
- No fixed ETA or percentage of complete emulation is established. Missing hardware behavior remains the main uncertainty.
