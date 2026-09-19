"""Exercise production packaging policy with harmless temporary files.
SPDX-License-Identifier: GPL-3.0-only
"""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('packager', ROOT / 'Scripts/package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)

class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        package.ROOT = Path(self.temp.name) / 'source'
        package.ROOT.mkdir()
        (package.ROOT / 'README.md').write_text('Synthetic source\n')
        self.manifest(['README.md', 'SOURCE_MANIFEST.txt'])

    def manifest(self, names):
        (package.ROOT / 'SOURCE_MANIFEST.txt').write_text('\n'.join(sorted(names)) + '\n')

    def test_exact_manifest(self):
        self.assertEqual([p.name for p in package.source_files()], ['README.md', 'SOURCE_MANIFEST.txt'])

    def test_private_and_untracked_files(self):
        for name in ('capture.bin', 'private.pem', 'app.debug', 'app.map', 'BOOTX64.EFI', 'unexpected.md'):
            with self.subTest(name=name):
                path = package.ROOT / name
                path.write_text('dummy')
                with self.assertRaises(ValueError):
                    package.source_files()
                path.unlink()

    def test_forbidden_even_if_manifested(self):
        for name in ('capture.BIN', 'key.PEM', 'Config.ini', '.env.secret', '../outside.md'):
            with self.subTest(name=name):
                self.manifest(['README.md', 'SOURCE_MANIFEST.txt', name])
                with self.assertRaises(ValueError):
                    package.source_files()

    def test_symlink_file_and_parent(self):
        outside = Path(self.temp.name) / 'outside'
        outside.mkdir()
        (outside / 'a.md').write_text('dummy')
        (package.ROOT / 'linked.md').symlink_to(outside / 'a.md')
        self.manifest(['README.md', 'SOURCE_MANIFEST.txt', 'linked.md'])
        with self.assertRaises(ValueError):
            package.source_files()
        (package.ROOT / 'linked.md').unlink()
        (package.ROOT / 'linked').symlink_to(outside, target_is_directory=True)
        self.manifest(['README.md', 'SOURCE_MANIFEST.txt', 'linked/a.md'])
        with self.assertRaises(ValueError):
            package.source_files()
        with self.assertRaises(ValueError):
            package.archive(package.ROOT / 'linked/result.zip', [])
        self.assertFalse((outside / 'result.zip').exists())

    def test_archive_replacement(self):
        import zipfile
        target = Path(self.temp.name) / 'result.zip'
        package.archive(target, [('a.txt', b'one', False)])
        package.archive(target, [('b.txt', b'two', False)])
        with zipfile.ZipFile(target) as archive:
            self.assertEqual(archive.namelist(), ['b.txt'])
            self.assertEqual(archive.read('b.txt'), b'two')
        self.assertEqual(list(target.parent.glob('.ncv-zip-*')), [])

if __name__ == '__main__':
    unittest.main()
