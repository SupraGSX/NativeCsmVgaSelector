#!/usr/bin/env python3
"""Inject boot/log failures in disposable OVMF images and verify visible errors.
No physical storage or GPU handoff. SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import argparse
import shutil
import socket
import subprocess
import tempfile
import time

from run import ROOT, BASE, build_harness, create_disk, run

MODES = ('diag-boot-open', 'diag-log-open', 'diag-preflight', 'diag-log-write',
         'diag-log-flush', 'diag-log-close', 'diag-keyboard', 'diag-fatal',
         'diag-primary-and-log', 'diag-typeahead')


def scenario(output, efi, mode):
    directory = output / mode
    directory.mkdir()
    disk = create_disk(directory)
    fat = str(disk) + '@@1048576'
    for name, content in [('BOOTX64.EFI', efi.read_bytes()), ('UiTest.mode', mode.encode()),
                          ('Config.ini', BASE)]:
        source = directory / name
        source.write_bytes(content)
        run(['mcopy', '-i', fat, str(source), '::/EFI/BOOT/' + name])
    if mode == 'diag-log-open':
        run(['mmd', '-i', fat, '::/EFI/BOOT/NativeCsmVgaBoot.log'])
    ovmf = Path('/usr/share/OVMF')
    shutil.copyfile(ovmf / 'OVMF_VARS_4M.fd', directory / 'vars.fd')
    with tempfile.TemporaryDirectory(prefix='ncv-diag-') as runtime:
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
                        raise RuntimeError('QEMU did not start')
                    time.sleep(.1)
                connection.connect(monitor)
                connection.recv(4096)

                def command(text):
                    connection.sendall((text + '\n').encode())
                    time.sleep(.2)
                    connection.recv(16384)

                def get(name):
                    result = subprocess.run(['mtype', '-i', fat, '::/EFI/BOOT/' + name], capture_output=True)
                    return result.stdout if result.returncode == 0 else b''

                # Countdown, actual boot failure, and several seconds to prove
                # it remains visible rather than instantly returning to BIOS.
                time.sleep(20)
                command('stop')
                screen = get('ErrorScreen.txt')
                assert b'BOOT STOPPED' in screen, (mode, screen)
                assert not get('DispatcherTest.result'), (mode, 'returned without acknowledgment')
                command('screendump ' + str(directory / 'error.ppm'))
                if mode == 'diag-preflight':
                    assert b'NCV_BLOCKED_CSM_CAPABILITY' in screen, screen
                    assert b'Code: NCV_BOOT_FAIL_PREFLIGHT' not in screen, screen
                if mode.startswith('diag-log-') or mode == 'diag-primary-and-log':
                    assert b'BOOT LOG ERROR' in screen and b'Log error:' in screen, screen
                if mode in ('diag-log-write', 'diag-log-flush'):
                    assert b'Code: NCV_BLOCKED_BOOT_CONTROLLER' in screen, screen
                if mode == 'diag-keyboard':
                    assert b'keyboard unavailable' in screen, screen
                if mode == 'diag-primary-and-log':
                    assert b'0x8000000000000011' in screen.split(b'Status:')[1].splitlines()[0], screen
                    assert b'0x800000000000000B' in screen.split(b'Log error:')[1].splitlines()[0], screen
                if mode == 'diag-fatal':
                    assert b'NCV_BOOT_FAIL_ROM_BACKUP_ALLOCATION' in screen, screen
                    assert b'Restart the PC' in screen, screen
                    assert b'Press Enter' not in screen, screen
                    assert b'NCV_BOOT_FAIL_ROM_BACKUP_ALLOCATION' in get('FatalTest.log')
                command('cont')
                command('sendkey esc')
                time.sleep(.3)
                command('stop')
                assert not get('DispatcherTest.result'), (mode, 'wrong key dismissed error')
                command('cont')
                command('sendkey ret')
                time.sleep(.5)
                command('stop')
                returned = get('DispatcherTest.result')
                if mode in ('diag-keyboard', 'diag-fatal'):
                    assert not returned, (mode, returned)
                else:
                    assert returned.startswith(b'returned='), (mode, returned)
                    assert returned != b'returned=Success', (mode, returned)
                (directory / 'ErrorScreen.txt').write_bytes(screen)
                (directory / 'DispatcherTest.result').write_bytes(returned)
                command('quit')
                process.wait(timeout=10)
            finally:
                connection.close()
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
    print(mode + ': PASS', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', action='append', choices=MODES)
    args = parser.parse_args()
    parent = ROOT / 'Build/FirmwareTests'
    parent.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='diagnostics-', dir=parent))
    print('Evidence: ' + str(output), flush=True)
    efi = build_harness(output)
    for mode in args.mode or MODES:
        scenario(output, efi, mode)


if __name__ == '__main__':
    main()
