"""Exercise release validation without contacting GitHub."""
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
ASSETS = {
    'RemoteKeyMapper-windows-arm64.exe',
    'RemoteKeyMapper-windows-x86_64.exe',
    'RemoteKeyMapper-macos-arm64.zip',
    'RemoteKeyMapper-macos-x86_64.zip',
    'RemoteKeyMapper-firmware-esp32s3.zip',
}


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.release = self.root / 'release'
        self.release.mkdir()
        (self.root / 'scripts').mkdir()
        self.script = self.root / 'scripts/publish_release.py'
        shutil.copy2(ROOT / 'scripts/publish_release.py', self.script)
        for name in ASSETS:
            (self.release / name).write_bytes(name.encode())

    def publish(self, assets=ASSETS):
        with patch.dict(os.environ, GITHUB_REF_NAME='v1.2.3', GITHUB_REPOSITORY='test/repo'), \
                patch('subprocess.run', return_value=subprocess.CompletedProcess(
                    [], 0, stdout=json.dumps({'isDraft': True}))) as run, \
                patch('subprocess.check_output', return_value=json.dumps({
                    'assets': [{'name': name} for name in assets | {'SHA256SUMS.txt'}]})):
            runpy.run_path(str(self.script), run_name='__main__')
        return [call.args[0] for call in run.call_args_list]

    def test_exes_and_archives_are_checksummed_and_uploaded_before_publishing(self):
        commands = self.publish()
        checksums = (self.release / 'SHA256SUMS.txt').read_text()
        for name in ASSETS:
            self.assertIn(f'{hashlib.sha256(name.encode()).hexdigest()}  {name}\n', checksums)
        upload = next(command for command in commands if command[1:3] == ['release', 'upload'])
        for name in ASSETS:
            self.assertIn(str(self.release / name), upload)
        self.assertEqual(commands[-1][1:5], ['release', 'edit', 'v1.2.3', '--draft=false'])

    def test_missing_exe_or_obsolete_windows_zip_blocks_all_github_calls(self):
        for obsolete_zip in (False, True):
            with self.subTest(obsolete_zip=obsolete_zip):
                exe = self.release / 'RemoteKeyMapper-windows-x86_64.exe'
                old_zip = exe.with_suffix('.zip')
                if obsolete_zip:
                    old_zip.write_bytes(b'obsolete folder bundle')
                else:
                    exe.unlink()
                with patch.dict(os.environ, GITHUB_REF_NAME='v1.2.3', GITHUB_REPOSITORY='test/repo'), \
                        patch('subprocess.run') as run:
                    with self.assertRaisesRegex(SystemExit, 'Incomplete release'):
                        runpy.run_path(str(self.script), run_name='__main__')
                    run.assert_not_called()
                exe.write_bytes(exe.name.encode())
                old_zip.unlink(missing_ok=True)

    def test_missing_uploaded_exe_keeps_release_private(self):
        with self.assertRaisesRegex(RuntimeError, 'not publishing'):
            self.publish(ASSETS - {'RemoteKeyMapper-windows-arm64.exe'})


if __name__ == '__main__':
    unittest.main()
