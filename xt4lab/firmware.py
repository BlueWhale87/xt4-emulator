"""Validate fixed firmware and stage immutable copies for offline execution."""
from dataclasses import asdict, dataclass
import hashlib
import json
import os
from pathlib import Path
import stat
from .startup import ORIGINAL_SHA256, BOOTSTRAP_OFFSET, extract_bootstrap
from .scenarios import ExplorationScenario, load_scenario, scenario_dict, scenario_sha256, validate_scenario

REPO = Path(__file__).resolve().parents[1]
PROJECT = REPO.parents[1]
SOURCE_FILES = ('unpacked/segment-260274.bin', 'unpacked/segment-5c0274.bin',
                'unpacked/segment-ae0274.bin')


@dataclass(frozen=True)
class Region:
    name: str
    base: int
    size: int
    file: str | None
    file_size: int
    sha256: str | None
    initialization: str
    confidence: str


@dataclass(frozen=True)
class BootProfile:
    name: str
    cpu_type: str
    entry: int
    registers: dict[str, int]
    regions: tuple[Region, ...]
    assumptions: tuple[str, ...]
    models: tuple[str, ...] = ()
    scenario: ExplorationScenario | None = None


@dataclass(frozen=True)
class PreparedRun:
    directory: Path
    profile: BootProfile
    manifest: dict


def load_profile(name: str, *, scenario_name: str | None = None) -> BootProfile:
    if name == 'validation':
        raise ValueError('missing_boot_state: loader-created CPU state is not recovered')
    profiles = {'module-entry-diagnostic': 'xt4-212.json',
                'bootstrap-entry-diagnostic': 'xt4-212-bootstrap.json',
                'bootstrap-port-config-diagnostic': 'xt4-212-ports.json',
                'bootstrap-input-exploration': 'xt4-212-inputs.json',
                'bootstrap-timer-exploration': 'xt4-212-timer.json',
                'bootstrap-controller-exploration': 'xt4-212-controller.json'}
    if name not in profiles:
        raise ValueError('Unsupported profile')
    data = json.loads(Path(__file__).with_name('profiles').joinpath(profiles[name]).read_text())
    data.pop('schema_version')
    data['regions'] = tuple(Region(**r) for r in data['regions'])
    data['assumptions'] = tuple(data['assumptions'])
    data['models'] = tuple(data.get('models', ()))
    if name in ('bootstrap-input-exploration', 'bootstrap-timer-exploration', 'bootstrap-controller-exploration'):
        if scenario_name is None:
            raise ValueError('Exploration requires an explicit synthetic scenario')
        data['scenario'] = load_scenario(scenario_name, timer=name != 'bootstrap-input-exploration',
                                         platform=name == 'bootstrap-controller-exploration')
    elif scenario_name is not None:
        raise ValueError('Synthetic scenario is not allowed on this profile')
    profile = BootProfile(**data)
    validate_profile(profile)
    return profile


def validate_profile(profile: BootProfile) -> None:
    expected_models = {'module-entry-diagnostic': (), 'bootstrap-entry-diagnostic': (),
                       'bootstrap-port-config-diagnostic': ('xt4-port-config-v1',),
                       'bootstrap-input-exploration': ('xt4-port-config-v1', 'xt4-static-port-input-v1'),
                       'bootstrap-timer-exploration': ('xt4-port-config-v1', 'xt4-static-port-input-v1',
                                                       'xt4-timer-exploration-v1'),
                       'bootstrap-controller-exploration': ('xt4-port-config-v1', 'xt4-static-port-input-v1',
                           'xt4-timer-exploration-v1', 'xt4-transfer-enable-exploration-v1',
                           'xt4-clock-apply-exploration-v1')}
    if profile.name not in expected_models or profile.models != expected_models[profile.name]:
        raise ValueError('Unsupported profile model set')
    if profile.name in ('bootstrap-input-exploration', 'bootstrap-timer-exploration', 'bootstrap-controller-exploration'):
        validate_scenario(profile.scenario, timer=profile.name != 'bootstrap-input-exploration',
                          platform=profile.name == 'bootstrap-controller-exploration')
    elif profile.scenario is not None:
        raise ValueError('Scenario is not allowed on a diagnostic profile')
    extents = []
    names = set()
    for r in sorted(profile.regions, key=lambda x: x.base):
        if (r.name in names or type(r.base) is not int or type(r.size) is not int
                or r.base < 0 or r.size <= 0 or r.base + r.size > 0x100000000
                or not 0 <= r.file_size <= r.size):
            raise ValueError('Invalid region bounds/name')
        if extents and r.base < extents[-1][1]:
            raise ValueError('Overlapping memory regions')
        if bool(r.file) != bool(r.sha256) or (r.file is None and r.file_size != 0):
            raise ValueError('Invalid image metadata')
        names.add(r.name)
        extents.append((r.base, r.base + r.size))
    if profile.entry & 3 or not any(r.file and r.base <= profile.entry < r.base + r.file_size
                                    for r in profile.regions):
        raise ValueError('Entry outside original image')


def read_regular(root: Path, relative: str) -> bytes:
    """Open each component without following symlinks; never block on a FIFO."""
    rel = Path(relative)
    root = root.absolute()
    if rel.is_absolute() or '..' in rel.parts or not rel.parts or root != root.resolve():
        raise ValueError('Unsafe input path')
    fd = None
    try:
        fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        for part in rel.parts[:-1]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd)
            fd = child
        file_fd = os.open(rel.name, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW, dir_fd=fd)
        with os.fdopen(file_fd, 'rb') as stream:
            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                raise ValueError('Input must be a regular file')
            return stream.read()
    except OSError as exc:
        raise ValueError('Cannot open safe regular input: ' + str(rel)) from exc
    finally:
        if fd is not None:
            os.close(fd)


def profile_dict(profile: BootProfile) -> dict:
    return {'schema_version': 1, **asdict(profile), 'scenario': scenario_dict(profile.scenario)}


def verify_prepared(run: PreparedRun) -> None:
    expected = json.dumps(profile_dict(run.profile), sort_keys=True, allow_nan=False)
    staged = json.dumps(json.loads(read_regular(run.directory, 'profile.json')),
                        sort_keys=True, allow_nan=False)
    # Python equality treats false/0, true/1 and integral floats as equivalent.
    # The staged declaration must retain its exact JSON types as well as values.
    if staged != expected:
        raise ValueError('Staged profile changed')
    for r in run.profile.regions:
        if r.file:
            data = read_regular(run.directory, r.file)
            if len(data) != r.file_size or hashlib.sha256(data).hexdigest() != r.sha256:
                raise ValueError('Staged image mismatch: ' + r.name)


def prepare_run(research_root: Path, run_root: Path, profile_name: str, *,
                scenario_name: str | None = None) -> PreparedRun:
    profile = load_profile(profile_name, scenario_name=scenario_name)
    research_root = research_root.absolute()
    run_root = run_root.absolute()
    if ('..' in run_root.parts or run_root != run_root.resolve()
            or not run_root.is_relative_to(REPO / 'runs') or run_root.exists()
            or research_root != research_root.resolve()
            or not research_root.is_relative_to(PROJECT)):
        raise ValueError('Output must be a new directory under project runs/; inputs must be project files')
    original = read_regular(research_root, 'FWUP0024.DAT')
    if len(original) != 51278280 or hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('Original DAT fingerprint mismatch')
    images = []
    sources = dict(zip(('primary', 'secondary', 'wireless'), SOURCE_FILES))
    used_sources = []
    derived = {}
    for r in (r for r in profile.regions if r.file):
        if r.name == 'bootstrap':
            data = extract_bootstrap(original)
            used_sources.append('FWUP0024.DAT')
            derived[r.name] = {'original_dat_offset': BOOTSTRAP_OFFSET,
                               'transform': 'xor-ff', 'sha256': r.sha256}
        else:
            source = sources[r.name]
            data = read_regular(research_root, source)
            used_sources.append(source)
        if len(data) != r.file_size or hashlib.sha256(data).hexdigest() != r.sha256:
            raise ValueError('Decoded image fingerprint mismatch: ' + r.name)
        images.append((r, data))
    run_root.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    run_root.mkdir(mode=0o700)
    (run_root / 'inputs').mkdir(mode=0o700)
    for r, data in images:
        path = run_root / r.file
        with path.open('xb') as stream:
            stream.write(data)
        path.chmod(0o400)
    manifest = {'schema_version': 1, 'original_sha256': ORIGINAL_SHA256,
                'research_root': str(research_root), 'source_files': tuple(used_sources),
                'profile_sha256': hashlib.sha256(json.dumps(profile_dict(profile), sort_keys=True).encode()).hexdigest()}
    manifest['scenario_sha256'] = scenario_sha256(profile.scenario)
    if derived:
        manifest['derived_images'] = derived
    (run_root / 'profile.json').write_text(json.dumps(profile_dict(profile), indent=2) + '\n')
    (run_root / 'provenance.json').write_text(json.dumps(manifest, indent=2) + '\n')
    run = PreparedRun(run_root, profile, manifest)
    verify_prepared(run)
    return run
