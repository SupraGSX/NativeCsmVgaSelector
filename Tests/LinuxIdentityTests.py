"""Run actual Linux revalidation against mocked identities. Never opens a disk.
SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import re
import subprocess
import unittest

INSTALLER = (Path(__file__).resolve().parents[1] / 'Installers/Linux/install.sh').read_text()
def function(name):
    return re.search(r'^' + name + r'\(\) \{\n.*?^\}', INSTALLER, re.M | re.S).group()

class IdentityTests(unittest.TestCase):
    def validate(self, change):
        script = '''set -euo pipefail
USB_PATHS=(/dev/sdz); USB_MAJMIN=(65:144); USB_MODELS=('same model'); USB_SIZES=(1006632960)
USB_SERIALS=(serial-a); USB_WWNS=(wwn-a); USB_SYSFS=(/sys/devices/usb/unit); USB_DISKSEQS=(42)
die() { exit 19; }
disk_is_safe_usb() {
 ID_PATH=/dev/sdz; ID_MAJMIN=65:144; ID_MODEL='same model'; ID_SIZE=1006632960
 ID_SERIAL=serial-a; ID_WWN=wwn-a; ID_SYSFS=/sys/devices/usb/unit; ID_DISKSEQ=42
 ''' + change + '\n}\n' + function('revalidate_selected_disk') + '\nrevalidate_selected_disk 0\n'
        return subprocess.run(['bash', '-c', script], capture_output=True).returncode

    def test_same_connection(self):
        self.assertEqual(self.validate('return 0'), 0)

    def test_replacement_or_reconnection(self):
        for change in ('ID_SERIAL=serial-b', 'ID_WWN=wwn-b', 'ID_DISKSEQ=43',
                       'ID_SYSFS=/sys/devices/usb/other', 'ID_MAJMIN=65:145',
                       "ID_DISKSEQ=''", 'return 1'):
            with self.subTest(change=change):
                self.assertEqual(self.validate(change), 19)

    def test_no_serial_still_detects_new_connection(self):
        self.assertEqual(self.validate("USB_SERIALS=(''); ID_SERIAL=''; ID_DISKSEQ=43"), 19)

    def test_missing_disk_sequence_stops_before_destructive_work(self):
        script = '''set -euo pipefail
die() { exit 19; }
require_commands() { :; }
select_usb_disk() { SELECTED_DISK_INDEX=0; USB_PATHS=(/dev/sdz); USB_DISKSEQS=(''); }
show_disk() { exit 99; }
''' + function('erase_mode') + '\nerase_mode\n'
        self.assertEqual(subprocess.run(['bash', '-c', script], capture_output=True).returncode, 19)

if __name__ == '__main__':
    unittest.main()
