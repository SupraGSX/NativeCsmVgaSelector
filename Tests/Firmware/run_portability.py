#!/usr/bin/env python3
"""Test production firmware portability validators with synthetic edge cases.
No host disks, PCI mutations, real CSM handoff, or firmware variable writes.
SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

from run import ROOT, build_harness, create_disk, run


def main():
    parent = ROOT / 'Build/FirmwareTests'
    parent.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='portability-', dir=parent))
    print(f'Evidence: {output}', flush=True)
    efi = build_harness(output, portability=True)
    disk = create_disk(output)
    fat = str(disk) + '@@1048576'
    run(['mcopy', '-i', fat, str(efi), '::/EFI/BOOT/BOOTX64.EFI'])
    ovmf = Path('/usr/share/OVMF')
    shutil.copyfile(ovmf / 'OVMF_VARS_4M.fd', output / 'vars.fd')
    with (output / 'qemu.log').open('w') as log:
        run(['qemu-system-x86_64', '-machine', 'q35', '-m', '256', '-net', 'none',
             '-drive', f'if=pflash,format=raw,readonly=on,file={ovmf}/OVMF_CODE_4M.fd',
             '-drive', f'if=pflash,format=raw,file={output}/vars.fd',
             '-drive', f'file={disk},format=raw', '-display', 'none', '-no-reboot'],
            timeout=60, stdout=log, stderr=log)
    for name in ('PortabilityTest.log', 'PortabilityTest.result'):
        content = subprocess.check_output(['mtype', '-i', fat, '::/EFI/BOOT/' + name])
        (output / name).write_bytes(content)
    result = (output / 'PortabilityTest.result').read_text()
    print(result, end='', flush=True)
    assert result.startswith('PASS '), f'Firmware test failed; see {output}'


if __name__ == '__main__':
    main()
