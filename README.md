[한국어](README.md) | [English](README.en.md)

# Great Portable Monitor Remote

**그레이트 포터블 모니터 리모컨을 macOS·Windows의 키보드와 마우스로 사용하세요.**

ESP32가 BLE 리모컨의 입력을 받아 컴퓨터에 표준 Bluetooth HID 키보드·마우스·볼륨 입력으로 전달합니다. **Remote Key Mapper**에서 일반 모드와 마우스 커서 모드의 버튼 할당을 편집하고 ESP32에 저장할 수 있습니다. 저장한 설정은 프로그램을 종료해도 유지됩니다.

```text
리모컨 ── BLE ── ESP32 ── BLE HID ── macOS / Windows
                    ↑                   │
                    └── 키 매핑 저장 ────┘
```

## 다운로드

[최신 릴리스](https://github.com/alphaorderly/Great-Portable-Monitor-Remote/releases/latest)에서 컴퓨터에 맞는 ZIP을 받아 전체 압축을 해제하세요. Python을 별도로 설치하지 않아도 됩니다.

| 파일 이름의 플랫폼 | 대상 | 실행 파일 |
|---|---|---|
| `macos-arm64` | Apple Silicon, macOS 13 이상 | `RemoteKeyMapper.app` |
| `macos-x86_64` | Intel Mac, macOS 13 이상 | `RemoteKeyMapper.app` |
| `windows-arm64` | Windows 11 ARM64 | `RemoteKeyMapper/RemoteKeyMapper.exe` |
| `windows-x86_64` | Windows 11 Intel/AMD 64비트 | `RemoteKeyMapper/RemoteKeyMapper.exe` |
| `firmware-esp32` | ESP32용 공통 펌웨어 | 플래시용 BIN과 주소 정보 |

여기서 x86은 **x86_64**를 뜻합니다. 32비트 Windows와 ESP32-S2/S3/C3 등 다른 칩용 펌웨어는 이 릴리스의 대상이 아닙니다. 앱은 macOS Developer ID 공증 및 Windows Authenticode 서명이 없어 OS 보안 확인이 표시될 수 있습니다. 릴리스의 `SHA256SUMS.txt`로 파일 해시를 비교할 수 있습니다.

## 시작하기

1. ESP32에 [펌웨어](docs/FIRMWARE.md)를 설치하고 리모컨의 전원을 켭니다. 최초 페어링은 [리모컨 연결 안내](docs/FIRMWARE.md#리모컨과-호스트-연결)를 참고하세요.
2. 컴퓨터의 Bluetooth 설정에서 **ESP32 Remote Bridge**를 페어링합니다.
3. 앱을 열고 **기기 찾기 → 연결**을 누릅니다. 장치의 현재 설정을 읽습니다.
4. 편집할 **일반 모드 / 마우스 커서 모드**와 버튼 동작을 선택합니다.
5. **ESP32에 적용·저장**을 누릅니다. 장치에서 다시 읽어 일치하는지 확인한 후 저장 완료를 표시합니다.

조합 키는 모든 OS에서 **Ctrl / Control, Shift, Alt / Option, Win / Command**를 함께 표시합니다. Control과 Command는 서로 다른 HID 키입니다. 예를 들어 복사는 Windows에서 Ctrl+C, macOS에서 Command+C로 지정합니다.

**기본값 대상**에서 OS를 고르고 **현재 모드 기본값**을 누르면 앱목록 버튼을 macOS는 Command+Tab, Windows/Linux는 Alt+Tab으로 설정합니다. 기존 장치 설정은 자동 변환하지 않습니다. Windows에서 처음 사용할 때는 두 모드 각각의 기본값을 불러온 뒤 저장하세요. 펌웨어의 공장 기본값은 기존 장치와의 호환성을 위해 Command+Tab입니다.

![Remote Key Mapper](python/gui/keymapper-preview.png)

실제 Qt 앱의 미연결 화면입니다. 프로그램 UI는 한국어이며, 이 저장소의 사용 안내는 한국어·영어로 제공합니다.

## 지원 범위와 검증

- 방향키, 문자·숫자·기호, F1~F24, 탐색 키, 볼륨, 마우스 세 버튼과 조합 키를 지원합니다.
- 일반 모드/마우스 모드의 매핑은 따로 저장합니다. 커서 버튼은 모드 전환 전용입니다.
- GUI는 전용 Vendor HID 컬렉션만 열며 전역 키 입력을 수집하지 않습니다. macOS에서는 공유 접근을 사용합니다.
- Windows는 키보드·마우스 컬렉션을 OS가 독점합니다. GUI의 원본 리모컨 기록은 지원하지만 실제 출력 기록은 이 연결에 전달되지 않을 수 있습니다.
- CI는 네 플랫폼의 Python/Qt 테스트, 앱 패키징과 압축 해제 후 실행, ESP-IDF 빌드와 C 모의 테스트를 검사합니다. **Windows 실제 BLE 페어링·설정 저장·절전 복귀는 아직 하드웨어 미검증입니다.** 자세한 근거와 확인 절차는 [Windows 호환성 검토](docs/WINDOWS_COMPATIBILITY.md)를 참고하세요.

## 소스에서 실행

Python 3.12를 사용합니다. 저장소 루트에서 실행하세요.

macOS:

```sh
python3 -m venv .venv-gui
.venv-gui/bin/python -m pip install -r python/requirements.txt
.venv-gui/bin/python python/run_keymapper.py
```

Windows PowerShell:

```powershell
py -3.12 -m venv .venv-gui
.venv-gui\Scripts\python.exe -m pip install -r python/requirements.txt
.venv-gui\Scripts\python.exe python/run_keymapper.py
```

Windows ARM64에서는 ARM64 Python을 사용하세요. `hidapi==0.15.0`은 ARM64 휠이 없어 소스 빌드에 Visual Studio C++ ARM64 빌드 도구가 필요합니다. CI는 해당 도구로 빌드합니다. 일반 사용자는 위의 완성된 앱 ZIP을 이용하면 됩니다. Linux 소스 실행도 고려한 코드지만 Linux 배포 패키지는 제공하지 않습니다.

## 개발과 릴리스

```sh
# 가상환경 Python으로 실행 (Windows에서는 Scripts/python.exe)
.venv-gui/bin/python -m unittest discover -s python/tests -v
.venv-gui/bin/python python/run_keymapper.py --self-test
# 활성화된 ESP-IDF 5.5.5 터미널
python firmware/scripts/build.py -B build build
# C sanitizer 테스트: macOS/Linux/WSL + cc, 패치된 ESP-IDF SDK
python firmware/tests/native/run_tests.py --idf-path /path/to/esp-idf
```

`main` 푸시와 PR에서 빌드·테스트합니다. `v*` 태그를 푸시하면 같은 검증을 거친 앱 4종, 펌웨어, SHA-256 목록을 첨부해 GitHub Release를 자동 작성합니다. 릴리스는 모든 빌드가 성공한 뒤 공개됩니다. 빌드 스크립트 사용법은 [배포 문서](docs/RELEASING.md)를 참고하세요.

| 폴더 | 내용 |
|---|---|
| `python/` | Qt GUI, HID 설정 통신, 테스트 |
| `firmware/` | ESP-IDF 소스, NimBLE 패치, C 테스트 |
| `shared/fixtures/` | Python/C 공통 프로토콜 검증 데이터 |
| `scripts/`, `.github/workflows/` | 앱 패키징, 검증, 자동 릴리스 |
| `docs/` | 현재 사용·호환성·배포 안내 |

[Python 상세 안내](python/README.md) · [펌웨어 설치](docs/FIRMWARE.md) · [Windows 검토](docs/WINDOWS_COMPATIBILITY.md)

과거 페어링·캡처 조사 기록은 로컬에 보존하며 공개 저장소와 앱 패키지에 포함하지 않습니다. 로컬 SDK, 가상환경, 빌드 산출물과 원시 Bluetooth 로그도 Git에 포함하지 않습니다.
