# Causal flash initialization exploration

`bootstrap-flash-exploration` executes the unchanged bootstrap through its flash
initializer. It then stops on the first unsupported PHY-enable configuration
write: PC `0x4f34`, address `0xfffd8000`, value `0xc10000c9`, width4. The observed
count is **23,565 QEMU raw engine-counted instructions** under either explicit
port fixture. This is exploratory startup progress. Original menus, display,
controls, SD/settings, interrupts and the full camera remain unimplemented.

```sh
python3.13 -m xt4lab run \
  --research-root ../../research/2026-10-14-xt4-firmware \
  --out runs/flash-startup-zero \
  --profile bootstrap-flash-exploration --scenario zero-ports-v1 \
  --steps 100000 --fast
```

Use a new output directory. Build and prepare your own pinned update as described
in [public setup](public-setup.md). The repository distributes no vendor images,
decoded bytes, disassembly or private run output. Execution has no camera,
USB, physical-storage or host-network backend. The [previous controller profile](controller-startup.md)
and all earlier diagnostic boundaries remain available unchanged.

## What the original code did

The five completed commands were SPI `9f` (one ID byte), SPI `06` (write enable),
SPI `05` (status with WEL set), SPI `72` (four-byte address0, data2), and OPI
`05/fa` (four address bytes, four dummy cycles, two status bytes). Completion
delays were64,64,64,104,64 virtual nanoseconds from their accepted launches.
The volatile configuration change clears WEL and changes the virtual NOR mode.

The original code cached selector `c2` and its selected descriptor pointer
`c1bc`. Its final successful initialization writes include Config `410800c1`,
read instruction `ee`, size `12`, delay `08080808`, SRAM partition `606`, PHY
configuration `80200020`, extensions `11edfa00` and variant word `10`.
It subsequently constructs the nonzero probing address and enters calibration,
where it attempts the unsupported PHY-enable write. No routine is skipped or
replaced with a successful return.

At this checkpoint the successful device operations comprise195 writes,
125 reads, two clock-apply transitions and five NOR completions. The failing
PHY write is retained as a failure, excluded from successful access counts.
Repeat and fresh-engine reset checks compare event hashes and timestamps.
Compact traces count raw QEMU execution, not retired instructions or completed
single steps; see [execution semantics](execution.md).

## Declared virtual contract

The virtual part is specifically **MX25UM51245GXDI00**, a public SPI-start option
with JEDEC ID `c2 80 3a`. This is a selected model, not an identification of the
chip fitted to an X-T4. Its boot subset is documented in [NOR core](nor-boot.md).
Its source is the [Macronix PM2183 v1.6 datasheet](https://www.macronix.com/Lists/Datasheet/Attachments/9241/MX25UM51245G%2C%201.8V%2C%20512Mb%2C%20v1.6.pdf).

The fixed scenario declares a settled power-on state,64ns logical transaction
latency and40ns typical volatile-configuration settling. The latter is a
published typical value, without a published maximum. It is included in logical
completion rather than claimed as a measured wire waveform. There is no
invented WIP pulse for this volatile operation.

Controller initial command0, extensions `13edfa00`, write-completion `00010005`
and variant word0 are explicit reference-profile choices. Published controller
integration resets differ. Unknown logical configuration words remain unreadable
until written. Idle is computed from the pending command, independently of NOR
WIP. The adapter uses Config dual-opcode bit30 to choose early STIG SPI versus
OPI-DTR framing. This is a declared inference from the firmware flow, not proof
of electrical lanes/rates. Global Config DTR bit24 alone does not explain that
flow. Variant F0 bit4 is retained without assigning an unproven meaning.

The register fields follow the public Cadence-compatible implementations in
[Linux](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/drivers/spi/spi-cadence-quadspi.c)
and [U-Boot](https://github.com/u-boot/u-boot/blob/ece349ade2973e220f524ce59e59711cc919263f/drivers/spi/cadence_qspi_apb.c).
Sticky status bit14 and its separate enable register follow the selected
[AMD reference](https://docs.amd.com/r/en-US/am012-versal-register-reference/IRQ_Status-OSPI-Register?contentId=uscNCZyI08y0QHrgm~Jw1w):
an enabled completion latches status, status is W1C, and disabling the enable
register does not acknowledge status. There is no guessed CPU interrupt route.

## Lifecycle and strict boundaries

The controller prepares a generation-bound NOR effect without changing the chip,
then schedules an external virtual deadline. Response bytes become available
only when that matching effect commits. A new launch invalidates old data;
unsupported commands, widths, offsets, framing, unknown phase data and signed
deadline overflow reject before changing state. Busy writes reject atomically.
Read-data registers expose only response-covered words, zero padded to32bits.

Debugger pauses freeze virtual time and preserve the pending operation. Reset
cancels it and restores declared state. A terminal board stop cancels pending
effects; the instruction budget wins at a tied completion deadline regardless
of timer insertion order. A new runner segment uses a fresh engine and cannot
receive an abandoned callback. The asynchronous failure path retains the first
stop with null guest width/value.

The profile intentionally rejects PHY enable, lock/training registers, array
reads, indirect transfers and DMA. Those are subsequent hardware dependencies,
not always-ready flags. The supplied tests cover pure NOR effects, controller
timing/IRQ/data/reset behavior, exact typed C/Python scenario identity, original
commands and final writes, pause/resume, tied budgets and deterministic repeats.

Only this explicitly named profile instantiates the flash device. Reports retain
the fixed contract and its hash, separate NOR transitions from controller MMIO
counts, and label all results as synthetic exploration. No claim of physical
camera compatibility or complete boot follows from this checkpoint.
