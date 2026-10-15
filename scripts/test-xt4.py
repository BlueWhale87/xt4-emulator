#!/usr/bin/env python3
"""Run the complete X-T4 laboratory suite and its real QEMU board checks."""
from pathlib import Path
import subprocess
import sys

repo = Path(__file__).resolve().parents[1]
subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s',
                'tests/xt4lab', '-v'], cwd=repo, check=True)
subprocess.run(['ninja', '-C', 'build', '-j', '4', 'tests/qtest/xt4-test'],
               cwd=repo, check=True)
subprocess.run([str(repo / 'build/pyvenv/bin/meson'), 'test', '-C', 'build',
                '--no-rebuild', '--print-errorlogs', 'qtest-arm/xt4-test'],
               cwd=repo, check=True)
