# Public checkout setup

The first prototype retains its original project-relative input layout. Use this layout to build and run it without changing source paths:

```text
xt4-lab/
  work/xt4-emulator/                 # this repository
  research/2026-10-14-xt4-firmware/
    FWUP0024.DAT                    # obtain separately
    unpacked/segment-260274.bin      # generated locally
    unpacked/segment-5c0274.bin
    unpacked/segment-ae0274.bin
    unpacked/segment-1a00274.bin
```

## Source and build

The tested host is Apple Silicon macOS with Python 3.13, Clang, GLib, pkgconf and Ninja. The runner requires Unix-domain sockets and POSIX file-opening behavior. Windows hosts have not been validated.

```sh
mkdir -p xt4-lab/work xt4-lab/research/2026-10-14-xt4-firmware
git clone --depth 64 https://github.com/BlueWhale87/xt4-emulator.git xt4-lab/work/xt4-emulator
cd xt4-lab/work/xt4-emulator
git fetch --depth 1 https://github.com/qemu/qemu.git tag v11.1.2
python3.13 scripts/build-xt4.py
```

The pin check requires the base release to be an ancestor of the checkout. If a shallow clone does not contain that ancestry, deepen the project checkout before building:

```sh
git fetch --deepen=64 origin
```

The build is local and never installs QEMU globally. Initial dependency acquisition may use the Internet; execution uses local files and a private Unix socket.

## Prepare your firmware

Obtain the official **X-T4 2.12** update from [Fujifilm support](https://fujifilm-x.com/global/support/download/firmware/cameras/x-t4/). This repository does not download or redistribute it. Put `FWUP0024.DAT` in the external research directory shown above, then from the repository root:

```sh
cp contrib/xt4-offline/xt4_unpack.py ../../research/2026-10-14-xt4-firmware/
python3.13 ../../research/2026-10-14-xt4-firmware/xt4_unpack.py
```

The decoder verifies the exact input hash before decoding. It checks declared lengths and backward-reference bounds and produces a manifest. The algorithm is reconstructed from the update file; decoded output has not been compared to a camera RAM dump. Re-running the decoder replaces its local decoded outputs; keep the original DAT read-only.

Keep firmware and decoded files outside the repository. Run directories contain private firmware copies and host paths and are ignored by Git; do not upload them wholesale.

## Verify and run

After building and preparing the firmware:

```sh
python3.13 -m unittest discover -s contrib/xt4-offline -v
python3.13 scripts/test-xt4.py
python3.13 -m xt4lab run \
  --research-root ../../research/2026-10-14-xt4-firmware \
  --out runs/first-firmware-trace \
  --profile module-entry-diagnostic --steps 1000
```

The full suite includes both diagnostic profiles and therefore requires your local update and decoded inputs. The bootstrap profile alone needs only the original DAT and derives its small checked image at staging time. It also exercises the built QEMU machine. The decoder's small unit suite needs no firmware or camera.

Use a new output directory for each run, without commas in its path. Expected current result: eight completed original instructions followed by `unsupported_access` at PC `0x01291024`, reading one byte at `0x00e20008`. Read `report.md` in the output directory. This is a dependency trace, not a successful camera boot.

`--profile validation` deliberately refuses to run because authentic boot state is incomplete. See [the runner guide](foundation.md) for pause, reset and reporting controls.

The [bootstrap investigation](startup.md) includes the newer profile and its 14,731-instruction hardware checkpoint. Use its `--steps 30000` command to reproduce that path.

The [port configuration model guide](port-bank.md) has the latest `bootstrap-port-config-diagnostic` command. It also needs only the original DAT. It records 97 modeled writes and stops on an unknown external input after 19,613 instructions. QEMU runs headlessly at this stage; view the generated `report.md` and `device-events.json`. A native camera menu window is not yet implemented.


For the next checkpoint, select `--profile bootstrap-input-exploration` with an explicit `--scenario zero-ports-v1` or `one-ports-v1`. The [port guide](port-bank.md#explicit-synthetic-input-exploration) describes these synthetic fixtures and the verified first timer write. Omitting the scenario fails before staging; supplying it to a diagnostic profile is rejected. This profile remains headless and cannot establish authentic boot state.

For the timer checkpoint, use `--profile bootstrap-timer-exploration --scenario zero-ports-v1 --steps 30000` with a fresh output directory. The [timer guide](timer-startup.md) describes its explicit 54 MHz/initially-halted assumptions, 20,680-instruction result and next unknown controller read. Reports split 107 writes / 17 reads by model and retain deterministic timing. `one-ports-v1` reaches the same boundary with different declared input words.

The latest checkpoint adds transfer enable and clock-apply exploration. Use `--profile bootstrap-controller-exploration --scenario zero-ports-v1 --steps 100000 --fast` with a fresh output directory. The [controller guide](controller-startup.md) explains the additional synthetic assumptions and the first flash write after 21,173 engine-counted instructions. The [execution guide](execution.md) distinguishes compact traces from single stepping. A camera menu window remains unimplemented.
