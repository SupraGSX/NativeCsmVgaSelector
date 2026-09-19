#!/usr/bin/env python3
"""Verify exact source members and runtime SHA256SUMS after packaging.
SPDX-License-Identifier: GPL-3.0-only
"""
import argparse
import hashlib
from pathlib import Path
import zipfile
import package

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, default=package.ROOT / 'Build/Packages')
a = p.parse_args()
prefix = 'NativeCsmVgaSelector-1.2/'
sources = {prefix + p.relative_to(package.ROOT).as_posix(): p for p in package.source_files()}
with zipfile.ZipFile(a.output / 'NativeCsmVgaSelector-1.2-source.zip') as archive:
    if len(archive.namelist()) != len(sources) or set(archive.namelist()) != set(sources):
        raise SystemExit('Unexpected/missing/duplicate source archive member')
    for name, source in sources.items():
        if archive.read(name) != source.read_bytes():
            raise SystemExit('Source archive differs: ' + name)
with zipfile.ZipFile(a.output / 'NativeCsmVgaSelector-1.2.zip') as archive:
    entries = archive.namelist()
    hashes = dict(line.split('  ', 1)[::-1] for line in archive.read('SHA256SUMS').decode().splitlines())
    if len(entries) != len(set(entries)) or set(entries) != set(hashes) | {'SHA256SUMS'}:
        raise SystemExit('Runtime member/manifest mismatch')
    for name, digest in hashes.items():
        data = archive.read(name)
        if hashlib.sha256(data).hexdigest() != digest:
            raise SystemExit('Runtime ZIP hash mismatch: ' + name)
        if (a.output / 'NativeCsmVgaSelector-1.2' / name).read_bytes() != data:
            raise SystemExit('Runtime directory mismatch: ' + name)
print(f'PASS {len(sources)} exact source members and {len(hashes)} runtime hashes')
