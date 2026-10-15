# Native detector platform contract

The library and host adapters are independent of the X-T4 firmware. Firmware preview and graphics adapters currently return UNAVAILABLE for every profile. No research address is cast to a function pointer or called.

## Memory and ABI

The caller supplies a 128-byte-aligned region and an explicit ceiling. The core reserves context, filters, scratch, one 76,800-byte prepared image, results and pipeline/overlay state inside it. Model constants and code are reported separately; stack observations are separate. Host default is 8 MiB, not a camera budget. All API structures with headers require their exact struct_size and ABI version 1. Pointer layouts follow the compiled target ABI.

There is one live detector and one pipeline. API calls use nonblocking try guards and return BUSY on contention. A registered pipeline/overlay prevents detector destruction. Stop external calls before destroying their handles; destroyed opaque pointers cannot be reused. Worker destruction returns BUSY while work is active. Overlay destruction must run on its validated graphics task and precede pipeline destruction.

## Frame provider

try_acquire returns a completed read-only BGR8 view, accessible extent, full dimensions/stride, visible region, capture timestamp, display identity, geometry generation and an ownership token. A successful acquisition must be released exactly once. Acquisition, copying and release happen outside the UI task; inference starts only after release. The input and prepared output must not overlap. Unsupported formats must be converted through a separately verified adapter, not guessed.

The provider must establish producer completion/reuse and cache coherency for the selected mode. This project has no validated camera lease API. Getter chain 0x2197f80 → 0x121a440 → 0x122c6a0 reads a PIC address field at ctx+0xd4+layer*8; setter chain 0x2197fcc → 0x1219fdc → 0x122c624 updates it. The address is not proof of a valid frame lease. Observe arguments and completion events before reading pixels.

Exclude 0x121c49c, which writes a diagnostic file and waits, and eglQueryFrameDMP at 0x13b9eac, whose helper clears surface+0xa0. Work-area setters copy geometry, not preview pixels.

## Results and controls

Initialize disabled, then select recorded or explicitly configured synthetic mode. Verified-preview mode is unavailable. There is no input queue. get_state returns the current epoch; callers must invalidate on geometry/display changes and pass the matching epoch and generation to rendering. Invalidation works while the platform snapshot lock is busy. Internally, bounded lock-free 32-bit epoch updates avoid implicit ARM32 locks; saturation or repeated contention disables visibility until pipeline reinitialization.

Snapshot synchronization callbacks must be nonblocking try-enter/release operations with appropriate cross-core memory ordering. The implementation also uses a short lifecycle try guard. Neither inference nor graphics calls run while the snapshot lock is held. BUSY drops a read/sample; bad frames invalidate results, and internal/allocation failures latch the worker disabled until reinitialization.

Frame capture, worker and rendering clocks must use the same monotonic nanosecond timebase. Missing/reversing clocks and future captures suppress results. Results older than 250 ms are suppressed by default, including inference delay. Set backend.max_result_age_ns before overlay initialization to override that timeout; zero selects the default. This is an age limit, not a promised inference latency. Worker timing queries return BUSY while a sample runs, avoiding torn 64-bit measurements on ARM32.

## Graphics backend

Observe 0x1247120 and 0x124688c to recover task/context ownership, surface alternation, clear/draw/flush/completion order and a valid insertion mechanism. 0x1248bb8 uses vgDrawPath; vguRect at 0x137b58c is a primitive, not a callback. The callback array drained after presentation is not a proven drawing hook. Match firmware hash and runtime mapping before using these observation points.

Initialize resources on the verified graphics task. required_pool_bytes must cover fixed CPU-side state for 16 rectangles; init must unwind any partial external resources on failure. The backend owns no per-frame allocation. Any external graphics-driver allocations require separate measurement before camera use.

save_state/restore_state preserve all changed matrix, clip, paint, blend and stroke state. set_style selects display-pixel coordinates, the given viewport, opaque green, unfilled rectangles and a two-pixel stroke. No clear, swap, context switching or PIC write operation exists in this adapter interface. Normal UI redraw removes old boxes.

validate_identity must verify the current epoch, geometry generation and display immediately before drawing. Drawing runs after the normal clear/UI objects and before surface completion. Graphics failures invalidate results and latch the overlay disabled. A failed restore means UI state is unknown; disabling prevents further drawing but cannot prove restoration succeeded. Keep firmware integration unavailable until backend ownership and restoration behavior are established.

## Evidence and QEMU handoff

profiles/xt4-2.12-evidence.json contains 13 disabled static candidates, their evidence IDs and unresolved conditions. Official firmware 2.12 SHA256 is 853d273b5603d3a2b93493f0ebd6a762b0b4b6bdc70317777775ae5f8ed93bc5. Analysis addresses come from synthetic ELF SHA256 2de7f18409a5a4a96f5da2b3b4e76022f81040f3ba573146bb62922f31c08249; runtime addresses and the firmware call convention still need confirmation.

Before enabling any future binding, retain original-firmware traces establishing mapping/ABI, logical display/layer, format/extent/stride, lease/cache behavior, scheduling, task/context ownership, image-to-display transforms and available RAM. Reject unknown hardware behavior rather than substituting success. Host fake graphics captures are labeled diagnostic outputs and are not firmware screens.

The validated generic ARM smoke payload is a standalone arithmetic/runtime diagnostic. It does not run original firmware, validate these hooks or measure X-T4 speed. This component is built separately from the camera emulator; importing it does not change guest behavior.

## Standalone ARM diagnostic

The `arm-soft` preset uses Arm GNU 15.2.Rel1, ARMv7-A little-endian ARM state, software floating point, no SIMD, no exceptions/RTTI and explicit libm/libgcc libraries. This profile is deliberately provisional until camera ABI evidence exists. `arm/runtime.cpp` implements memory operations and a single-core errno slot; it supplies no heap, filesystem, threading, graphics or dummy syscall success. Startup disables interrupts, initializes data/BSS and a 128 KiB marked stack, then runs trusted embedded fixtures at diagnostic RAM base 0x40010000. Semihosting is used only for diagnostic text and exit.

The audit records attributes, executable entry and segments, all archive unresolved symbols and providers, final symbol closure and section sizes. Generic QEMU passed all six reference fixtures; observed stack high-water is workload-specific and excludes future pipeline/backend stacks. The supplied 8 MiB region is reserved in BSS even when less is live. All hooks remain disabled. Before camera use, establish loader/relocation policy, actual ISA/call convention/FPU state, RAM ownership, task priority/preemption, synchronization/cache semantics and frame/display contracts above. Do not load this ELF into the physical camera or treat its link address as a camera address.

Build/run steps and checksummed artifacts are in [public build instructions](../README.md).
