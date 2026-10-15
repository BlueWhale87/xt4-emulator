# Covered calibration transfer exploration

`bootstrap-calibration-exploration` executes the unchanged X-T4 2.12 bootstrap through its calibration loop. The original code performs 64 actual 128-byte transfers, compares the delivered bytes, and computes trained=1, midpoint=62, phase=0 and selector=0x107 under the declared ideal sampling policy. It then stops on a 32-bit read at virtual CBAR+0x1000 (`0xf8f01000`), PC `0x34b0`, after 96,742 raw QEMU-counted instructions. This count does not claim retired instructions or a percentage of camera boot.

```sh
python3.13 -m xt4lab run \
  --research-root ../../research/2026-10-14-xt4-firmware \
  --out runs/calibration-first \
  --profile bootstrap-calibration-exploration \
  --scenario zero-ports-v1 --steps 200000 --fast
```

Use a fresh directory for every run. `one-ports-v1` declares the other synthetic input fixture. Both fixtures, a repeated run and a fresh engine reset reproduce the same execution boundary. Reports retain profile, scenario, input, engine and event identities. The current path has 2,380 modeled writes and 2,304 modeled reads; transfer completion produces separate route, DMA and indirect component events, not extra guest accesses.

## Alternate selector 0x2c branch

Select `--profile bootstrap-selector2c-exploration` with either explicit port fixture to execute the original C504 descriptor branch. It supplies `selector2c-spi-boot-v1`: only a one-byte SPI `9f` response is supported. Extra identity bytes, status, write-enable and configuration commands reject. It is an unidentified virtual device, with no claim about a fitted flash part or electrical protocol.

Original firmware selects descriptor `0xc504`, controller size `0x111003`, initial read command `0xcc`, trained read command `0xfd`, extensions `0x0000fa00` and F0 zero. It skips the other part's WREN/CR2/OPI transition. The strict array admission requires the matching SPI tuple; mixing the older part's size, read instruction or dual-opcode configuration rejects before delivery.

The same original calibration code completes 32 transfers in programmed phase16 and 32 in phase17. Its result is trained=1, midpoint=62, phase=0, selector=0x107 and flash selector=0x2c. It reaches the same unsupported GIC read at PC `0x34b0` after **95,402 raw instructions**. All taps remain explicitly ideal; the computed midpoint does not establish physical sampling behavior. No comparison buffer or result cache is prefilled. The reset, latency and coverage policies below are shared; CR2 settling is zero because that operation is unsupported in this subset.

## Data and transaction contract

The only covered source is CPU `[0xf002a080,0xf002a100)`, corresponding to flash offset `[0x2a080,0x2a100)`. Staging derives its 128 bytes from the pinned original DAT and verifies SHA-256 `10fa7918107458a1b72e18ef91b9bfc27b6eb76e3b562aa6566c5561dcf4929a`. Each segment receives a checked immutable input. C independently validates metadata, file type, size and hash before recording execution. The media is never mapped as RAM or copied to a guest destination at initialization/reset.

One coordinator owns distinct request-route, DMA channel 1 and indirect-read state. Its windows are `0xfff4a500`, `0xfffe1010..0xfffe1033` and `0xfffd8060..0xfffd806f`. Only aligned 32-bit accesses are admitted. Unknown readback fails. Descriptor and indirect parameter words may be staged in any order; the entire request is validated when start=1 is written. Unsupported staged words can be read back but cannot cause delivery. Channel control, route values, status acknowledgement and indirect start/done commands have immediate exact-value admission.

An accepted start requires request 0x2a routed to channel 1, authoritative global enable, all descriptor/indirect fields known and agreeing, the supported controller/NOR configuration, completed PHY lock, an even covered source, and a complete destination inside the linked bootstrap RAM. The model snapshots sequential source bytes and prerequisite generations, then schedules delivery after the declared 64 virtual nanoseconds. No bytes, done flag or successful completion appear before delivery. The normal RAM write path performs the full transfer before clearing active bit 16 and setting indirect done bit 5. Programmed source/destination words remain unchanged.

Pending route/descriptor/flash changes reject before mutation. Reset and first terminal stop cancel pending work. Completion rechecks the board guard, global-enable generation, controller/NOR/PHY state and media coverage. Loss followed by restoration of the global gate still invalidates the request. Earlier and tied instruction budgets suppress delivery; wall timeout reaps the paused process; ordinary debugger pauses freeze virtual work. A fresh segment inherits neither callbacks nor delivered RAM.

## Explicit virtual assumptions

- Translation subtracts `0xf0000000` within 64 MiB capacity. It does not mask addresses, fill holes or expose a complete installed flash image.
- All taps 0..127 in programmed controller phases 20 and 21 are ideal. The firmware chooses its result from actual comparisons; no result cache, comparison RAM or function return is fabricated. These are not measured camera data-eye semantics.
- Whole-transfer latency is 64 ns, request-active status uses bit 16, and terminal addresses retain their programmed values. Physical timings, partial progress, status error bits, queues and CPU IRQ routing remain unknown.
- `reset-cbar=0xf8f00000` is an explicit virtual layout set before CPU realization. It keeps the next GIC access outside bootstrap RAM. No GIC is instantiated by this profile, and the camera's actual CBAR is not established.

The original firmware completes calibration under these assumptions. Full platform boot, SMP, interrupt handling, main loader continuation, menus, virtual controls, settings and SD storage are still unfinished. The emulator has no physical camera or networking backend. This result does not establish physical-camera fidelity or safety.
