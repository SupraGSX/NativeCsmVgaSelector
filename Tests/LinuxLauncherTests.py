#!/usr/bin/env python3
"""Regression tests for Run as a Program on the executable desktop launcher.
SPDX-License-Identifier: GPL-3.0-only
"""
import os
from pathlib import Path
import pty
import select
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LinuxLauncherTests(unittest.TestCase):
    def check_launch(self, missing_installer=False):
        # Exercise the actual executable file header, not a parsed Exec key.
        # The installer stub never queries or modifies disks.
        with tempfile.TemporaryDirectory(prefix="selector launcher ") as directory:
            folder = Path(directory) / "space ' quote $ folder"
            folder.mkdir()
            for name in ("NativeCsmVgaSelector-Installer.desktop", "launch.sh"):
                shutil.copy2(ROOT / "Installers/Linux" / name, folder / name)
            if not missing_installer:
                (folder / "install.sh").write_text(
                    '#!/bin/bash\nprintf "TEST MENU: "\nread -r answer\n'
                    '[[ $answer == 3 ]] || exit 9\nexit 0\n'
                )
            master, slave = pty.openpty()
            process = subprocess.Popen(
                [str(folder / "NativeCsmVgaSelector-Installer.desktop")],
                stdin=slave, stdout=slave, stderr=slave,
                cwd=directory, start_new_session=True,
            )
            os.close(slave)
            output = bytearray()

            def wait_for(text):
                deadline = time.monotonic() + 10
                while text not in output:
                    self.assertLess(time.monotonic(), deadline, output.decode(errors="replace"))
                    if select.select([master], [], [], 0.1)[0]:
                        try:
                            chunk = os.read(master, 65536)
                        except OSError:
                            chunk = b""
                        self.assertTrue(chunk, output.decode(errors="replace"))
                        output.extend(chunk)
                self.assertIsNone(process.poll(), output.decode(errors="replace"))

            try:
                if not missing_installer:
                    wait_for(b"TEST MENU: ")
                    os.write(master, b"3\n")
                else:
                    wait_for(b"Installer stopped (exit 127)")
                wait_for(b"Press Enter to close this installer window.")
                self.assertIsNone(process.poll())
                os.write(master, b"\n")
                self.assertEqual(process.wait(timeout=5), 127 if missing_installer else 0)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                os.close(master)

    def test_direct_execution_holds_result(self):
        self.check_launch()

    def test_direct_execution_holds_error(self):
        self.check_launch(missing_installer=True)


if __name__ == "__main__":
    unittest.main()
