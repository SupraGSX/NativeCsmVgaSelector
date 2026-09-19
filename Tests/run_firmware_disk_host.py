#!/usr/bin/env python3
"""Test production firmware-disk parsing without physical memory or hardware I/O.
SPDX-License-Identifier: GPL-3.0-only
"""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
edk = Path(os.environ.get('EDK2_DIR', root / 'Toolchain/edk2-stable202605'))
app = root / 'NativeCsmVgaSelectorPkg/Application'
output = root / 'Tests/build/LegacyFirmwareDiskHost'
output.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(['gcc', '-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
    '-fshort-wchar', '-DEFIAPI=', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
    '-I' + str(app), '-I' + str(root / 'NativeCsmVgaSelectorPkg/Include'),
    '-I' + str(edk / 'MdePkg/Include'), '-I' + str(edk / 'MdePkg/Include/X64'),
    str(app / 'LegacyFirmwareDisk.c'), str(root / 'Tests/LegacyFirmwareDiskHost.c'), '-o', str(output)], check=True)
subprocess.run([str(output)], check=True,
    env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1'))
