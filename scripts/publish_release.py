"""Publish only complete, tested artifacts; recover drafts without replacing releases."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
release = root / 'release'
tag = os.environ['GITHUB_REF_NAME']
repo = os.environ['GITHUB_REPOSITORY']
if not re.fullmatch(r'v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?', tag):
    raise SystemExit(f'Invalid release tag: {tag}')
platforms = ['macos-arm64', 'macos-x86_64', 'windows-arm64', 'windows-x86_64', 'firmware-esp32s3']
expected = {f'RemoteKeyMapper-{p}.zip' for p in platforms}
actual = {p.name for p in release.glob('*.zip')}
if actual != expected:
    raise SystemExit(f'Incomplete release: expected {sorted(expected)}, received {sorted(actual)}')
checksums = release / 'SHA256SUMS.txt'
checksums.write_text(''.join(f'{hashlib.file_digest((release / name).open("rb"), "sha256").hexdigest()}  {name}\n'
                             for name in sorted(expected)), encoding='utf-8')

def gh(*args):
    subprocess.run(['gh', *args, '--repo', repo], check=True)

result = subprocess.run(['gh', 'release', 'view', tag, '--repo', repo, '--json', 'isDraft,tagName'],
                        text=True, capture_output=True)
if result.returncode == 0:
    if not json.loads(result.stdout)['isDraft']:
        raise SystemExit('Release is already public; refusing to overwrite its assets. Use a new tag.')
else:
    if 'release not found' not in result.stderr.lower() and '404' not in result.stderr:
        raise RuntimeError(result.stderr)
    body = f'''## Remote Key Mapper {tag}

macOS와 Windows의 ARM64/x86_64 앱 및 ESP32-S3 USB HID 펌웨어입니다.
Native ARM64/x86_64 apps for macOS and Windows, plus ESP32-S3 USB HID firmware.

- 밝은 설정창과 버튼 목록·선택 항목 편집 UI를 제공합니다.
- 영문 문자열 매크로: 대문자·기호, Enter·Tab, 랜덤 문자 간격·중간 대기·반복을 지원합니다.
- 매크로를 ESP32-S3에 저장하고 앱을 종료해도 리모컨으로 실행할 수 있습니다.
- USB 제품명은 `USB Keyboard & Mouse`이며, 기존 키 설정과 매크로를 별도로 저장·검증합니다.
- 명시적 OS별 기본값: macOS Command+Tab, Windows/Linux Alt+Tab.
- 네 플랫폼의 테스트, 패키징, 압축 해제 후 실행·화면 렌더링과 ESP-IDF/C 회귀 검사를 통과한 빌드입니다.
- 한국어·영어 README / Korean and English documentation.

ZIP 전체를 해제한 뒤 앱을 실행하세요. 파일 해시는 SHA256SUMS.txt를 참고하세요.
Extract the full ZIP before launching. Verify downloads using SHA256SUMS.txt.

새 앱과 ESP32-S3 펌웨어를 함께 업데이트하세요. 문자열 입력은 미국식 QWERTY·영문 입력·Caps Lock 꺼짐 기준입니다.
Update the app and ESP32-S3 firmware together. String macros require US QWERTY, English input mode, and Caps Lock off.

**S3 USB HID 실기 인식·설정 저장·절전 복귀는 아직 미검증입니다.**
**ESP32-S3 USB HID hardware interoperability remains unverified.**
Apps are not Developer ID notarized or Authenticode signed. x86 means 64-bit x86_64.

[사용 안내 / Guide](https://github.com/{repo}/blob/{tag}/README.md) · [English](https://github.com/{repo}/blob/{tag}/README.en.md)
'''
    with tempfile.TemporaryDirectory() as tmp:
        notes = Path(tmp) / 'notes.md'
        notes.write_text(body, encoding='utf-8')
        options = ['--prerelease'] if '-' in tag else []
        gh('release', 'create', tag, '--verify-tag', '--draft', '--generate-notes',
           '--title', f'Remote Key Mapper {tag}', '--notes-file', str(notes), *options)
gh('release', 'upload', tag, *[str(release / name) for name in sorted(expected)], str(checksums), '--clobber')
# Verify the complete upload before making it visible.
assets = json.loads(subprocess.check_output(['gh', 'release', 'view', tag, '--repo', repo, '--json', 'assets'], text=True))['assets']
if {a['name'] for a in assets} != expected | {'SHA256SUMS.txt'}:
    raise RuntimeError('Draft release asset list is incomplete; not publishing')
gh('release', 'edit', tag, '--draft=false')
