#!/usr/bin/env python3
"""Exercise the real dispatcher, INI I/O and target UI on synthetic FAT media.
No physical disks or real GPU handoff are used. SPDX-License-Identifier: GPL-3.0-only
"""
import argparse
from pathlib import Path
import os
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
BASE = b'''# preserved comment
[Marker]
Enabled=true
DiskSignature=0x12345678
PartitionNumber=1
PartitionStart=2048
PartitionSectors=131072
Path=\\BOOTSEL.DAT
Profile=SECONDARY
[Behavior]
Probe=false
[Video]
TargetPci=0000:03:00.0
[Endpoint]
IoDecoding=On
MemoryDecoding=On
BusMastering=Ignore
[Boot]
TargetControllerPci=0000:05:00.0
TargetBbsDescription=Example disk A
'''
MODES = ['fresh', 'edit', 'cancel', 'repeat', 'recover', 'missing', 'temp',
         'partial', 'comments', 'reject', 'recover-cancel', 'reset-cancel', 'reset',
         'restrictions', 'missing-target', 'restrictions-block', 'reset-preserve-failure', 'marker',
         'edit-again', 'diagnostic', 'incomplete', 'save-failure', 'reload-failure', 'cancel-fresh',
         'fresh-padded', 'edit-padded', 'edit-again-padded', 'edit-bdf-only']

def run(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)

def build_harness(output, marker=False, boot_target=False, portability=False, rom_guard=False):
    name = 'HarnessRomGuard' if rom_guard else ('HarnessPortability' if portability else ('HarnessBootTarget' if boot_target else ('HarnessMarker' if marker else 'Harness')))
    harness = output / name
    if harness.exists():
        shutil.rmtree(harness)
    package = harness / 'NativeCsmVgaSelectorPkg'
    shutil.copytree(ROOT / 'NativeCsmVgaSelectorPkg', package)
    app = package / 'Application'
    shutil.copyfile(app / 'NativeCsmVgaSelector.c', app / 'DispatcherUnderTest.c')
    source = 'RomGuardHarness.c' if rom_guard else ('PortabilityHarness.c' if portability else ('BootTargetHarness.c' if boot_target else ('MarkerHarness.c' if marker else 'Harness.c')))
    shutil.copyfile(Path(__file__).with_name(source), app / 'NativeCsmVgaSelector.c')
    if name == 'Harness':
        shutil.copyfile(Path(__file__).with_name('CountdownTests.inc'), app / 'CountdownTests.inc')
    if rom_guard:
        shutil.copyfile(Path(__file__).with_name('StorageRomTests.inc'), app / 'StorageRomTests.inc')
        shutil.copyfile(Path(__file__).with_name('FirmwareDiskTests.inc'), app / 'FirmwareDiskTests.inc')
        firmware = app / 'LegacyFirmwareDisk.c'
        text = firmware.read_text()
        assert text.count('NCV_DISK_FIRMWARE_BASE + Block * NCV_DISK_FIRMWARE_BLOCK') == 2
        text = text.replace('NCV_DISK_FIRMWARE_BASE + Block * NCV_DISK_FIRMWARE_BLOCK',
                            '(UINTN)FirmwareDiskTestMemory + Block * NCV_DISK_FIRMWARE_BLOCK')
        assert text.count('(CONST UINT8 *)(UINTN)NCV_DISK_FIRMWARE_BASE') == 1
        text = text.replace('(CONST UINT8 *)(UINTN)NCV_DISK_FIRMWARE_BASE', 'FirmwareDiskTestMemory')
        firmware.write_text('#include <Uefi.h>\nextern UINT8 FirmwareDiskTestMemory[];\n' + text)
        # Redirect only physical reads in the full guard wrapper to allocated
        # fixture RAM. The inspector keeps C0000 semantics; production source
        # has no test hooks and the harness never reads/writes actual ROM space.
        guard = app / 'LegacyRomGuard.c'
        text = guard.read_text()
        start = text.index('LegacyRomGuardCheck (APP_LOGGER *Logger,')
        head, body = text[:start], text[start:]
        assert body.count('NCV_ROM_SHADOW_BASE + Block * NCV_ROM_BLOCK_SIZE') == 2
        body = body.replace('NCV_ROM_SHADOW_BASE + Block * NCV_ROM_BLOCK_SIZE',
                            '(UINTN)RomGuardTestPhysicalShadow + Block * NCV_ROM_BLOCK_SIZE')
        assert body.count('0x13U * 4U') == 2
        body = body.replace('0x13U * 4U', '(UINTN)RomGuardTestVector')
        assert body.count('(CONST UINT8 *)(UINTN)NCV_ROM_SHADOW_BASE') == 1
        body = body.replace('(CONST UINT8 *)(UINTN)NCV_ROM_SHADOW_BASE', 'RomGuardTestPhysicalShadow')
        guard.write_text('#include <Uefi.h>\nextern UINT8 *RomGuardTestPhysicalShadow;\nextern UINT8 RomGuardTestVector[4];\n' + head + body)
    if portability:
        shutil.copyfile(Path(__file__).with_name('AuditBoundaryTests.inc'), app / 'AuditBoundaryTests.inc')
        # Exercise the production scanner against allocated low RAM, never
        # writing OVMF's legacy region. Only its two address constants differ.
        runtime = app / 'RuntimePlan.c'
        text = runtime.read_text()
        for name, value in [('START', 'CompatibilityTestStart'), ('END', 'CompatibilityTestEnd')]:
            text, count = re.subn(r'^#define COMPAT_SCAN_' + name + r' +0x[0-9A-Fa-f]+U$',
                                 lambda match: '#ifdef NCV_TEST_SCANNER\n#define COMPAT_SCAN_' + name + ' ' + value + '\n#else\n' + match.group(0) + '\n#endif', text, flags=re.M)
            assert count == 1, 'Scanner fixture boundary changed; review test adapter'
        runtime.write_text(text)
    dsc = package / 'NativeCsmVgaSelectorPkg.dsc'
    dsc.write_text(dsc.read_text().replace('NativeCsmVgaSelector$(SELECTOR_EDITION)', 'NativeCsmVgaSelector' + name))
    script = harness / 'build.sh'
    build_script = (ROOT / 'build.sh').read_text()
    # These private harnesses replace the real entry point/Boot core. Release
    # layout and provenance gates always run on production builds separately.
    build_script = '\n'.join(line for line in build_script.splitlines()
                             if not line.startswith('python3 "$PROJECT_ROOT/Scripts/')) + '\n'
    script.write_text(build_script.replace('Build/NativeCsmVgaSelector$EDITION/', 'Build/NativeCsmVgaSelector' + name + '/'))
    (harness / 'Data').mkdir()
    shutil.copyfile(ROOT / 'Data/pci.ids', harness / 'Data/pci.ids')
    env = dict(os.environ)
    env.setdefault('EDK2_DIR', str(ROOT / 'Toolchain/edk2-stable202605'))
    with (output / 'harness-build.log').open('w') as log:
        run(['bash', str(script)], env=env, stdout=log, stderr=subprocess.STDOUT)
    return harness / 'Build/Release/BOOTX64.EFI'

def create_disk(directory):
    volume = directory / 'fat.img'
    with volume.open('wb') as stream:
        stream.truncate(64 * 1024 * 1024)
    run(['mkfs.fat', '-F', '32', '-i', '12345678', str(volume)], stdout=subprocess.DEVNULL)
    disk = directory / 'disk.img'
    with disk.open('wb') as stream:
        mbr = bytearray(512)
        struct.pack_into('<I', mbr, 440, 0x12345678)
        mbr[446:462] = struct.pack('<B3sB3sII', 0x80, b'\0\2\0', 0x0c, b'\xfe\xff\xff', 2048, 131072)
        mbr[510:512] = b'\x55\xaa'
        stream.write(mbr)
        stream.seek(1048576)
        # Preserve zero-filled regions as holes: dozens of disposable guests
        # should not allocate their entire mostly-empty 64 MiB FAT volumes.
        with volume.open('rb') as source:
            while chunk := source.read(1024 * 1024):
                if chunk.count(0) == len(chunk):
                    stream.seek(len(chunk), 1)
                else:
                    stream.write(chunk)
            stream.truncate(stream.tell())
    volume.unlink()
    run(['mmd', '-i', str(disk) + '@@1048576', '::/EFI', '::/EFI/BOOT'])
    return disk

def scenario(mode, output, efi, ovmf):
    directory = output / mode
    directory.mkdir()
    bdf_only = mode == 'edit-bdf-only'
    test_mode = 'edit' if bdf_only else mode
    mode = 'edit' if bdf_only else mode
    padded = mode.endswith('-padded')
    mode = mode.removesuffix('-padded')
    disk = create_disk(directory)
    fat = str(disk) + '@@1048576'
    def put(name, data):
        source = directory / name
        source.write_bytes(data)
        run(['mcopy', '-o', '-i', fat, str(source), '::/EFI/BOOT/' + name])
    def get(name):
        result = subprocess.run(['mtype', '-i', fat, '::/EFI/BOOT/' + name], capture_output=True)
        return result.stdout if result.returncode == 0 else None
    original = BASE.replace(b'TargetBbsDescription=Example disk A\n', b'') if bdf_only else BASE
    if mode in ('restrictions', 'restrictions-block'):
        original = BASE.replace(b'[Video]\n', b'[Video]\nExpectedVendor=9999\n')
    if mode == 'missing-target':
        original = BASE.replace(b'TargetPci=0000:03:00.0', b'TargetPci=0000:09:00.0')
    if mode == 'marker':
        initial = (b'NATIVE CSM BOOT PROFILE 1\nPROFILE=DEFAULT').ljust(511, b' ') + b'\n'
        (directory / 'BOOTSEL.DAT').write_bytes(initial)
        run(['mcopy', '-i', fat, str(directory / 'BOOTSEL.DAT'), '::/BOOTSEL.DAT'])
    put('UiTest.mode', test_mode.encode())
    put('BOOTX64.EFI', efi.read_bytes())
    put('pci.ids', b'1234  Example Vendor\n\t1111  Example GPU\n')
    if mode in ('edit', 'edit-again', 'cancel', 'repeat', 'restrictions', 'restrictions-block', 'missing-target'):
        put('Config.ini', original)
    if mode == 'diagnostic':
        put('Config.ini', b'[Behavior]\nProbe=true\n')
    if mode in ('recover', 'reject', 'recover-cancel', 'reset', 'reset-cancel'):
        put('Config.ini', b'[invalid]\n')
    if mode == 'reset-preserve-failure':
        put('Config.ini', b'X' * 70000)
    if mode == 'partial':
        put('Config.ini', b'[Behavior]\nProbe=false\n')
    if mode == 'comments':
        put('Config.ini', b'# truncated config\n')
    if mode in ('recover', 'missing', 'recover-cancel', 'partial', 'comments'):
        put('Config.ini.previous', BASE)
    if mode in ('reset', 'reset-cancel'):
        put('Config.ini.previous', b'[also-invalid]\n')
        put('Config.ini.tmp', b'# incomplete\n')
    if mode == 'temp':
        put('Config.ini.tmp', BASE)
    if mode == 'repeat':
        put('Config.ini.previous', BASE.replace(b'Example disk A', b'Older disk'))
    shutil.copyfile(ovmf / 'OVMF_VARS_4M.fd', directory / 'vars.fd')
    # Keep UNIX sockets short and unique even in a long project checkout path.
    with tempfile.TemporaryDirectory(prefix='ncv-fw-') as runtime:
        monitor = str(Path(runtime) / 'qemu.sock')
        with (directory / 'qemu.log').open('w') as log:
            process = subprocess.Popen(['qemu-system-x86_64', '-machine', 'q35', '-m', '256',
                '-net', 'none', '-drive', f'if=pflash,format=raw,readonly=on,file={ovmf}/OVMF_CODE_4M.fd',
                '-drive', f'if=pflash,format=raw,file={directory}/vars.fd',
                '-drive', f'file={disk},format=raw', '-display', 'none',
                '-monitor', f'unix:{monitor},server=on,wait=off'], stdout=log, stderr=log)
            connection = socket.socket(socket.AF_UNIX)
            connection.settimeout(5)
            try:
                deadline = time.monotonic() + 15
                while not Path(monitor).exists():
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError('QEMU failed to start; inspect qemu.log')
                    time.sleep(.1)
                connection.connect(monitor)
                connection.recv(4096)
                def command(text):
                    connection.sendall((text + '\n').encode())
                    time.sleep(.25)
                    connection.recv(16384)
                def key(name):
                    command('sendkey ' + name)
                def screen(name):
                    time.sleep(.75)  # Allow the firmware console to finish repainting.
                    command('screendump ' + str(directory / (name + '.ppm')))
                time.sleep(5)
                screen('initial')
                if mode in ('recover', 'missing', 'temp', 'partial', 'comments', 'recover-cancel'):
                    key('esc' if mode == 'recover-cancel' else 'r')
                    time.sleep(.5)
                if mode in ('edit', 'edit-again', 'cancel', 'repeat', 'restrictions', 'restrictions-block', 'missing-target'):
                    key('f2')
                if mode in ('reset', 'reset-cancel', 'reset-preserve-failure'):
                    key('f2')
                    screen('reset-confirmation')
                    key('esc' if mode == 'reset-cancel' else 'y')
                if mode in ('fresh', 'edit', 'edit-again', 'repeat', 'reset', 'restrictions', 'restrictions-block', 'missing-target', 'save-failure', 'reload-failure'):
                    for name in ('right', 'tab', 'right', 'ret'):
                        key(name)
                    screen('review')
                    # Marker routing stays fixed when the controller changes.
                    if mode != 'restrictions-block':
                        key('a')
                    key('ret')
                if mode in ('cancel', 'cancel-fresh'):
                    key('esc')
                if mode == 'edit-again':
                    screen('first-save-countdown')
                    key('f2')
                    key('ret')
                    screen('second-review')
                    key('ret')
                if mode in ('fresh', 'edit', 'edit-again', 'repeat', 'reset', 'restrictions',
                            'missing-target', 'recover', 'missing', 'temp', 'partial', 'comments'):
                    time.sleep(11)
                if mode == 'reload-failure':
                    screen('reload-error')
                    key('esc')
                screen('result')
                command('quit')
                process.wait(timeout=10)
            finally:
                connection.close()
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
    active, previous = get('Config.ini'), get('Config.ini.previous')
    expected_boot = mode in ('fresh', 'edit', 'edit-again', 'repeat', 'reset', 'restrictions',
                             'missing-target', 'recover', 'missing', 'temp', 'partial', 'comments')
    boot = get('BootTest.result')
    assert bool(boot) == expected_boot, (mode, boot, get('DispatcherTest.result'))
    if boot:
        expected_gpu, expected_disk = (3, 5) if mode in ('recover', 'missing', 'temp', 'partial', 'comments') else (4, 6)
        assert boot == f'GPU={expected_gpu} disk={expected_disk} probe=0 first=0 editing=0 auto=1'.encode(), (mode, boot)
    if padded:
        assert get('DiskName.result') == b'SanDisk Extreme Pro 0 ', (test_mode, get('DiskName.result'))
        assert b'TargetBbsDescription="SanDisk Extreme Pro 0 "' in active, (test_mode, active)
    if mode == 'marker':
        assert get('MarkerTest.result') == b'PASS'
        marker = subprocess.check_output(['mtype', '-i', fat, '::/BOOTSEL.DAT'])
        assert len(marker) == 512 and b'PROFILE=SECONDARY' in marker
    elif mode in ('incomplete', 'cancel-fresh'):
        assert active is None and previous is None
    elif mode == 'diagnostic':
        assert active == b'[Behavior]\nProbe=true\n' and previous is None
    elif mode == 'reload-failure':
        assert active.startswith(b'[invalid]') and previous.startswith(b'[invalid]')
    elif mode == 'edit-again':
        assert active == previous and b'TargetPci=0000:04:00.0' in active
    elif mode == 'restrictions-block':
        assert active == original and previous is None
    elif mode == 'reset-preserve-failure':
        assert active == b'X' * 70000 and previous is None
    elif mode in ('edit', 'repeat', 'restrictions', 'missing-target'):
        assert active and b'TargetPci=0000:04:00.0' in active, (mode, active)
        expected_name = b'"SanDisk Extreme Pro 0 "' if padded else b'Example disk B'
        assert b'TargetBbsDescription=' + expected_name in active, mode
        assert previous == original, mode
        assert active.startswith(original[:original.index(b'[Video]')]), mode
        if mode in ('restrictions', 'restrictions-block'):
            assert b'ExpectedVendor=9999' in active
    elif mode in ('fresh', 'reset', 'save-failure'):
        assert active and active == previous and b'Enabled=false' in active, (mode, active)
        if mode == 'reset':
            assert get('Config.ini.invalid-01') == b'[invalid]\n'
            assert get('Config.ini.previous.invalid-01') == b'[also-invalid]\n'
            assert get('Config.ini.tmp.invalid-01') == b'# incomplete\n'
    elif mode == 'cancel':
        assert active == original and previous is None
    elif mode in ('recover', 'missing', 'temp', 'partial', 'comments'):
        assert active == BASE, (mode, active)
    else:
        assert active == b'[invalid]\n', mode
        if mode == 'reset-cancel':
            assert get('Config.ini.invalid-01') is None
            assert previous == b'[also-invalid]\n'
    print(directory.name + ': PASS', flush=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', action='append', choices=MODES)
    parser.add_argument('--ovmf', type=Path, default=Path('/usr/share/OVMF'))
    parser.add_argument('--efi', type=Path, help='Previously built test harness, never a production EFI')
    args = parser.parse_args()
    for tool in ('qemu-system-x86_64', 'mmd', 'mcopy', 'mtype', 'mkfs.fat'):
        if not shutil.which(tool):
            parser.error('Required tool is missing: ' + tool)
    output_root = ROOT / 'Build/FirmwareTests'
    output_root.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='run-', dir=output_root))
    print('Evidence: ' + str(output), flush=True)
    modes = args.mode or MODES
    efi = args.efi or (build_harness(output) if any(m != 'marker' for m in modes) else None)
    for mode in modes:
        selected_efi = build_harness(output, marker=True) if mode == 'marker' else efi
        scenario(mode, output, selected_efi, args.ovmf)

if __name__ == '__main__':
    main()
