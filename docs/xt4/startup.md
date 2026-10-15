# Startup dependency investigation

Status: offline analysis, 15 October 2026. The emulator still stops after eight completed instructions; the newly located regions below have **not yet been integrated into its memory profile**. No camera was accessed for this investigation.

## First missing object recovered

The first diagnostic stop is a byte read at `0x00e20008`, from PC `0x01291024`. The update contains a 1 KiB object for runtime address `0x00e20000` at original DAT offset `0x140274`. Recover each byte by XOR with `0xff`; no decompression is involved.

Its SHA-256 is `41ca21d9445075090441de329567b9fa459f9be4ccf54b7e51ad0637884d8d3e`. The first four words are pointers `0x00e20040`, `0x00e201ec`, the value zero, and pointer `0x00e201cc`. The object includes the module table that corroborates the primary and secondary load addresses.

Bootstrap code copies `0x400` bytes from flash offset `0x140000` to RAM `0x00e20000` on one branch. The update container's payload begins at DAT offset `0x274`, accounting for the different flash and file offsets. Another branch writes word `1` at context offset `+8`. The source bytes alone therefore do not establish the value seen at every module-entry boundary.

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

## Next checkpoint

Trace context writers, support-module initialization and the first hardware dependencies. Then extend the diagnostic profile with independently pinned source slices and record the next precise stop. Preserve the existing trace as a regression baseline. Do not force the context flag, skip calls, or return guessed successful hardware responses to manufacture progress.

A supported runtime checkpoint still precedes framebuffer, controls and virtual SD work. Menus remain an unachieved goal.

## Reproduction boundary

Source: the separately obtained official X-T4 2.12 `FWUP0024.DAT`, 51,278,280 bytes, SHA-256 `853d273b5603d3a2b93493f0ebd6a762b0b4b6bdc70317777775ae5f8ed93bc5`. For a slice, read the exact original file range, invert every byte, and compare its hash above. Do not upload the recovered bytes to this repository.

These findings describe static instructions and data. The public runner has not executed the recovered bootstrap or established processor reset state, startup branch selection, interrupts, timers, persistent configuration, calibration, display or storage hardware behavior.
