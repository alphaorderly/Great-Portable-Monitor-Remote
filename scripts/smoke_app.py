"""Extract and launch the distributed app without connecting to hardware."""
import argparse
import json
import os
from pathlib import Path
import platform
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--platform', required=True)
args = parser.parse_args()
archive = ROOT / 'release' / f'RemoteKeyMapper-{args.platform}.zip'
with tempfile.TemporaryDirectory(prefix='remote-package-') as tmp:
    tmp = Path(tmp)
    if args.platform.startswith('macos-'):
        subprocess.run(['ditto', '-x', '-k', str(archive), str(tmp)], check=True)
        binary = tmp / archive.stem / 'RemoteKeyMapper.app/Contents/MacOS/RemoteKeyMapper'
        actual = subprocess.check_output(['lipo', '-archs', str(binary)], text=True).strip()
        if actual != args.platform.split('-')[1]:
            raise RuntimeError(f'Wrong Mach-O architecture: {actual}')
    else:
        with zipfile.ZipFile(archive) as z:
            z.extractall(tmp)
        binary = tmp / archive.stem / 'RemoteKeyMapper/RemoteKeyMapper.exe'
        data = binary.read_bytes()
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        machine = struct.unpack_from('<H', data, pe + 4)[0]
        expected = 0xAA64 if args.platform.endswith('arm64') else 0x8664
        if data[pe:pe+4] != b'PE\0\0' or machine != expected:
            raise RuntimeError(f'Wrong PE architecture: {machine:#x}')
    # Qt's generic offscreen plugin on Windows cannot see the system fonts.
    qpa = 'windows' if args.platform.startswith('windows-') else 'offscreen'
    env = {**os.environ, 'QT_QPA_PLATFORM': qpa}
    for variable in ('PYTHONPATH', 'PYTHONHOME'):
        env.pop(variable, None)
    diagnostics = tmp / 'hid-diagnostics.json'
    # This must work in a windowed exe and must not initialize a GUI. An
    # enumeration error is still useful diagnostics, with exit status 1.
    result = subprocess.run([str(binary), '--diagnostics', str(diagnostics)], cwd=tmp,
                            env={**env, 'QT_QPA_PLATFORM': 'diagnostics-must-not-start-qt'}, timeout=60)
    report = json.loads(diagnostics.read_text(encoding='utf-8'))
    if result.returncode != (1 if report['search_error'] else 0):
        raise RuntimeError('Diagnostic exit status does not match search result')
    if report['schema_version'] != 1 or report['raw_count'] != len(report['devices']):
        raise RuntimeError('Invalid search diagnostic schema/count')
    expected_build = json.loads((ROOT / 'build/build-info.json').read_text(encoding='utf-8'))
    if report['app'] != expected_build or report['app']['build_id'] == 'unknown':
        raise RuntimeError('Packaged build identity is missing or incorrect')
    if report['runtime']['hid_package_version'] != '0.15.0' or report['runtime']['hidapi_version'].startswith('unavailable'):
        raise RuntimeError('Packaged HIDAPI version unavailable')
    (ROOT / 'build' / f'diagnostics-summary-{args.platform}.json').write_text(
        json.dumps({key: value for key, value in report.items() if key != 'devices'}, indent=2) + '\n', encoding='utf-8')
    subprocess.run([str(binary), '--self-test'], cwd=tmp, env=env, check=True, timeout=60)
    output = ROOT / 'build' / f'preview-{args.platform}.png'
    output.unlink(missing_ok=True)
    subprocess.run([str(binary), '--screenshot', str(output)], cwd=tmp, env=env, check=True, timeout=60)
    data = output.read_bytes()
    if not data.startswith(b'\x89PNG\r\n\x1a\n') or len(data) < 1000:
        raise RuntimeError('Packaged GUI failed to render a PNG')
    print(f'PASS: extracted {args.platform} app, native architecture, HIDAPI and Qt render ({platform.machine()})')
