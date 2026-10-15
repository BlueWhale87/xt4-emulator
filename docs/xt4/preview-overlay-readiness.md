# Preview and overlay readiness

The native detector is now included under [contrib/xt4-native-detection](../../contrib/xt4-native-detection/README.md). Its host tests and standalone ARM diagnostic can run independently of camera startup. **The pinned camera bench does not reach preview or menu rendering yet, so live firmware bindings remain unavailable.**

The initial flash/module/getter checkpoint uses emulator commit `924be63b55ad37c2d808477d5589af43ab0a63b1`; the latest PHY continuation follows upstream commit `5d16d9c`. Both use official X-T4 firmware 2.12 SHA256 `853d273b5603d3a2b93493f0ebd6a762b0b4b6bdc70317777775ae5f8ed93bc5`, and the existing declared startup assumptions. [Machine-readable evidence](preview-overlay-readiness.json) contains stop records, identities and private-trace digests without vendor bytes or local source paths. The main checkout is under active development; each observation retains its own engine identity and does not predict future DMA models.

## What ran

| Check | Observation | Meaning |
| --- | --- | --- |
| PHY exploration, explicit zero-port fixture | 23,842 raw engine-counted instructions; PC `0x4f34`, unsupported write `0x01002a2c` to `0xfff4a500` | PHY reset/lock modeled; DMA routing and calibration remain unresolved |
| Flash exploration, explicit zero-port fixture | 23,565 raw engine-counted instructions; PC `0x4f34`, unsupported write `0xc10000c9` to `0xfffd8000` | Flash PHY setup remains a startup dependency |
| Post-loader module diagnostic | Eight completed single steps; ninth attempts byte read at `0x00e20008`, PC `0x01291024` | Synthetic module entry also stops before UI startup |
| Isolated PIC getter `0x122c6a0` | Four cases execute the original 44 bytes / 11 ARM instructions each | Leaf accessor behavior corroborated with a synthetic caller |
| Native graph / worker / host overlay | Independent contract tests pass | These tests do not validate camera preview hooks |

Reproduce the startup boundaries with your own prepared firmware and a new output directory, following [public setup](public-setup.md):

```sh
python3.13 -m xt4lab run --research-root ../../research/2026-10-14-xt4-firmware --out runs/preview-phy --profile bootstrap-phy-exploration --scenario zero-ports-v1 --steps 100000 --fast
python3.13 -m xt4lab run --research-root ../../research/2026-10-14-xt4-firmware --out runs/preview-flash --profile bootstrap-flash-exploration --scenario zero-ports-v1 --steps 100000 --fast
python3.13 -m xt4lab run --research-root ../../research/2026-10-14-xt4-firmware --out runs/preview-module --profile module-entry-diagnostic --steps 1000
python3 scripts/test-xt4-native.py
```

Flash exploration retains its synthetic input/timer/part/reset/timing assumptions. PHY exploration additionally declares the readiness/lock timing described in [the PHY checkpoint](phy-startup.md); lock is not proof of successful calibration or a data transfer. No original routine was skipped or replaced with a successful device response to reach an overlay.

## PIC getter: limited runtime corroboration

Static REA evidence `ev_fd0d146f10412aa45e3851eb95cc06f18edb689a6eabc543cd499083f05c9818` identifies an unsigned layer check and the field at `ctx+0xd4+layer*8`. Its source is the synthetic analysis ELF SHA256 `2de7f18409a5a4a96f5da2b3b4e76022f81040f3ba573146bb62922f31c08249`; this mapping remains provisional for a normal camera run.

The isolated experiment staged the unchanged module-entry images in the existing machine, stopped the CPU, then set a synthetic ARM caller: `r0=0x7fff0100` (context), `r1=layer`, `r2=0x7fff0400` (output), `sp=0x7fffff00`, `lr=0x7fffee00`, `cpsr=0x1d3`, `pc=0x122c6a0`. Only the profile's synthetic stack RAM was initialized through the debugger; no firmware instruction was patched. It seeded context fields `+0xd4=0x12345678`, `+0xdc=0xabcdef00` and output sentinel `0xdeadbeef`, stepped at most 40 instructions and stopped upon return to the synthetic LR.

Layers 0 and 1 returned success and the selected field. Layers 2 and `UINT32_MAX` returned `0xffffffe2`, preserving the output sentinel. Context bytes were unchanged in every case. Each executed instruction sequence matched the original getter bytes. Raw traces and vendor bytes remain private; their hashes are recorded in the JSON.

This proves only the leaf's field-access/rejection behavior under the supplied registers and mapping. It does not establish the real display context, normal calling convention, active PIC layer, pixel format, stride, cache completion or a read-only frame lease. Neither seeded pointer was treated as a pixel buffer. The getter wrapper `0x121a440` and real callers still require normal execution traces. No setter, file-dump routine or state-changing graphics query was invoked.

## Overlay: next observation point

Static REA evidence `ev_099e3b6019cc74f03577662347612629f699456a71189bf1678456cb9b4791f2` corroborates this render path:

- `0x12468dc`: initial clear.
- `0x12468fc`: UI-object walk.
- `0x1246a0c`: optional second clear, followed by a branch back to `0x124690c`.
- `0x124690c`: graphics flush.
- `0x1246924`: surface-completion call.

**Observe immediately before `0x124690c` once this function is naturally reached.** This point follows both the object walk and the optional clear; drawing immediately after the object walk could otherwise be erased. It is an observation candidate, not an enabled insertion hook. The update path at `0x1247120` selects surfaces and later drains callbacks after display submission; that callback array is not a proven drawing hook. Update evidence: `ev_591b61b56313d30999bf27e8350de16cec2a63a76eac46bf73d88f3a4b6e55e5`.

Observe task/context identity, clear/draw/flush/completion order, surface alternation and display transforms. Do not call OpenVG with invented initialized state to manufacture a successful overlay. `eglQueryFrameDMP` and the picture-dump routine remain excluded as passive preview taps because they change state or perform file I/O.

## Gates before live detection boxes

1. Reach the preview and UI tasks through original firmware, resolving startup dependencies explicitly.
2. Confirm runtime mappings/calling convention and identify the real display/PIC context.
3. Establish frame dimensions, accessible extent, stride, format, completion/reuse and cache rules.
4. Confirm UI task/context ownership, transform generation and a state-preserving insertion mechanism after the final clear and before surface completion.
5. Establish a target RAM budget and scheduling budget, then connect the existing interfaces and measure interference.

Until those observations exist, all camera bindings stay disabled. Recorded-frame detection boxes and host overlay tests remain available now; QEMU execution counts and Mac inference timings do not measure X-T4 performance.
