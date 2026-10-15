# First preview observation

This procedure prepares the native detector's next original-firmware experiment.
It adds no device model or callable camera binding. Run it only when an emulator
checkpoint naturally reaches the relevant application code. Startup and DMA
implementation belong to the emulator workstream; the native detector remains
independent under `contrib/xt4-native-detection`.

## Establish the run identity

Retain the emulator commit, built-engine SHA256, profile/scenario identities,
firmware SHA256, run mode, stop record and event/trace hashes. Use a new output
directory for each run. The expected official firmware 2.12 SHA256 is
`853d273b5603d3a2b93493f0ebd6a762b0b4b6bdc70317777775ae5f8ed93bc5`.

The addresses below come from analysis ELF SHA256
`2de7f18409a5a4a96f5da2b3b4e76022f81040f3ba573146bb62922f31c08249`.
They are analysis virtual addresses, not update-file offsets. Before installing
observation points, compare the mapped instruction bytes with the matching
decoded firmware and retain both byte hashes and the mapping evidence. If they
differ, resolve the mapping first; a guessed relocation delta is insufficient.

Use emulator debugger execution breakpoints or tracing that leaves guest
instruction bytes unchanged. Do not redirect PC into these functions, supply
invented initialized graphics state, call a getter/setter from the debugger, or
skip an initializer. The earlier isolated PIC getter test used a synthetic
caller and does not satisfy this natural-execution requirement.

## Capture metadata before pixels

At each selected hit retain sequence number, core, PC, CPSR, r0-r3, SP and LR,
plus the enclosing invocation and available task/context identity. Record task
identity as unknown when the bench cannot establish it; stack address alone is
not proof of a task. Distinguish emulator virtual time, instruction counts and
host wall time. Paused debugger runs cannot measure preview responsiveness.

The register interpretations below are static hypotheses to check against the
natural caller. A nonzero word is not sufficient proof of a valid pointer.
Read a pointed-to field only after establishing its mapped RAM extent and
accessibility. Do not probe unknown MMIO or scan memory to find buffers.

| Observation point | Retain and correlate | What remains unresolved |
| --- | --- | --- |
| PIC getter wrapper entry `0x121a440` | Raw r0 handle, r1 layer, r2 output address, caller LR; match its later leaf invocation and return | Actual handle resolution and calling convention |
| PIC getter leaf entry `0x122c6a0` | Raw r0 context, r1 layer, r2 output; for a valid mapped context and layer 0/1, the word at `ctx+0xd4+layer*8`; observe r0 status and output on the matching natural return | A returned address does not establish completion, stride, format or a lease |
| PIC setter wrapper/leaf `0x1219fdc` / `0x122c624` | Observe natural calls only; correlate handle/context, layer and r2 value with getter results and producer events | Setter use does not establish when pixels become readable or stop being reused |
| OSD update entry `0x1247120` | Invocation, caller, context/task evidence and subsequent render/surface selection | Display identity and surface alternation must be observed |
| Render entry `0x124688c` | Raw r0 selector, r1/r2 dimensions, r3 surface argument and fifth argument at entry SP when mapped RAM permits that read | Selectors, dimensions and surface roles are provisional; r0 can be `0xffffffff`, so it is not an established display ID |
| Initial clear `0x12468dc`, object walk `0x12468fc`, optional clear `0x1246a0c` | Ordered hits within the same render invocation | Object-walk return can be followed by another clear |
| Pre-flush `0x124690c` | Same invocation and selected surface; correlate preceding clears and object traversal | Candidate insertion location only; no drawing is enabled here |
| Surface-completion call `0x1246924` | Actual call arguments and return at `0x1246928`, correlated to render entry and later display submission | A successful call does not by itself prove display scanout or safe buffer reuse |

Do not interpret r0-r3 at an interior render address as the original entry
arguments: intervening calls clobber them. Keep an invocation record from entry
and pair returns by core, caller and stack/context evidence. If pairing is
ambiguous or observations are dropped, mark the interval incomplete. A missing
hit is not proof that a branch or frame did not occur.

Start with a bounded sample, such as 32 complete render invocations, and stop
earlier on the bench's first unsupported access. This is an observation limit,
not an assertion that the current client supports a multi-breakpoint recorder.
Record actual debugger/trace support and any imposed limits with the run.

## Resolve the frame contract

Correlate PIC address changes with the actual producer, its completion event,
cache maintenance, ownership and reuse. Record the exact mode, layer, dimensions,
visible rectangle, row stride, format, plane layout and accessible byte extent.
Unknown fields stay unknown; the diagnostic dump routine's two-byte storage
unit does not establish live preview format. Never multiply guessed dimensions
to decide how much memory is readable.

Once format and extent are established, retain a bounded frame sample with its
metadata and hash in private run output. A debugger snapshot can establish byte
content at a paused instant; it cannot establish a concurrent frame lease or
cache coherency for a running inference worker. Do not invoke `0x121c49c` or
`eglQueryFrameDMP` to obtain a sample: the former writes files and waits, and the
latter clears graphics surface state.

The native provider currently accepts completed BGR8 views. If firmware uses
another format, leave that binding unavailable until a separate conversion is
verified against known samples. Copy/downsample to owned memory and release the
producer's verified lease before inference. The current 8 MiB host allocation
ceiling is not a camera RAM reservation.

## Resolve placement and ownership

Verify the order clear -> object traversal -> optional clear -> pre-flush ->
surface completion independently for each observed surface/mode. The selected
pre-flush address is after both clear paths in the retained instructions; it
is not yet a registered callback or a state-preserving insertion mechanism.
The callback array drained after display submission remains unqualified for
drawing.

Recover image-to-display crop, scale, mirror and rotation from actual mode
state. Correlate those transforms with a geometry generation and invalidate
results whenever that generation or display changes. Confirm the owning
graphics task/context, initialization and resource budget, and preservation of
matrix, clip, paint, blend and stroke state before inserting a box. Run inference
outside the graphics task. Keep all firmware bindings disabled until these
conditions and frame ownership are established.

## Report the next result

Publish a concise summary alongside [hook readiness](preview-overlay-readiness.md):
which points were naturally reached, observed call/clear/surface order, verified
metadata and unresolved fields. Link private trace hashes and the static Evidence
IDs below. Keep firmware bytes, decompilation, raw RAM, frame dumps and local host
paths out of the public repository. Report recorded-frame or synthetic boxes by
their actual source; do not label them an original-firmware preview.

Progress to adapter implementation requires verified runtime mapping/ABI,
completed frame ownership/cache rules, format/extent, transforms, task/context
ownership, insertion/state restoration and measured available memory. If the
bench stops earlier, retain that boundary and leave these observations pending.
Original-firmware UI execution and X-T4 performance are separate claims.

## Static evidence

- Getter wrapper: `ev_373e30f5209e0e93493af99253921c5bf1e081cc7c05da72cb76098743fb3e6e`.
- Getter instructions / isolated probe: `ev_fd0d146f10412aa45e3851eb95cc06f18edb689a6eabc543cd499083f05c9818`.
- Setter wrapper: `ev_c3076417556bda46f4c3eb67a664adf1a14473fe4809d4ef5ec79375fa928657`.
- Setter leaf: `ev_52249738b3dab06ccd009da6b1cc37e9afa92575252fd6a87104199c90bd5196`.
- Render instructions: `ev_099e3b6019cc74f03577662347612629f699456a71189bf1678456cb9b4791f2`.
- Update path: `ev_591b61b56313d30999bf27e8350de16cec2a63a76eac46bf73d88f3a4b6e55e5`.

These establish static candidates and the limited isolated getter result, not
natural preview execution. All candidates remain disabled in
`contrib/xt4-native-detection/profiles/xt4-2.12-evidence.json`.
