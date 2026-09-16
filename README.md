[한국어](README.md) | [English](README.en.md)

# Great Portable Monitor Remote

그레이트 포터블 모니터 리모컨으로 macOS·Windows 컴퓨터를 조작하는 프로젝트입니다.

ESP32-S3가 리모컨의 BLE 신호를 받아 컴퓨터에 키보드·마우스·볼륨 입력으로 전달합니다. **Remote Key Mapper** 앱에서 버튼별 동작을 설정할 수 있으며, 일반 모드와 마우스 커서 모드의 설정은 따로 저장합니다. 설정을 ESP32-S3에 저장하면 앱을 종료해도 그대로 사용할 수 있습니다.

```text
리모컨 ── BLE ── ESP32-S3 ── USB HID ── macOS / Windows
설정 앱도 선택한 보드의 HID 연결로 매핑을 읽고 저장합니다.
```

## 다운로드

[최신 릴리스](https://github.com/alphaorderly/Great-Portable-Monitor-Remote/releases/latest)에서 컴퓨터에 맞는 ZIP을 받아 전체 압축을 해제하세요. Python을 별도로 설치하지 않아도 됩니다.

| 다운로드 파일 | 대상 | 실행 파일 또는 구성 |
|---|---|---|
| `macos-arm64` | Apple Silicon, macOS 13 이상 | `RemoteKeyMapper.app` |
| `macos-x86_64` | Intel Mac, macOS 13 이상 | `RemoteKeyMapper.app` |
| `windows-arm64` | Windows 11 ARM64 | `RemoteKeyMapper/RemoteKeyMapper.exe` |
| `windows-x86_64` | Windows 11 Intel/AMD 64비트 | `RemoteKeyMapper/RemoteKeyMapper.exe` |
| `firmware-esp32s3` | ESP32-S3 N16R8 | USB HID 펌웨어 BIN과 설치 주소 |

ESP32-S3 N16R8 전용 프로젝트입니다. 펌웨어 ZIP은 `firmware-esp32s3`를 사용하세요. 앱은 macOS 공증과 Windows 코드 서명을 받지 않아 실행 시 보안 경고가 표시될 수 있습니다. 다운로드한 파일의 해시는 릴리스의 `SHA256SUMS.txt`에서 확인할 수 있습니다.

## 시작하기

1. ESP32-S3에 [펌웨어](docs/FIRMWARE.md)를 설치하고 리모컨의 전원을 켭니다. 최초 페어링은 [리모컨 연결 안내](docs/FIRMWARE.md#리모컨과-호스트-연결)를 참고하세요.
2.  USB/OTG 포트를 PC에 연결합니다. 별도 UART/COM 포트는 플래시·로그용이며 PC Bluetooth 페어링은 필요하지 않습니다.
3. 앱을 열고 **기기 찾기 → 연결**을 눌러 ESP32-S3에 저장된 설정을 불러옵니다.
4. 편집할 **일반 모드 / 마우스 커서 모드**와 버튼 동작을 선택합니다.
5. **ESP32-S3에 적용·저장**을 누릅니다. 앱이 저장된 설정을 확인하면 완료 메시지가 표시됩니다.

조합 키 표시는 **Ctrl / Control, Shift, Alt / Option, Win / Command**입니다. 복사 단축키를 지정하려면 Windows에서는 Ctrl+C, macOS에서는 Command+C를 선택하세요.

앱목록 버튼의 초기 설정은 Command+Tab입니다. Windows에서 처음 사용할 때는 **설정 메뉴**에서 Windows를 선택하고, 두 모드에서 각각 **기본값 불러오기**를 누른 뒤 저장하세요. 앱목록 버튼이 Alt+Tab으로 바뀝니다. OS를 선택하는 것만으로는 저장된 설정이 바뀌지 않습니다.

![Remote Key Mapper](python/gui/keymapper-preview.png)

앱은 한국어로 제공됩니다.

## 기능과 제한 사항

S3 USB HID의 macOS·Windows 실기 동작은 아직 미검증입니다. 펌웨어 빌드·모의 테스트와 실제 하드웨어 검증은 구분합니다.

- 방향키, 문자·숫자·기호, F1~F24, 탐색 키, 볼륨, 마우스 세 버튼과 조합 키를 지원합니다.
- 마우스 커서 모드의 **마우스 속도** 슬라이더로 0.25~3배(기본 1배)를 설정하고 **ESP32-S3에 적용·저장**할 수 있습니다.
- 마우스 모드에서 홈 버튼은 커서 일시 정지 전용입니다. 누르는 동안의 움직임은 버리고, 놓으면 현재 커서 위치에서 다시 움직입니다. 일반 모드의 홈 버튼 설정은 유지됩니다.
- 이 기능은 새 앱과 펌웨어가 함께 필요합니다. 기존 기기 설정은 속도 1배로 불러오며, 마우스 모드의 홈 버튼 할당은 일시 정지 전용으로 바뀝니다.
- 커서 버튼은 모드 전환 전용으로, 다른 동작을 지정할 수 없습니다.
- 입력 디버깅으로 리모컨 버튼 입력을 확인할 수 있습니다. Windows에서는 컴퓨터로 전달된 키보드·마우스 출력이 기록에 보이지 않을 수 있습니다.
- 설정 앱은 통신이 끊기면 선택한 기기에 자동 재연결합니다. 저장 명령은 자동으로 반복하지 않습니다.
- **이번 연결 복구 변경은 macOS·Windows에서 실제 하드웨어 검증이 아직 필요합니다. Windows의 BLE 설정 저장도 미검증입니다.** 자세한 내용은 [Windows 호환성 검토](docs/WINDOWS_COMPATIBILITY.md)를 참고하세요.

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

Windows ARM64에서는 ARM64 Python과 Visual Studio C++ ARM64 빌드 도구가 필요합니다. `hidapi==0.15.0`을 소스에서 빌드하기 때문입니다. 빌드 없이 사용하려면 릴리스의 앱 ZIP을 받으세요. Linux용 배포 패키지는 제공하지 않습니다.

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

`main`에 푸시하거나 PR을 열면 CI에서 네 플랫폼의 앱 빌드·테스트와 ESP32-S3 펌웨어 빌드·C 테스트를 실행합니다. `v*` 태그를 푸시하면 모든 검증을 통과한 뒤 앱 4종, ESP32-S3 펌웨어 1종, SHA-256 체크섬을 GitHub Release에 게시합니다. 자세한 절차는 [배포 문서](docs/RELEASING.md)를 참고하세요.

| 폴더 | 내용 |
|---|---|
| `python/` | Qt GUI, HID 설정 통신, 테스트 |
| `firmware/` | ESP32-S3 단일 ESP-IDF 프로젝트, BLE 수신·USB 출력, NimBLE 패치, C 테스트 |
| `shared/fixtures/` | Python/C 공통 프로토콜 검증 데이터 |
| `scripts/`, `.github/workflows/` | 앱 패키징, 검증, 자동 릴리스 |
| `docs/` | 사용법, 호환성, 배포 안내 |

시리얼 로그만 보려면 macOS에서 `./monitor-macos.sh`, Windows에서 `.\monitor-windows.cmd`를 실행하세요. [모니터링 사용법](docs/FIRMWARE.md#플래시-없이-시리얼-로그만-보기)을 참고하세요.

[Python 상세 안내](python/README.md) · [펌웨어 설치](docs/FIRMWARE.md) · [Windows 검토](docs/WINDOWS_COMPATIBILITY.md)

## 문자열 매크로

새 앱·펌웨어는 버튼별 영문 문자열, 대문자·기호, Enter·Tab, 랜덤 문자 간격과 중간 대기, 최대 100회 반복을 지원합니다. **문자열 매크로** 탭에서 읽기·편집·저장하며, 저장 후 앱을 종료해도 리모컨으로 실행할 수 있습니다. 새 USB 제품명은 `USB Keyboard & Mouse`입니다. 사용 방법과 입력 배열 조건은 [매크로 안내](python/README.md#문자열-매크로)를 참고하세요.
