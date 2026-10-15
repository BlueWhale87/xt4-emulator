# Core-0 interrupt startup exploration

`bootstrap-gic-exploration` continues the unchanged selector2c bootstrap beyond calibration and through its original interrupt-controller setup. It performs nine admitted GIC accesses, including SGIR values `0x01000001`, `0x01000002` and `0x01000003`. These encode SGI IDs1/2/3 targeting all other CPUs; they are not individual CPU target numbers. This profile has only core0, so those broadcasts have no peer recipient.

Original code then requests its next flash-to-RAM copy: source `0xf0140000`, destination `0x00e20000`, length1024. The current covered-media path supports only the128-byte calibration transfer, so it rejects the request at Write32 `0xfffd8060=1`, PC `0x4f34`, after **96,412 raw QEMU-counted instructions**. No context bytes are delivered and no loader return is fabricated. Both port fixtures, repeat and fresh reset reproduce this boundary. Original menus and multicore boot remain unfinished.

```sh
python3.13 -m xt4lab run \
  --research-root ../../research/2026-10-14-xt4-firmware \
  --out runs/gic-first \
  --profile bootstrap-gic-exploration \
  --scenario zero-ports-v1 --steps 250000 --fast
```

Use a fresh output directory. The selector2c identity, ideal sampling and immutable calibration coverage are inherited from the [calibration profile](calibration-startup.md#alternate-selector-0x2c-branch); older profiles retain their original boundaries and hashes.

The adapter forwards admitted accesses to the actual pinned QEMU GIC implementation. Its declared virtual contract is GICv2, one CPU,64 interrupt IDs, five implemented priority bits, security and virtualization extensions disabled, standard disabled reset and layout at virtual CBAR `0xf8f00000`. Firmware helpers support the distributor/CPU-interface mapping; the actual camera inventory, reset state and core release ordering are unmeasured.

Only aligned32-bit accesses in the adapter's documented register subset are supported: distributor control/identity, two enable and pending banks, read-only active banks,64 priority bytes as words, read-only target/configuration words and SGIR; CPU control, priority mask, binary point, acknowledgement, EOI and running/pending priority. Unsupported offsets, directions, widths, reserved SGIR fields or out-of-contract controls reject before backend mutation. Additional GIC features require explicit extensions.

The CPU observer delegates modeled IRQ entry to the inherited Arm implementation in this profile. An anonymous diagnostic guest verifies an actual IRQ vector, IAR read, EOI write and architectural return to the interrupted mainline. That test does not claim the original bootstrap has taken an IRQ: its observed path initializes the distributor, sets the priority mask and broadcasts to absent peers. Other architectural exceptions remain strict stops. Tests also cover pending/active acknowledgement, spurious acknowledgement, all-other target filtering, reset and bounded continuation.

No camera connection, physical storage or network backend is present. Next work is the evidenced context-copy and loader dependency, followed by required multicore, storage, display and input support.
