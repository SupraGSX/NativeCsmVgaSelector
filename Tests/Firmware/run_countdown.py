#!/usr/bin/env python3
"""Exercise split five-second setup/switch windows in disposable OVMF guests.
The dispatcher/countdown/INI/UI are real; successful preflight/handoff is a stub.
No real GPU switch or disk-controller writes. SPDX-License-Identifier: GPL-3.0-only
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
from run import ROOT, BASE, build_harness, create_disk, run

MODES = ('unit', 'order', 'early-cancel', 'early-edit', 'cancel', 'edit-cleanup',
         'preflight-edit', 'readonly', 'overlap-edit', 'overlap-assessed')

def scenario(parent, efi, suffix):
    mode = 'countdown-' + suffix
    directory = parent / suffix
    directory.mkdir()
    disk = create_disk(directory)
    fat = str(disk) + '@@1048576'
    config = BASE.replace(b'[Behavior]\n', b'[Behavior]\nAutoBoot=false\n') if suffix == 'readonly' else BASE
    for name, data in [('BOOTX64.EFI', efi.read_bytes()), ('UiTest.mode', mode.encode()), ('Config.ini', config)]:
        file = directory / name
        file.write_bytes(data)
        run(['mcopy', '-i', fat, str(file), '::/EFI/BOOT/' + name])
    ovmf = Path('/usr/share/OVMF')
    shutil.copyfile(ovmf / 'OVMF_VARS_4M.fd', directory / 'vars.fd')
    with tempfile.TemporaryDirectory(prefix='ncv-count-') as runtime:
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
                    connection.sendall((text + '\n').encode()); time.sleep(.15); connection.recv(16384)
                def get(name):
                    result = subprocess.run(['mtype', '-i', fat, '::/EFI/BOOT/' + name], capture_output=True)
                    return result.stdout if result.returncode == 0 else b''
                def wait_for(name, needle, timeout=25):
                    # Inspect only paused guests, returning paused on a match.
                    deadline = time.monotonic() + timeout
                    while time.monotonic() < deadline:
                        command('stop')
                        value = get(name)
                        if needle in value:
                            return value
                        command('cont'); time.sleep(.25)
                    raise AssertionError((suffix, name, needle, get('ErrorScreen.txt')))
                if suffix == 'unit':
                    result = wait_for('CountdownUnit.result', b'PASS ')
                    (directory / 'CountdownUnit.result').write_bytes(result)
                elif suffix == 'readonly':
                    wait_for('DispatcherTest.result', b'returned=Success')
                    screen = get('ErrorScreen.txt')
                    assert b'GPU switch in' not in screen and b'Preflight starts in' not in screen
                    assert b'auto=0' in get('BootTest.result')
                else:
                    wait_for('ErrorScreen.txt', b'Preflight starts in 5 seconds.')
                    assert not get('BootCallCount.txt') and not get('CountdownTest.log'), 'core ran during early F2 window'
                    assert b'GPU switch in' not in get('ErrorScreen.txt')
                    command('screendump ' + str(directory / 'setup-window.ppm'))
                    command('cont')
                    if suffix in ('early-edit', 'early-cancel'):
                        command('sendkey ' + ('f2' if suffix == 'early-edit' else 'esc'))
                        if suffix == 'early-edit':
                            wait_for('SetupEntered.result', b'yes')
                            assert not get('BootCallCount.txt'), 'early F2 ran preflight before setup'
                            command('cont')
                            for key in ('ret', 'ret'):
                                command('sendkey ' + key); time.sleep(.3)
                        else:
                            wait_for('DispatcherTest.result', b'returned=Aborted')
                            assert not get('BootCallCount.txt') and not get('CountdownTest.log')
                    if suffix != 'early-cancel':
                        if suffix in ('preflight-edit', 'overlap-edit', 'overlap-assessed'):
                            screen = wait_for('ErrorScreen.txt', b'Press Enter' if suffix == 'preflight-edit' else b'NCV_ROM_RECOVERY_MENU')
                            assert b'GPU switch in' not in screen
                            assert b'F2: edit GPU' in screen
                            command('cont')
                            if suffix == 'overlap-assessed':
                                command('sendkey a'); time.sleep(.5)
                            command('sendkey f2'); time.sleep(.5)
                            if suffix != 'overlap-assessed':
                                for key in ('ret', 'ret'):
                                    command('sendkey ' + key); time.sleep(.3)
                        else:
                            screen = wait_for('ErrorScreen.txt', b'GPU switch in 5 seconds.')
                            assert screen.index(b'Preflight starts in 5 seconds.') < screen.index(b'TEST PREFLIGHT COMPLETE') < screen.index(b'GPU switch in 5 seconds.')
                            assert not get('BootTest.result')
                            command('screendump ' + str(directory / 'countdown.ppm'))
                            command('cont')
                            command('sendkey ' + ('esc' if suffix == 'cancel' else 'f2' if suffix == 'edit-cleanup' else 'ret'))
                        if suffix in ('order', 'early-edit', 'preflight-edit', 'overlap-edit'):
                            wait_for('DispatcherTest.result', b'returned=Success')
                            screen = get('ErrorScreen.txt'); file_log = get('CountdownTest.log')
                            assert get('BootTest.result')
                            assert file_log.index(b'NCV_BOOT_PREFLIGHT_COMPLETE') < file_log.index(b'NCV_BOOT_INPUT_COUNTDOWN_BEGIN') < file_log.index(b'NCV_BOOT_INPUT_COUNTDOWN_COMPLETE') < file_log.index(b'TEST_HANDOFF_REACHED')
                            assert b'TEST_HANDOFF_REACHED' not in screen
                            assert b'GPU switch in 6 seconds.' not in screen
                            for remaining in range(5, 0, -1):
                                assert f"GPU switch in {remaining} second{'s' if remaining != 1 else ''}.".encode() in screen
                            if suffix in ('preflight-edit', 'overlap-edit'):
                                assert get('SetupAfterCleanup.result') == b'clean' and get('BootCallCount.txt') == b'2'
                            else:
                                assert get('BootCallCount.txt') == b'1'
                        elif suffix == 'overlap-assessed':
                            time.sleep(.6); command('stop')
                            assert b'Alternative boot is unavailable' in get('ErrorScreen.txt')
                            assert not get('SetupEntered.result') and not get('BootTest.result')
                            assert get('BootCallCount.txt') == b'1' and not get('DispatcherTest.result')
                            command('cont'); command('sendkey ret')
                            wait_for('DispatcherTest.result', b'returned=Unsupported')
                        elif suffix == 'cancel':
                            wait_for('DispatcherTest.result', b'returned=Aborted')
                            assert not get('BootTest.result')
                            assert b'NCV_BOOT_INPUT_CANCELLED' in get('CountdownTest.log')
                        else:
                            screen = wait_for('ErrorScreen.txt', b'BOOT STOPPED')
                            assert b'Device Error' in screen and not get('BootTest.result') and not get('DispatcherTest.result')
                            command('cont'); command('sendkey f2'); time.sleep(.4); command('stop')
                            assert not get('SetupEntered.result') and not get('BootTest.result')
                            command('cont'); command('sendkey ret')
                            wait_for('DispatcherTest.result', b'returned=Device Error')
                    assert not get('SetupOwnership.error')
                for name in ('ErrorScreen.txt','CountdownTest.log','DispatcherTest.result','BootCallCount.txt'):
                    (directory/name).write_bytes(get(name))
                command('quit'); process.wait(timeout=10)
            finally:
                connection.close()
                if process.poll() is None:
                    process.terminate(); process.wait(timeout=10)
    print(mode + ': PASS', flush=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--efi', type=Path)
    parser.add_argument('--mode', action='append', choices=MODES)
    args = parser.parse_args()
    parent = ROOT / 'Build/FirmwareTests'
    parent.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='countdown-', dir=parent))
    print('Evidence: ' + str(output), flush=True)
    efi = args.efi or build_harness(output)
    with ThreadPoolExecutor(max_workers=3) as pool:
        list(pool.map(lambda mode: scenario(output, efi, mode), args.mode or MODES))

if __name__ == '__main__':
    main()
