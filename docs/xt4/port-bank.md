# First bootstrap register bank

Static analysis of the pinned X-T4 2.12 bootstrap, 15 October 2026. The new logical model has observed the 97 initial configuration writes and the first unresolved input read described in [the startup checkpoint](startup.md). Timer transactions below remain predictions from instructions and initialized tables, not an executed device trace.

## Recovered software interface

The driver addresses sixteen ports, sixteen pins per port and eight 32-bit register selectors:

`address = 0xfff41000 + port * 0x40 + selector * 4`

The generic writer is at `0x41ec`, with the actual word store at `0x4f34`. The reader is at `0x4440`, with word load at `0x4f7c`. Invalid port/selector arguments produce no transaction in these helpers. Other access widths and reserved offsets have not been characterized.

| Selector | Port offset | Reconstructed relationship |
| --- | --- | --- |
| 0, 1, 2 | `+0`, `+4`, `+8` | Single-bit setters send upper-half write enables and lower-half values; getters consume the corresponding low-half bit. |
| 3, 4 | `+0xc`, `+0x10` | Two-bit fields for pins 0–7 and 8–15, using upper-half write enables and low-half field values. |
| 5 | `+0x14` | Per-pin getter consumes one sampled bit. Initialization reads this selector but does not write it. |
| 6, 7 | `+0x18`, `+0x1c` | Full 32-bit words with sixteen two-bit fields. Per-pin setters preserve other fields through read/modify/write. |

For selectors 0–4, the setters strongly imply logical low-half updates of `mask = command >> 16`, `data = command & 0xffff`, `state = (old & ~mask) | (data & mask)`. This is a software-required interpretation, not a measured hardware law. Upper readback bits, reserved bits, reset values and electrical effects remain unknown.

A separate register at `0xfff41500` has an indexed one-bit mask/value setter and reader. It lies outside the sixteen port slots and is not used by the initial sequence. Its physical role is unknown.

“GPIO-like port control” is an inference from the addressing and bit APIs. No vendor peripheral name, pin-to-camera-control mapping, interrupt behavior, pull behavior or input/output coupling has been recovered. A flat read/write RAM block would mishandle command masks and sampled inputs.

## Development model checkpoint

`xt4-port-config-v1` now implements a bounded logical configuration model in `hw/gpio/xt4-ports.c`, enabled only by the fixed `bootstrap-port-config-diagnostic` profile. Register-level tests and original-firmware continuation now pass: 19,613 original instructions, 97 modeled writes, then a stop at the first unknown external input. The two earlier startup diagnostics retain their original stops.

The model tracks values and known bits separately. Masked commands define only the selected low-half bits; their upper readback bits remain unknown, so full-word reads still fail. Selectors 6/7 become fully readable only after a full-word write. Unknown reset state and external samples never become invented zeros. Selector 3/4 half-field masks, reserved offsets, input writes, unsupported widths and misalignment are rejected before model state changes. Guest device-memory misalignment can instead trap in the CPU before MMIO; the diagnostic preserves that architectural stop.

No electrical pin behavior, interrupts, host GPIO, input fixtures or timer behavior is added. The two earlier profiles retain their original startup state, device absence and stops. This is an inferred software interface, not a measured peripheral implementation.

## Next dependencies

Initializer `0x1244` writes selectors **6, 3, 4, 2, 0, 1** for every port: 96 writes. It then writes selector 7 of port 2 at `0xfff4109c`, value `0x00015555`. Its source tables begin at `0xa188`, with 28-byte rows.

Next it reads selector 5 for all sixteen ports, storing samples at `0x10aec + 4*port`. The first is **`0xfff41014` at PC `0x4f7c`**, now verified in the new model-backed diagnostic. The failed read returns no pin sample. Those external input samples cannot be inferred by echoing earlier configuration commands. A future model without an explicit input fixture should stop at that read. Any synthetic fixture must remain labeled.

If those inputs are supplied, the next separate device access is a timer-like block at **`0xfff3c348`**, through the same generic store instruction. Static conversion code uses 54,000,000 and a denominator of 1,000; its first computed value is 540,000 (`0x83d60`), written three times, followed by 539,999 (`0x83d5f`) at `0xfff3c34c` three times. The real clock source, running counter, timing and interrupt behavior are unverified.

The inspected port initialization has no busy-poll or interrupt-acknowledgment loop. This bounded observation does not establish peripheral readiness or the effects of pin changes. Neither a port configuration model nor a timer write sink would establish camera menus or a working peripheral.

## Source boundary

These findings come from the recovered bootstrap SHA-256 `bf2d1d4ca9d1d60e5125a964988a3f1852e177c3176d35dc59dcc63afe41f6b1`, with exact extraction offsets in [startup.md](startup.md). The complete port helper range `0x4154..0x4908`, initializer `0x1244`, generic accesses and immediate timer continuation were inspected. A matching driver copy was found in the decoded primary image; computed aliases and other code paths can remain undiscovered. Vendor binaries, disassembly and decompiled evidence remain outside this repository.

## Reproduce and inspect

```sh
python3.13 -m xt4lab run \
  --research-root ../../research/2026-10-14-xt4-firmware \
  --out runs/ports-first-input \
  --profile bootstrap-port-config-diagnostic --steps 30000
```

Expected: 19,613 completed instructions and stop on attempt 19,614 at `0xfff41014`, detail `unknown_port_input`. Open `report.md` for the readable result. `device-events.json` contains the 97 successful state updates plus the rejected input read, with state/known masks and source evidence. CPU and device-event hashes are separate. Virtual reset preserves earlier segment files while the report counts modeled operations in the latest segment only.

QEMU currently runs without a display window. These reports show execution progress; original camera menus require later display, input, storage and platform modeling.

## Explicit synthetic input exploration

`bootstrap-input-exploration` adds `xt4-static-port-input-v1`. It requires an explicit `--scenario zero-ports-v1` or `--scenario one-ports-v1`, declaring sixteen static 32-bit zero or all-one samples. Neither fixture establishes real pin levels, button wiring or a normal camera state. The three earlier diagnostics remain unchanged and never inherit synthetic samples.

Original firmware has now completed the 97 configuration writes and all sixteen input reads/cache stores under both fixtures, then stopped at the first unmodeled timer write: PC `0x4f34`, address `0xfff3c348`, width 4, value 540,000. The input values do not choose this first timer path. This is a synthetic exploration checkpoint; original menus and physical compatibility remain unestablished.

```sh
python3.13 -m xt4lab run \
  --research-root ../../research/2026-10-14-xt4-firmware \
  --out runs/inputs-first-timer \
  --profile bootstrap-input-exploration --scenario zero-ports-v1 --steps 30000
```

The full scenario and canonical scenario hash are retained in run metadata and reset evidence. Each successful synthetic sample has a distinct model identity and semantics in `device-events.json`. Configuration/reset tests prove samples are independent of configuration writes, while virtual reset clears configuration knowledge and retains the selected static fixture. No timer or input interrupt behavior is added by this profile.
