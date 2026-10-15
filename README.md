# X-T4 offline emulator research

An experimental Fujifilm X-T4 machine for QEMU, with a bounded execution runner and reproducible dependency reports. The long-term goal is to run the original camera menus and virtual SD storage before developing an SD recipe manager.

**Current result: original bootstrap execution completes flash and first PHY initialization, then reaches unsupported DMA request routing after 23,842 engine-counted instructions, with explicit synthetic input, timer, platform, flash and PHY assumptions. Camera menus do not boot yet.** Display, controls, storage, interrupts and full platform startup remain work in progress.

The project runs entirely on the host. Its runner exposes no camera connection, USB passthrough, host networking or physical storage devices. It does not modify or install camera firmware.

## Start here

- [Native face detector and host box tests](contrib/xt4-native-detection/README.md)
- [Preview/overlay hook readiness](docs/xt4/preview-overlay-readiness.md)
- [Build, firmware preparation and testing](docs/xt4/public-setup.md)
- [Diagnostic runner, evidence and limitations](docs/xt4/foundation.md)
- [Startup investigation](docs/xt4/startup.md)
- [Latest PHY initialization and how to run it](docs/xt4/phy-startup.md)
- [Earlier flash initialization](docs/xt4/flash-startup.md)
- [Earlier controller exploration](docs/xt4/controller-startup.md)
- [Fast execution, count semantics and pause controls](docs/xt4/execution.md)

Fujifilm firmware, decoded images, per-camera data and local runs are **not included**. Obtain your own official X-T4 2.12 update. The input is pinned to SHA-256 `853d273b5603d3a2b93493f0ebd6a762b0b4b6bdc70317777775ae5f8ed93bc5`; other versions are rejected. The included offline decoder writes only local files.

## Project status

| Work | Status |
| --- | --- |
| Pinned QEMU Arm CPU engine | Implemented; software translation |
| Checked firmware loading and isolated run controls | Implemented |
| Engine-bounded fast continuation | Compact trace; exact total budgets, pause/resume/reset and failure cleanup tested |
| Precise unknown-memory and exception stops | Implemented |
| Original firmware entry trace | Reproducible; eight completed instructions |
| Original bootstrap execution | Reproducible first hardware write; 14,731 instructions |
| Port configuration model | 97 writes verified; next external input is unknown |
| Full platform startup | Under investigation |
| Explicit synthetic port inputs | Original bootstrap reaches first timer write with either declared fixture; exploration only |
| Bounded timer exploration | 107 writes / 17 reads verified; next unknown controller at `0xfffe0000`; exploration only |
| Transfer enable and clock-apply exploration | 127 writes / 35 reads / 2 transitions; next unknown flash write at `0xfffd8000` |
| Causal flash command exploration | 195 writes / 125 reads / 7 transitions; next unsupported PHY-enable configuration; no array/DMA/PHY training |
| Causal PHY lock exploration | 204 writes / 128 reads / 9 transitions; next unsupported DMA request-routing write; calibration/transfer incomplete |
| Original menu display and virtual controls | Not implemented |
| Virtual SD / settings storage | Not implemented |
| SD recipe manager | Planned after menu and storage emulation |

All profiles assume one Cortex-A7 core, disabled MMU/caches initially and explicitly recorded zero allocation. The original module-entry profile uses a synthetic stack; the bootstrap profiles execute original stack setup. The port configuration model tracks inferred logical settings and unknown bits. These are diagnostic boundaries rather than measured cold-boot reconstructions. Explicit exploration profiles declare and retain synthetic input/clock assumptions. Other unknown hardware stops execution; no implicit successful device response is supplied.

Emulation results apply only to modeled behavior. They do not establish physical-camera recovery or firmware safety.

## QEMU base and licensing

This repository is a development fork of [QEMU](https://github.com/qemu/qemu), based on **11.1.2**, commit `4fc49f46dc95d4a27de2509e7fceb2931e91faeb`. The original [QEMU README](README.rst), [LICENSE](LICENSE), [COPYING](COPYING), notices and file-specific licenses are retained. QEMU as a whole is distributed under GPL version 2; individual files and components may have other compatible terms described in those notices.

This is an independent research project, unaffiliated with Fujifilm. Fujifilm and X-T4 are trademarks of their respective owners.
