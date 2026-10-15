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
class ExplorationScenario:
    scenario_id: str
    port_inputs: tuple[int, ...]
    timer: TimerScenario | None


def load_scenario(name: str, *, timer: bool) -> ExplorationScenario:
    if name not in ('zero-ports-v1', 'one-ports-v1'):
        raise ValueError('Unsupported synthetic scenario')
    return ExplorationScenario(name, (0 if name == 'zero-ports-v1' else 0xffffffff,) * 16,
        TimerScenario(54000000, True, True, 0, True) if timer else None)


def validate_scenario(scenario: ExplorationScenario, *, timer: bool) -> None:
    if not isinstance(scenario, ExplorationScenario):
        raise ValueError('Explicit synthetic scenario required')
    expected = load_scenario(scenario.scenario_id, timer=timer)
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


def scenario_dict(scenario: ExplorationScenario | None) -> dict | None:
    return json.loads(json.dumps(asdict(scenario))) if scenario is not None else None


def scenario_from_dict(data: dict, *, timer: bool) -> ExplorationScenario:
    if not isinstance(data, dict):
        raise ValueError('Missing synthetic scenario declaration')
    expected = load_scenario(data.get('scenario_id'), timer=timer)
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
