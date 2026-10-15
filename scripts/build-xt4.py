#!/usr/bin/env python3
"""Build only the local Arm system emulator; never install it globally."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from xt4lab.engine import build_engine

if __name__ == '__main__':
    print(build_engine(Path(__file__).resolve().parents[1]))
