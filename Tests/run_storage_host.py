#!/usr/bin/env python3
"""Sanitize production ROM parser using fixture files from run_rom_guard.py.
No firmware execution, physical memory reads or hardware writes.
SPDX-License-Identifier: GPL-3.0-only
"""
import argparse
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('fixtures', type=Path)
args = parser.parse_args()
edk = Path(os.environ.get('EDK2_DIR', root / 'Toolchain/edk2-stable202605'))
app = root / 'NativeCsmVgaSelectorPkg/Application'
output = root / 'Tests/build/LegacyStorageRomHost'
output.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(['gcc', '-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
    '-fshort-wchar', '-DEFIAPI=', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
    '-I' + str(app), '-I' + str(root / 'NativeCsmVgaSelectorPkg/Include'),
    '-I' + str(edk / 'MdePkg/Include'), '-I' + str(edk / 'MdePkg/Include/X64'),
    str(app / 'LegacyStorageRom.c'), str(root / 'Tests/LegacyStorageRomHost.c'), '-o', str(output)], check=True)
subprocess.run([str(output), *(str(args.fixtures / ('capture-b.' + suffix))
    for suffix in ('bin', 'bbs', 'pci', 'storage'))], check=True,
    env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1'))
