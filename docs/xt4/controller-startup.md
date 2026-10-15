# Transfer enable and clock-apply exploration

The unchanged X-T4 2.12 bootstrap reaches its first flash-controller write under an explicit synthetic platform scenario. Original menus, display, virtual SD storage and full multi-core startup remain unimplemented.

## Run

After [building and preparing the local firmware](public-setup.md), use a new output directory:

```sh
python3.13 -m xt4lab run \
  --research-root ../../research/2026-10-14-xt4-firmware \
  --out runs/controller-startup \
  --profile bootstrap-controller-exploration \
  --scenario zero-ports-v1 --steps 100000 --fast
```

`one-ports-v1` is the second explicit fixture. Both include the existing port and timer assumptions, sixteen declared initial transfer-global words (all zero or all `0xffffffff`), and a one-nanosecond selection-latch latency. The scenario fingerprint includes these fields. Older profiles omit them and keep their exact prior profile/scenario fingerprints. The two new models cannot be selected on older profiles.

`--fast` records a compact boundary/device trace and raw QEMU instruction accounting. It does not record a complete instruction path or claim acknowledged single-step completions. See [execution controls](execution.md). The older single-step API remains available on this profile too.

## Observed checkpoint

The exploratory run accounts for **21,173 engine instructions**, with **127 successful modeled writes, 35 reads and two clock-apply transitions**. The next unsupported operation is a 32-bit write of `0x000800c1` to `0xfffd8000`, from PC `0x4f34`. The stop carries no stale model label from a previous operation and retains the actual attempted write value.

| Model | Writes | Reads | Transitions |
| --- | ---: | ---: | ---: |
| Port configuration | 97 | 0 | 0 |
| Static synthetic inputs | 0 | 16 | 0 |
| Synthetic timer | 10 | 1 | 0 |
| Transfer global enable | 16 | 16 | 0 |
| Clock selection apply | 4 | 2 | 2 |

The original code performs sixteen ordered read/OR-bit28/write pairs, then stages selections `0x101` and `0x107` and triggers each apply. Each transition occurs at its declared virtual deadline. Both fixtures reach the same stop; their state/event hashes differ because their declared initial words differ. Repeated instances of the same fixture reproduce counts, timestamps and event hashes. Reset starts a fresh process and segment, retaining earlier evidence and clearing latest-segment report counts.

## Modeled interface and limits

The transfer window at `0xfffe0000` spans sixteen `0x1000` banks. Only the aligned 32-bit global word at offset zero in each bank is supported. Reads return the declared fixture word; writes may change bit28 while preserving every other bit. No descriptor, transfer, status, completion, interrupt or data copy is supplied. Reset restores the fixture. The bootstrap accesses support this narrow enable interpretation. The [Linux Milbeaut XDMAC driver](https://github.com/torvalds/linux/blob/v6.12/drivers/dma/milbeaut-xdmac.c) independently uses bit28 for an enable operation but has different channel geometry; it does not establish the camera's exact silicon or reset state.

The selection-latch window at `0xff701000` admits only selection `0x40=0x101`, selection `0x48=0x107` and command `0x80=2`. Selection writes create known shadow state. Unwritten readback remains unknown. A command requires a newly staged selection, snapshots it, becomes busy and schedules an external virtual deadline. Completion updates active logical state and clears the command. This causal handshake is a declared hypothesis; it does not change CPU or timer frequency and is not measured physical timing.

Writes while busy are rejected because queueing/in-flight modification is unestablished. Unsupported values, widths, misalignment, reserved offsets and overflowing deadlines reject before mutation. Reset cancels pending completion and clears known/active/shadow state. There is no unconditional ready response. A debugger pause does not advance the apply deadline.

Device tests cover all sixteen banks, enable/disable/preservation, wide unsigned invalid values, strict CPU faults and no post-fault store, unknown readback, two staged operations, pending/complete boundaries, busy rejection, overflow and canceled callbacks. Firmware tests separately check the original access sequence and both scenarios. These establish the declared virtual subset; they do not establish physical-camera fidelity or safety.

## Next dependency

The first flash write is now observed rather than merely predicted. A flash controller, causal virtual NOR protocol, covered media bytes and later transfer paths must be modeled before module loading can progress. A chip specification alone cannot justify guessed controller-ready bits. No initializer skip, successful return stub or replacement menu is used.
