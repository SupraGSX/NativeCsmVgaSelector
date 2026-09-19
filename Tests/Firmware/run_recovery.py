#!/usr/bin/env python3
"""Exercise reactive recovery in OVMF; all media are disposable virtual files.
SPDX-License-Identifier: GPL-3.0-only
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import argparse
import shutil
import socket
import subprocess
import tempfile
import time

from run import ROOT, BASE, build_harness, create_disk, run

MODES = ('return', 'cancel', 'assess', 'new', 'typeahead', 'log-fail',
         'no-evidence', 'dirty', 'status', 'cleanup', 'keyboard', 'fatal')


def scenario(parent, efi, suffix):
    mode = 'diag-overlap-' + suffix
    directory = parent / suffix
    directory.mkdir()
    disk = create_disk(directory)
    fat = str(disk) + '@@1048576'
    for name, data in [('BOOTX64.EFI', efi.read_bytes()), ('UiTest.mode', mode.encode()), ('Config.ini', BASE)]:
        file = directory / name
        file.write_bytes(data)
        run(['mcopy', '-i', fat, str(file), '::/EFI/BOOT/' + name])
    if suffix == 'log-fail':
        run(['mmd', '-i', fat, '::/EFI/BOOT/NativeCsmVgaRecovery.log'])
    ovmf = Path('/usr/share/OVMF')
    shutil.copyfile(ovmf / 'OVMF_VARS_4M.fd', directory / 'vars.fd')
    with tempfile.TemporaryDirectory(prefix='ncv-recovery-') as runtime:
        monitor = Path(runtime) / 'qemu.sock'
        with (directory / 'qemu.log').open('w') as log:
            process = subprocess.Popen(['qemu-system-x86_64', '-machine', 'q35', '-m', '256', '-net', 'none',
                '-drive', f'if=pflash,format=raw,readonly=on,file={ovmf}/OVMF_CODE_4M.fd',
                '-drive', f'if=pflash,format=raw,file={directory}/vars.fd',
                '-drive', f'file={disk},format=raw', '-display', 'none', '-monitor', f'unix:{monitor},server=on,wait=off'],
                stdout=log, stderr=log)
            connection = socket.socket(socket.AF_UNIX)
            connection.settimeout(5)
            try:
                deadline = time.monotonic() + 15
                while not monitor.exists():
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError(f'{mode}: QEMU failed to start')
                    time.sleep(.1)
                connection.connect(str(monitor)); connection.recv(4096)

                def command(text):
                    connection.sendall((text + '\n').encode()); time.sleep(.2); connection.recv(16384)

                def get(name):
                    value = subprocess.run(['mtype', '-i', fat, '::/EFI/BOOT/' + name], capture_output=True)
                    return value.stdout if value.returncode == 0 else b''

                time.sleep(20)
                command('stop')
                screen = get('ErrorScreen.txt')
                assert b'BOOT STOPPED' in screen, (mode, screen)
                assert not get('DispatcherTest.result'), (mode, 'returned without fresh user input')
                assert get('BootCallCount.txt') == b'1', mode
                menu = suffix not in ('no-evidence', 'dirty', 'status', 'cleanup', 'fatal')
                assert (b'NCV_ROM_RECOVERY_MENU' in screen) == menu, (mode, screen)
                assert not get('NativeCsmVgaRecovery.log'), (mode, 'assessment ran without opt-in')
                command('screendump ' + str(directory / 'menu.ppm'))
                command('cont')
                if suffix in ('assess', 'new', 'typeahead', 'log-fail'):
                    command('sendkey a'); time.sleep(.7); command('stop')
                    screen = get('ErrorScreen.txt')
                    reason = b'NCV_ALT_ROM_EXECUTOR_UNAVAILABLE' if suffix == 'new' else b'NCV_ALT_ROM_NO_SPLIT_CONTRACT'
                    assert reason in screen, (mode, screen)
                    assert b'Alternative boot is unavailable' in screen
                    assert not get('DispatcherTest.result') and get('BootCallCount.txt') == b'1'
                    assessment = get('NativeCsmVgaRecovery.log')
                    if suffix == 'log-fail':
                        assert b'could not be saved' in screen
                    else:
                        assert reason in assessment and assessment.count(b'NCV_ROM_RECOVERY_READ_ONLY') == 1
                        (directory / 'assessment.log').write_bytes(assessment)
                    command('screendump ' + str(directory / 'assessment.ppm'))
                    command('cont'); command('sendkey a'); time.sleep(.3); command('stop')
                    assert get('NativeCsmVgaRecovery.log') == assessment, 'repeated key duplicated assessment'
                    assert get('BootCallCount.txt') == b'1'
                    command('cont')
                command('sendkey esc' if suffix == 'cancel' else 'sendkey ret')
                time.sleep(.7); command('stop')
                returned = get('DispatcherTest.result')
                if suffix in ('keyboard', 'fatal'):
                    assert not returned
                    assert (b'keyboard unavailable' if suffix == 'keyboard' else b'Restart the PC') in screen
                else:
                    assert returned.startswith(b'returned=') and returned != b'returned=Success', (mode, returned)
                assert get('BootCallCount.txt') == b'1', 'recovery retried boot'
                assert get('Config.ini') == BASE, 'recovery changed configuration'
                (directory / 'ErrorScreen.txt').write_bytes(get('ErrorScreen.txt'))
                (directory / 'DispatcherTest.result').write_bytes(returned)
                command('quit'); process.wait(timeout=10)
            finally:
                connection.close()
                if process.poll() is None:
                    process.terminate(); process.wait(timeout=10)
    print(mode + ': PASS', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', action='append', choices=MODES)
    args = parser.parse_args()
    parent = ROOT / 'Build/FirmwareTests'
    parent.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='recovery-', dir=parent))
    print('Evidence: ' + str(output), flush=True)
    efi = build_harness(output)
    with ThreadPoolExecutor(max_workers=3) as pool:
        futures = [pool.submit(scenario, output, efi, mode) for mode in args.mode or MODES]
        for future in futures:
            future.result()


if __name__ == '__main__':
    main()
