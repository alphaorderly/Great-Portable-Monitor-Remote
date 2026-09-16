"""Build a Windows standalone EXE or macOS app ZIP with matching native Python."""
import argparse
import json
import platform
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--platform', choices=['macos-arm64', 'macos-x86_64', 'windows-arm64', 'windows-x86_64'], required=True)
args = parser.parse_args()
os_name, arch = args.platform.split('-')
actual_arch = {'AMD64': 'x86_64', 'aarch64': 'arm64', 'ARM64': 'arm64'}.get(platform.machine(), platform.machine())
if actual_arch != arch or sys.platform != {'macos': 'darwin', 'windows': 'win32'}[os_name]:
    sys.exit(f'Native build required: requested {args.platform}, running {sys.platform}/{actual_arch}')
sys.path.insert(0, str(ROOT / 'python/gui'))
from build_info import source_build_info
metadata_path = ROOT / 'build' / 'build-info.json'
metadata_path.parent.mkdir(parents=True, exist_ok=True)
metadata_path.write_text(json.dumps(source_build_info(ROOT), indent=2) + '\n', encoding='utf-8')
command = [sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean', '--windowed',
           '--onefile' if os_name == 'windows' else '--onedir',
           '--name', 'RemoteKeyMapper', '--distpath', str(ROOT / 'dist'),
           '--workpath', str(ROOT / 'build/pyinstaller'), '--specpath', str(ROOT / 'build'),
           '--paths', str(ROOT / 'python/gui'), '--add-data', f'{metadata_path}:.']
if os_name == 'macos':
    command += ['--target-arch', arch, '--osx-bundle-identifier', 'com.alphaorderly.remote-key-mapper']
subprocess.run(command + [str(ROOT / 'python/run_keymapper.py')], cwd=ROOT, check=True)
release = ROOT / 'release'
release.mkdir(exist_ok=True)
if os_name == 'windows':
    executable = release / f'RemoteKeyMapper-{args.platform}.exe'
    shutil.copy2(ROOT / 'dist/RemoteKeyMapper.exe', executable)
    # Do not leave an obsolete folder bundle available for upload on rebuilds.
    (release / f'RemoteKeyMapper-{args.platform}.zip').unlink(missing_ok=True)
    print(executable)
    sys.exit(0)

stage = ROOT / 'build/package' / f'RemoteKeyMapper-{args.platform}'
if stage.exists():
    shutil.rmtree(stage)
stage.mkdir(parents=True)
name = 'RemoteKeyMapper.app'
shutil.copytree(ROOT / 'dist' / name, stage / name, symlinks=True)
# Keep the documentation paths and screenshots usable inside an offline package.
for file in ['README.md', 'README.en.md']:
    shutil.copy2(ROOT / file, stage / file)
(stage / 'docs').mkdir()
for file in ['FIRMWARE.md', 'WINDOWS_COMPATIBILITY.md', 'RELEASING.md']:
    shutil.copy2(ROOT / 'docs' / file, stage / 'docs' / file)
(stage / 'python/gui').mkdir(parents=True)
shutil.copy2(ROOT / 'python/README.md', stage / 'python/README.md')
for file in (ROOT / 'python/gui').glob('*preview.png'):
    shutil.copy2(file, stage / 'python/gui' / file.name)
archive = release / f'{stage.name}.zip'
# Preserve framework symlinks, bundle permissions and ad-hoc code signatures.
subprocess.run(['ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', str(stage), str(archive)], check=True)
print(archive)
