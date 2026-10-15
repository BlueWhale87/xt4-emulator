#!/usr/bin/env python3
"""Build/test the independent native detector; never launch camera firmware."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1] / 'contrib/xt4-native-detection'
for command in [
    ['cmake', '--preset', 'host'],
    ['cmake', '--build', '--preset', 'host', '--parallel', '4'],
    ['ctest', '--test-dir', 'build/host', '--timeout', '90', '--output-on-failure'],
]:
    subprocess.run(command, cwd=root, check=True)
