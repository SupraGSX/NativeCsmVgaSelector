#!/usr/bin/env python3
"""Fail explicitly if production gains a second native handoff or disconnect.
SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
source = '\n'.join(p.read_text() for p in (root / 'NativeCsmVgaSelectorPkg/Application').glob('*.c'))
for pattern, description in ((r'gBS->DisconnectController\s*\(', 'controller disconnect'),
                             (r'->LegacyBoot\s*\(', 'native LegacyBoot')):
    count = len(re.findall(pattern, source))
    if count != 1:
        raise SystemExit(f'Expected one production {description}; found {count}')
print('Single production disconnect and native LegacyBoot: PASS')
