# Native face detection for the X-T4 laboratory

A scalar FP32 face detector for recorded frames and standalone ARM diagnostics. It is built independently of QEMU. Importing it does not enable firmware hooks or change the camera machine. Faces come first; animal detection and autofocus control are future work.

## Build and test

From the emulator repository root, with CMake >=3.24, Ninja, Clang and Python 3 installed:

```sh
python3 scripts/test-xt4-native.py
```

Or from this directory:

```sh
cmake --preset host
cmake --build --preset host
ctest --test-dir build/host --timeout 90 --output-on-failure
```

The portable core/tests build on Linux and macOS. The recorded-frame runner uses macOS system crypto and binary-size tools and is built on macOS only. OpenCV is optional for regenerating fixture images; ordinary builds/tests use the checked-in fixtures and do not need it. The reference runner is a separate executable and never shares adapted inference objects. Configuration verifies the upstream source hashes before generating reference sources.

On macOS, run the checked-in astronaut image:

```sh
build/host/xt4_face_run --input tests/fixtures/astronaut.bgr --width 160 --height 160 --stride 480 --mode recorded --warmup 20 --samples 200 --report runs/astronaut.json --overlay runs/astronaut.ppm
```

Use `mkdir -p runs` first if needed. The runner never opens a camera. Synthetic mode draws a fixed test box and labels inference unavailable; verified-preview mode refuses unavailable bindings. Input files, generated output, installed tools and binaries are ignored. Source images and output/measurement buffers sit outside the supplied detector region; region peak is not total process RAM.

## Standalone ARM

Use the official notarized Arm GNU **15.2.Rel1 darwin-arm64 arm-none-eabi** package named in `tools/toolchain-lock.json`. Verify its package SHA256 and installer signature, then extract it locally without running installer scripts:

```sh
pkgutil --check-signature arm-gnu-toolchain-15.2.rel1-darwin-arm64-arm-none-eabi.pkg
pkgutil --expand-full arm-gnu-toolchain-15.2.rel1-darwin-arm64-arm-none-eabi.pkg .tools/arm-expanded
export XT4_ARM_TOOLCHAIN_ROOT="$PWD/.tools/arm-expanded/Payload"
cmake --preset arm-soft
cmake --build --preset arm-soft
python3 tools/check_arm.py --elf build/arm/xt4_face_smoke.elf --archive build/arm/libxt4_face.a --report runs/arm-audit.json
python3 tools/run_arm_smoke.py --qemu /path/to/generic/qemu-system-arm
```

The audit resolves relative library paths inside that root and verifies their recorded contents before using toolchain utilities. This Apple Silicon toolchain lock is intentionally exact; other compiler packages require a separately validated lock rather than silently skipping hashes. The camera-specific QEMU build lacks `virt`; use a separate generic QEMU tool for this diagnostic. Runtime networking is disabled. Semihosting is enabled only for trusted diagnostic console/exit; the diagnostic has no guest filesystem operations.

The ELF runs on generic `virt`, ARMv7-A Cortex-A7, one CPU, software floating point. It embeds all six fixtures and checks ordered float boxes. This validates standalone arithmetic/runtime execution, not camera ABI, rendering hooks or X-T4 speed. Pipeline/overlay code is included in the archive but excluded from that smoke workload.

## Preview and overlay boundary

See the [platform contract](docs/platform-contract.md) and [current hook readiness](../../docs/xt4/preview-overlay-readiness.md). PIC pointer access alone is not a frame lease. Enabling camera bindings requires observed frame format/stride, completion/reuse/cache rules, graphics task/context ownership, transform generations, memory budget and actual calling convention. All 13 address candidates in `profiles/xt4-2.12-evidence.json` remain disabled.

The host overlay tests cover clipping, both logical displays, stale results, enable/disable, nonblocking snapshots, graphics failures and state restoration. A host CPU raster or synthetic box is not an original-firmware screenshot.

## Provenance and notices

`import-manifest.json` records the reviewed native source commit and original file hashes. The imported core is unchanged; integration adjusts build/toolchain provenance and documentation. Private benchmark reports, compiler installations, original firmware, decoded images, decompilation and generated ELF/archive files are not distributed here. The preceding private experiment measured about 8.15 ms per graph on an Apple M3 Pro; no X-T4 inference timing has been measured.

The graph adaptation and rounded FP32 coefficients derive from libfacedetection at `acf7b254121927e7dced30e233a2e03119f28ea2`. Retain its [BSD 3-clause notice](vendor/libfacedetection/LICENSE). The NASA astronaut sample and its derived fixtures are retained from the native experiment; `tests/fixtures/manifest.json` records source hash and transformations. This model variant is distinct from the earlier ONNX prototype. See [NOTICE](NOTICE) and the repository's licensing files.
