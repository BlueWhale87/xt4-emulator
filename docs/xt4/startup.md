# Startup dependency investigation

Status: offline analysis and original-bootstrap execution, 15 October 2026. The newest `bootstrap-port-config-diagnostic` completes **19,613 original instructions** through **97 modeled port configuration writes**, then stops at the first unknown pin sample. No camera was accessed. Menus have not booted.

The original `module-entry-diagnostic` remains the eight-instruction baseline, and `bootstrap-entry-diagnostic` remains the 14,731-instruction baseline with no device model. Both bootstrap profiles map only the recovered low bootstrap and its evidenced RAM envelope; the additional context/support/storage regions below are recovered research inputs, not yet a complete runtime memory map.

## First missing object recovered

The first diagnostic stop is a byte read at `0x00e20008`, from PC `0x01291024`. The update contains a 1 KiB object for runtime address `0x00e20000` at original DAT offset `0x140274`. Recover each byte by XOR with `0xff`; no decompression is involved.

Its SHA-256 is `41ca21d9445075090441de329567b9fa459f9be4ccf54b7e51ad0637884d8d3e`. The first four words are pointers `0x00e20040`, `0x00e201ec`, the value zero, and pointer `0x00e201cc`. The object includes the module table that corroborates the primary and secondary load addresses.

Bootstrap code copies `0x400` bytes from flash offset `0x140000` to RAM `0x00e20000` on one branch. For these uncompressed flash-layout regions, the DAT file range is shifted by `0x274` from the corresponding flash offset; this is not a general mapping for decompressed program addresses. Another branch writes word `1` at context offset `+8`. The source bytes alone therefore do not establish the value seen at every module-entry boundary.

Observed statically: the branch tests bootstrap global `0x0000b060`; its packaged initial value is `1`. The complete reset/runtime provenance of that global and the appropriate primary-entry state still require tracing. Loading the source object is a useful diagnostic boundary, but cannot yet be called an authentic cold boot.

## Additional code is present

A previously unmapped call at `0x00057974` falls inside a packaged support module. It should no longer be described as necessarily missing boot ROM.

| Region | Original DAT offset | Decoded bytes | Inferred runtime base | SHA-256 |
| --- | --- | ---: | --- | --- |
| Bootstrap initialized bytes | `0x20274` | `0xc880` | `0x00000000` | `bf2d1d4ca9d1d60e5125a964988a3f1852e177c3176d35dc59dcc63afe41f6b1` |
| Support module 0 | `0x60274` | `0x1b0c0` | `0x00040000` | `bae8613409663200796fef039468f00895924921f07179c987e368f4bc0a6111` |
| Startup context | `0x140274` | `0x400` | `0x00e20000` | `41ca21d9445075090441de329567b9fa459f9be4ccf54b7e51ad0637884d8d3e` |
| Storage module 3 | `0x160274` | `0x24060` | `0x00e20400` | `3ce0ffd3971f9aca07f7f59c7883ca36226d5e4d33894850951ce111157c1473` |
| Storage module 4 | `0x1e0274` | `0x24060` | `0x00f20400` | `e723030fed6140d01e93fed6d252610ebd83e9c1240cca70424a878b08de94cf` |

All five slices use XOR `0xff`. Bootstrap vector words target low addresses including `0x100`, corroborating runtime base zero. The module table supplies the support/storage load addresses and lengths. The bootstrap slice ends at its observed BSS-start literal; it is not the complete allocated memory extent. These are analyst-selected slices, not vendor ELF files or measured RAM images.

The primary startup path calls support function `0x00049d4c` from `0x0127ef78`. This introduces scheduler/allocator startup dependencies that must be explained alongside the context block. Mapping code alone does not supply initialized operating-system state.

## Verified bootstrap checkpoint

The profile enters the original vector at address zero, which loads PC `0x100`. It maps initialized bytes `[0,0xc880)` and RAM through `0x1b0c0`, based on the BSS and stack literals. Original instructions clear BSS `[0xc880,0x110c0)` and the core-0 stack subsection `[0x110c0,0x12cc0)`; they also set banked stack pointers. The observed loops contain 4,625 executions of the conditional BSS-store instruction (including its final false condition) and 224 eight-register stack-clear stores.

After **14,731 completed instructions**, attempt **14,732** stops with:

| Field | Observed value |
| --- | --- |
| Reason | `unsupported_access` |
| PC | `0x00004f34` |
| Physical address | `0xfff41018` |
| Access | 32-bit write |
| Attempted value | `0xc0ff3f0f` |

This matches the independent static prediction from `0x1244 -> 0x41ec -> 0x4f2c`. The register's behavior remains unknown; no device model accepted this write. The source-initialized global `0xb060=1`, provisional incoming CPU state, one-core execution, writable RAM and untouched tail zero allocation remain disclosed assumptions. This is a bootstrap-vector diagnostic, not measured architectural power-on state.

Run with the original DAT in the external research directory (decoded large segments are unnecessary for this profile):

```sh
python3.13 -m xt4lab run \
  --research-root ../../research/2026-10-14-xt4-firmware \
  --out runs/bootstrap-first-hardware \
  --profile bootstrap-entry-diagnostic --steps 30000
```

The [port-bank investigation](port-bank.md) now describes the first register interface and the following external-input and timer dependencies. The logical port configuration model is implemented; external input samples and timers remain unsupported. Subsequent clock/reset, flash/DMA and operating-system dependencies still need tracing. Preserve both diagnostic baselines. Do not force the context flag, skip calls, inject RTOS globals or return guessed successful hardware responses to manufacture progress.

The new model-backed profile completes the same bootstrap setup and all 97 initial port configuration writes. On attempt 19,614 it stops at PC `0x00004f7c`, reading a 32-bit word from `0xfff41014`, with device `xt4-port-config-v1`, detail `unknown_port_input` and value null. No pin sample is invented. The [port-bank guide](port-bank.md) gives its command and modeled-event evidence.

A supported runtime checkpoint still precedes framebuffer, controls and virtual SD work. Menus remain an unachieved goal.

## Reproduction boundary

Source: the separately obtained official X-T4 2.12 `FWUP0024.DAT`, 51,278,280 bytes, SHA-256 `853d273b5603d3a2b93493f0ebd6a762b0b4b6bdc70317777775ae5f8ed93bc5`. For a slice, read the exact original file range, invert every byte, and compare its hash above. Do not upload the recovered bytes to this repository.

The source/layout findings are static analysis; the bootstrap checkpoint above is observed emulation under its recorded assumptions. Neither establishes architectural processor reset state, all startup branches, interrupts, timers, persistent configuration, calibration, display or storage hardware behavior.
