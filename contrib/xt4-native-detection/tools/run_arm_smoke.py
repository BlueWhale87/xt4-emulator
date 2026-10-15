"""Run the trusted, filesystem-free standalone diagnostic on generic QEMU only."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def validate_output(output, returncode, expected):
    if returncode or output.count('PASS native ARM face inference\n') != 1:
        raise ValueError('Missing PASS or unsuccessful QEMU exit')
    lines = output.splitlines()
    cases = []
    for name, boxes in expected.items():
        marker = 'CASE ' + name + ' ' + str(len(boxes))
        if lines.count(marker) != 1:
            raise ValueError('Missing/count/order mismatch: ' + name)
        index = lines.index(marker)
        if cases and index <= cases[-1]['line']:
            raise ValueError('Fixture order changed')
        actual = []
        for i, box in enumerate(boxes):
            tokens = lines[index + i + 1].split()
            if len(tokens) != 6 or tokens[0] != 'BOX' or any(len(x) != 8 for x in tokens[1:]):
                raise ValueError('Malformed box')
            values = [struct.unpack('>f', bytes.fromhex(x))[0] for x in tokens[1:]]
            for j, key in enumerate(('score', 'x0', 'y0', 'x1', 'y1')):
                if not math.isfinite(values[j]) or abs(values[j] - box[key]) > (.0001 if j == 0 else .25):
                    raise ValueError('ARM/reference parity mismatch: ' + name)
            actual.append(dict(zip(('score', 'x0', 'y0', 'x1', 'y1'), values)))
        cases.append({'name': name, 'count': len(boxes), 'boxes': actual, 'line': index})
    if sum(x.startswith('CASE ') for x in lines) != len(cases) or sum(x.startswith('BOX ') for x in lines) != sum(len(x) for x in expected.values()):
        raise ValueError('Unexpected cases or boxes')
    def metric(prefix, limit):
        matches = [x.split()[1] for x in lines if x.startswith(prefix + ' ')]
        if len(matches) != 1 or not 0 < int(matches[0]) < limit:
            raise ValueError('Missing/out-of-bounds ' + prefix)
        return int(matches[0])
    return {'cases': cases, 'arena_peak_bytes': metric('PEAK', 8388609),
            'stack_high_water_bytes': metric('STACK_HIGH_WATER', 131072)}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--elf', type=Path, default=ROOT / 'build/arm/xt4_face_smoke.elf')
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'runs/arm-native')
    args = parser.parse_args()
    qemu, elf = args.qemu.resolve(), args.elf.resolve()
    manifest = json.loads((ROOT / 'tests/fixtures/manifest.json').read_text())
    expected = {}
    for item in manifest['fixtures']:
        folder = ROOT / 'tests/fixtures'
        raw, golden = folder / (item['name'] + '.bgr'), folder / (item['name'] + '.reference.json')
        if sha(raw) != item['sha256'] or sha(golden) != item['reference_sha256']:
            raise ValueError('Fixture hash mismatch')
        expected[item['name']] = json.loads(golden.read_text())['boxes']
    command = [str(qemu), '-M', 'virt', '-cpu', 'cortex-a7', '-smp', '1', '-m', '128M', '-nographic', '-nic', 'none',
               '-semihosting-config', 'enable=on,target=native', '-kernel', str(elf)]
    version = subprocess.check_output([str(qemu), '--version'], text=True)
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    output_path = args.output_dir / 'qemu-output.txt'
    output_path.write_text(completed.stdout)
    result = validate_output(completed.stdout, completed.returncode, expected)
    result.update({'status': 'passed', 'scope': 'standalone ARMv7-A soft-float; not camera execution or speed',
                   'command': command, 'returncode': completed.returncode, 'qemu_version': version,
                   'qemu_sha256': sha(qemu), 'elf_sha256': sha(elf), 'output_sha256': sha(output_path),
                   'fixture_manifest_sha256': sha(ROOT / 'tests/fixtures/manifest.json')})
    (args.output_dir / 'smoke.json').write_text(json.dumps(result, indent=2) + '\n')
    print('ARM smoke passed:', len(result['cases']), 'fixtures; peak', result['arena_peak_bytes'],
          'bytes; observed stack', result['stack_high_water_bytes'], 'bytes')

if __name__ == '__main__':
    main()
