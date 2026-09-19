#!/usr/bin/env python3
"""Exercise actual ROM inspection and error reporting in isolated OVMF.
Optional paired captures stay in disposable images, never in release source.
SPDX-License-Identifier: GPL-3.0-only
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
from run import ROOT, build_harness, create_disk, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture-dir', type=Path)
    args = parser.parse_args()
    parent = ROOT / 'Build/FirmwareTests'
    parent.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='rom-guard-', dir=parent))
    print(f'Evidence: {output}', flush=True)
    efi = build_harness(output, rom_guard=True)
    disk = create_disk(output)
    fat = str(disk) + '@@1048576'
    run(['mcopy', '-i', fat, str(efi), '::/EFI/BOOT/BOOTX64.EFI'])
    provenance = {}
    if args.capture_dir:
        for letter, run_name in [('a', 'RUN000001'), ('b', 'RUN000002')]:
            capture = args.capture_dir / run_name
            shadow = (capture / 'before-bbs-shadow-C0000-DFFFF.bin').read_bytes()
            assert len(shadow) == 0x20000
            assert (capture / 'before-bbs-shadow-valid.bin').read_bytes() == bytes([1]) * 256
            pcir = struct.unpack_from('<H', shadow, 0x18)[0]
            assert shadow[pcir:pcir + 4] == b'PCIR'
            vendor, device = struct.unpack_from('<HH', shadow, pcir + 4)
            config = (capture / 'Config.ini').read_text()
            target = re.search(r'^TargetPci=([0-9A-Fa-f:]+)\.([0-7])$', config, re.M)
            assert target
            gpu = capture / ('pci-' + target[1].replace(':', '-') + '-' + target[2] + '-rom.bin')
            copy_bytes = len(gpu.read_bytes())
            report = (capture / 'report.txt').read_text()
            vector = int(re.search(r'IVT before-bbs interrupt=13 .*physical=([0-9A-F]+)', report)[1], 16)
            if letter == 'a':
                expected, address, length = 0, 0, 0
            else:
                # Independent expected conflict from the first non-primary resident ROM.
                headers = [i for i in range(512, copy_bytes, 512) if shadow[i:i + 2] == b'\x55\xaa']
                assert headers
                offset = headers[0]
                expected, address, length = 1, 0xc0000 + offset, shadow[offset + 2] * 512
            metadata = struct.pack('<7I', vendor, device, copy_bytes, vector, expected, address, length)
            storage = re.search(r'^TargetControllerPci=([0-9A-Fa-f]+):([0-9A-Fa-f]+):([0-9A-Fa-f]+)\.([0-7])$', config, re.M)
            assert storage
            location = tuple(int(v, 16) for v in storage.groups())
            pci_name = 'pci-' + '-'.join(storage.groups()) + '-config.bin'
            pci_config = (capture / pci_name).read_bytes()[:64]
            bbs = (capture / 'bbs-table.bin').read_bytes()
            assert len(bbs) % 69 == 0
            selected = [bbs[i:i + 69] for i in range(0, len(bbs), 69)
                        if struct.unpack_from('<III', bbs, i + 2) == location[1:]]
            assert len(selected) == 1
            storage_meta = struct.pack('<5I', *location, int(letter == 'b'))
            fixtures = [(f'capture-{letter}.bin', shadow), (f'capture-{letter}.meta', metadata),
                        (f'capture-{letter}.storage', storage_meta), (f'capture-{letter}.bbs', selected[0]),
                        (f'capture-{letter}.pci', pci_config)]
            for name, data in fixtures:
                path = output / name
                path.write_bytes(data)
                run(['mcopy', '-i', fat, str(path), '::/EFI/BOOT/' + name])
                provenance[name] = hashlib.sha256(data).hexdigest()
    (output / 'fixture-sha256.json').write_text(json.dumps(provenance, indent=2) + '\n')
    ovmf = Path('/usr/share/OVMF')
    shutil.copyfile(ovmf / 'OVMF_VARS_4M.fd', output / 'vars.fd')
    with (output / 'qemu.log').open('w') as log:
        run(['qemu-system-x86_64', '-machine', 'q35', '-m', '256', '-net', 'none',
             '-drive', f'if=pflash,format=raw,readonly=on,file={ovmf}/OVMF_CODE_4M.fd',
             '-drive', f'if=pflash,format=raw,file={output}/vars.fd',
             '-drive', f'file={disk},format=raw', '-display', 'none', '-no-reboot'],
            timeout=60, stdout=log, stderr=log)
    for name in ('RomGuardTest.log', 'RomGuardTest.result'):
        data = subprocess.check_output(['mtype', '-i', fat, '::/EFI/BOOT/' + name])
        (output / name).write_bytes(data)
    result = (output / 'RomGuardTest.result').read_text()
    print(result, end='', flush=True)
    assert result.startswith('PASS '), f'Firmware test failed; see {output}'
    log = (output / 'RomGuardTest.log').read_text()
    if args.capture_dir:
        assert 'PASS captured layout 0: result=0' in log
        assert 'PASS captured layout 1: result=1' in log
        assert 'PASS captured storage 0: ROM-proof=0 status=0500 eligible=yes' in log
        assert 'PASS captured storage 1: ROM-proof=1 status=0000 eligible=yes' in log
    assert 'Code: NCV_BLOCKED_ROM_OVERLAP' in log


if __name__ == '__main__':
    main()
