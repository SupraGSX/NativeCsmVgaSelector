#!/usr/bin/env python3
"""Record build provenance. SPDX-License-Identifier: GPL-3.0-only"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--edk2', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
def command(args):
    return subprocess.check_output(args, text=True).strip()
efi = a.output.parent / 'BOOTX64.EFI'
source_files = sorted(p for p in (a.source / 'NativeCsmVgaSelectorPkg').rglob('*') if p.is_file())
a.output.write_text(json.dumps({
    'version': '1.2.2-audit-fixes.1',
    'source_base': command(['git', '-C', str(a.source), 'rev-parse', 'HEAD']),
    'source_status': command(['git', '-C', str(a.source), 'status', '--porcelain']),
    'source_hashes': {p.relative_to(a.source).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in source_files},
    'edk2_commit': command(['git', '-C', str(a.edk2), 'rev-parse', 'HEAD']),
    'edk2_status': command(['git', '-C', str(a.edk2), 'status', '--porcelain']),
    'gcc': command(['gcc', '--version']).splitlines()[0],
    'ld': command(['ld', '--version']).splitlines()[0],
    'objdump': command(['objdump', '--version']).splitlines()[0],
    'efi_sha256': hashlib.sha256(efi.read_bytes()).hexdigest(),
    'efi_bytes': efi.stat().st_size,
    'hardware_validation': 'pending'
}, indent=2) + '\n')
