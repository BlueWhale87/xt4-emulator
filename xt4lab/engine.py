"""Build and fingerprint the isolated, pinned QEMU source tree."""
import hashlib
import json
import platform
from pathlib import Path
import subprocess
import sys

LOCK = json.loads(Path(__file__).with_name('engine-lock.json').read_text())


def check_source(repo: Path) -> str:
    def git(*args):
        return subprocess.run(['git', '-C', str(repo), *args], text=True,
                              capture_output=True, check=True).stdout.strip()
    try:
        base = git('rev-parse', LOCK['tag'] + '^{commit}')
        if base != LOCK['commit']:
            raise ValueError('QEMU source pin mismatch')
        git('merge-base', '--is-ancestor', base, 'HEAD')
    except subprocess.CalledProcessError as exc:
        raise ValueError('Missing or unrelated QEMU source pin') from exc
    return base


def configure_args() -> list[str]:
    flags = ['--target-list=arm-softmmu', '--without-default-features',
             '--enable-tcg', '--disable-hvf', '--disable-docs',
             '--disable-user', '--disable-tools', '--disable-guest-agent',
             '--python=' + sys.executable]
    # Available once the custom machine is added, without forcing a fake board.
    if Path(__file__).resolve().parents[1].joinpath(
            'configs/devices/arm-softmmu/xt4.mak').exists():
        flags += ['--without-default-devices', '--with-devices-arm=xt4']
    return flags


def source_fingerprint(repo: Path) -> dict[str, str | None]:
    """Fingerprint tracked and untracked sources, excluding ignored outputs."""
    paths = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others',
                                      '--exclude-standard'], cwd=repo).split(b'\0')
    return {os_path.decode(): hashlib.sha256((repo / os_path.decode()).read_bytes()).hexdigest()
            if (repo / os_path.decode()).is_file() else None for os_path in paths if os_path}


def build_engine(repo: Path) -> Path:
    repo = repo.resolve()
    base = check_source(repo)
    build = repo / 'build'
    build.mkdir(exist_ok=True)
    flags = configure_args()
    subprocess.run([str(repo / 'configure'), *flags], cwd=build, check=True)
    subprocess.run(['ninja', '-j', '4', 'qemu-system-arm'], cwd=build, check=True)
    binary = build / 'qemu-system-arm'
    versions = {}
    for label, cmd in {'compiler': ['clang', '--version'],
                       'glib': ['pkg-config', '--modversion', 'glib-2.0'],
                       'ninja': ['ninja', '--version'],
                       'pkgconf': ['pkg-config', '--version']}.items():
        versions[label] = subprocess.check_output(cmd, text=True).splitlines()[0]
    manifest = {'source_base': base, 'configure_args': flags,
                'patch_revision': subprocess.check_output(
                    ['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip(),
                'dirty_diff_sha256': hashlib.sha256(subprocess.check_output(
                    ['git', 'diff', 'HEAD'], cwd=repo)).hexdigest(),
                'python': sys.version, 'host': platform.platform(),
                'dependencies': versions,
                'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}
    manifest['source_files_sha256'] = source_fingerprint(repo)
    (build / 'xt4-build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return binary
