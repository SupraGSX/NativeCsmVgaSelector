#!/usr/bin/env python3
"""Check the reviewed x64 boot prologue; static C guards check ABI sizes.
SPDX-License-Identifier: GPL-3.0-only
"""
import argparse
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('symbols', type=Path, help='Production NativeCsmVgaSelector.debug from EDK II build')
args = parser.parse_args()
text = subprocess.check_output(['objdump', '-d', str(args.symbols)], text=True)
match = re.search(r'^[0-9a-f]+ <NativeCsmVgaBootRun>:\n((?:[^\n]*\n){18})', text, re.M)
if not match:
    raise SystemExit('Reviewed boot symbol not found; inspect production disassembly')
prologue = match.group(0)
if '-0x7000(%rsp)' not in prologue:
    raise SystemExit('Boot stack probing extent changed\n' + prologue)
if not re.search(r'sub\s+\$0x578,%rsp', prologue):
    raise SystemExit('Boot stack remainder changed\n' + prologue)
pushes = re.findall(r'push\s+(%\w+)', prologue)
if pushes != ['%rbp', '%r15', '%r14', '%r13', '%r12', '%rdi', '%rsi', '%rbx']:
    raise SystemExit('Boot saved registers changed: ' + str(pushes))
print(prologue)
print('Reviewed boot prologue: PASS (0x7000 + 0x578, same saved registers)')
