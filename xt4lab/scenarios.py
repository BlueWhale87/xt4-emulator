"""Explicit synthetic exploration assumptions, never measured camera state."""
from dataclasses import asdict, dataclass
import hashlib
import json


@dataclass(frozen=True)
class TimerScenario:
    frequency_hz: int
    initial_halted: bool
    enable_preserves_count: bool
    icount_shift: int
    stop_at_terminal_count: bool


@dataclass(frozen=True)
class PlatformScenario:
    transfer_globals: tuple[int, ...]
    clock_apply_latency_ns: int


@dataclass(frozen=True)
class FlashScenario:
    virtual_part: str = 'mx25um51245gxdi00-boot-v1'
    powered_settled: bool = True
    transaction_latency_ns: int = 64
    cr2_typical_settle_ns: int = 40
    initial_command_word: int = 0
    initial_opcode_extensions: int = 0x13edfa00
    initial_write_completion: int = 0x00010005
    initial_variant_word: int = 0
    stig_protocol_policy: str = 'dual-opcode-8d-exploration-v1'


@dataclass(frozen=True)
class PhyScenario:
    master_lock_latency_ns: int = 4
    dll_lock_latency_ns: int = 4
    initial_observable_word: int = 0
    reset_policy: str = 'reset-disable-immediate-clear-v1'


@dataclass(frozen=True)
class TransferScenario:
    route_policy: str = 'request2a-channel1-v1'
    initial_status_word: int = 0
    initial_indirect_control: int = 0
    transfer_latency_ns: int = 64
    source_policy: str = 'f0-covered-span-v1'
    sample_policy: str = 'ideal-taps0-127-phases20-21-v1'
    status_policy: str = 'request-active-bit16-v1'
    terminal_address_policy: str = 'retain-programmed-v1'
    cbar_base: int = 0xf8f00000


@dataclass(frozen=True)
class ExplorationScenario:
    scenario_id: str
    port_inputs: tuple[int, ...]
    timer: TimerScenario | None
    platform: PlatformScenario | None = None
    flash: FlashScenario | None = None
    phy: PhyScenario | None = None
    transfer: TransferScenario | None = None


def load_scenario(name: str, *, timer: bool, platform: bool = False, flash: bool = False, phy: bool = False, transfer: bool = False, selector2c: bool = False) -> ExplorationScenario:
    if selector2c and not (flash and phy and transfer):
        raise ValueError('Selector2c requires its calibration profile')
    if name not in ('zero-ports-v1', 'one-ports-v1'):
        raise ValueError('Unsupported synthetic scenario')
    return ExplorationScenario(name, (0 if name == 'zero-ports-v1' else 0xffffffff,) * 16,
        TimerScenario(54000000, True, True, 0, True) if timer else None,
        PlatformScenario((0 if name == 'zero-ports-v1' else 0xffffffff,) * 16, 1) if platform else None, (FlashScenario(virtual_part='selector2c-spi-boot-v1', cr2_typical_settle_ns=0,
            stig_protocol_policy='spi-selector2c-exploration-v1') if selector2c else FlashScenario()) if flash else None,
        PhyScenario() if phy else None, (TransferScenario(sample_policy='ideal-taps0-127-phases16-17-v1')
            if selector2c else TransferScenario()) if transfer else None)


def validate_scenario(scenario: ExplorationScenario, *, timer: bool, platform: bool = False, flash: bool = False, phy: bool = False, transfer: bool = False, selector2c: bool = False) -> None:
    if not isinstance(scenario, ExplorationScenario):
        raise ValueError('Explicit synthetic scenario required')
    expected = load_scenario(scenario.scenario_id, timer=timer, platform=platform, flash=flash, phy=phy, transfer=transfer, selector2c=selector2c)
    if (type(scenario.port_inputs) is not tuple or len(scenario.port_inputs) != 16
            or any(type(v) is not int or not 0 <= v <= 0xffffffff for v in scenario.port_inputs)
            or scenario != expected):
        raise ValueError('Scenario differs from admitted synthetic state')
    if timer:
        if not isinstance(scenario.timer, TimerScenario):
            raise ValueError('Invalid synthetic clock')
        for key, value in asdict(scenario.timer).items():
            if type(value) is not type(getattr(expected.timer, key)):
                raise ValueError('Invalid clock field type')
    if platform:
        state = scenario.platform
        if (not isinstance(state, PlatformScenario) or type(state.transfer_globals) is not tuple
                or len(state.transfer_globals) != 16
                or any(type(v) is not int for v in state.transfer_globals)
                or type(state.clock_apply_latency_ns) is not int):
            raise ValueError('Invalid platform field type')

    if flash:
        if not isinstance(scenario.flash, FlashScenario):
            raise ValueError('Invalid flash contract')
        for key, value in asdict(scenario.flash).items():
            if type(value) is not type(getattr(expected.flash, key)):
                raise ValueError('Invalid flash field type')

    if phy:
        if not isinstance(scenario.phy, PhyScenario):
            raise ValueError('Invalid PHY contract')
        for key, value in asdict(scenario.phy).items():
            if type(value) is not type(getattr(expected.phy, key)):
                raise ValueError('Invalid PHY field type')

    if transfer:
        if not isinstance(scenario.transfer, TransferScenario):
            raise ValueError('Invalid transfer contract')
        for key, value in asdict(scenario.transfer).items():
            if type(value) is not type(getattr(expected.transfer, key)):
                raise ValueError('Invalid transfer field type')


def scenario_dict(scenario: ExplorationScenario | None) -> dict | None:
    if scenario is None:
        return None
    data = asdict(scenario)
    if scenario.platform is None:
        data.pop('platform')
    if scenario.flash is None:
        data.pop('flash')
    if scenario.phy is None:
        data.pop('phy')
    if scenario.transfer is None:
        data.pop('transfer')
    return json.loads(json.dumps(data))


def scenario_from_dict(data: dict, *, timer: bool, platform: bool = False, flash: bool = False, phy: bool = False, transfer: bool = False, selector2c: bool = False) -> ExplorationScenario:
    if not isinstance(data, dict):
        raise ValueError('Missing synthetic scenario declaration')
    expected = load_scenario(data.get('scenario_id'), timer=timer, platform=platform, flash=flash, phy=phy, transfer=transfer, selector2c=selector2c)
    # Canonical JSON also distinguishes booleans/floats from integer words.
    if json.dumps(data, sort_keys=True, separators=(',', ':')) != json.dumps(
            scenario_dict(expected), sort_keys=True, separators=(',', ':')):
        raise ValueError('Serialized scenario differs from fixed identity')
    return expected


def scenario_sha256(scenario: ExplorationScenario | None) -> str | None:
    if scenario is None:
        return None
    return hashlib.sha256(json.dumps(scenario_dict(scenario), sort_keys=True,
                                   separators=(',', ':')).encode()).hexdigest()
