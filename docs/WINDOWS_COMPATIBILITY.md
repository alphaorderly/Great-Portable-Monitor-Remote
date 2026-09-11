[한국어](../README.md) | [English](../README.en.md)

# Windows 호환성 검토 / Windows compatibility review

검토일: 2026-09-11. **코드·프로토콜상 Windows를 지원할 수 있는 구조이며, 실제 ESP32↔Windows BLE 동작은 아직 미검증입니다.** 과거 `WINDOWS_CAPTURE_ANALYSIS.md`는 원래 리모컨과 Windows 간 캡처이며 이 ESP32 브리지의 실기 결과가 아닙니다.

The bridge uses standard BLE HID and is designed to work with Windows. Native Windows build/test success does not prove Bluetooth interoperability. The old Windows capture concerns the original remote, not this ESP32 bridge.

## 코드 검토 결과

| 항목 | 근거와 처리 |
|---|---|
| ESP32 실행 코드 | ESP-IDF/NimBLE에서 실행하며 Win32 또는 macOS API 의존성이 없음. 컴퓨터 CPU가 ARM64/x86_64여도 같은 ESP32 바이너리 사용 |
| 표준 HID | Service 0x1812, HID Information, Report Map, Control Point, Report Reference. 키보드 ID 2=8 bytes, 마우스 ID 3=4 bytes, 볼륨 ID 4=1 byte |
| GUI 접근 | 독립 Vendor TLC FF00/1의 Input ID 1과 Feature ID 5 사용. Windows가 독점하는 키보드/마우스 TLC를 열지 않음 |
| 키보드 조합 | Control=1, Shift=2, Alt=4, GUI=8. GUI bit는 Windows 키 또는 macOS Command. 서로 교환하면 안 됨 |
| 앱 전환 | 공장 기본값 Command+Tab은 Windows에서 Win+Tab. GUI의 Windows 기본값을 명시적으로 불러오면 Alt+Tab으로 저장 |
| 암호화와 본딩 | 중앙 호스트의 암호화 시작 또는 암호화 필수 GATT 접근을 기다림. 기존 본드와 구독 게이트 유지; 암호화 15초 제한과 연결 종료 복구 추가. Windows 최초 페어링·복귀 실기 확인 필요 |
| 설정 저장 | GATT payload 64 bytes, HIDAPI ID 포함 65 bytes. ESP32 길이/revision 검증과 NVS 저장 후 GUI readback. Windows BLE Feature read/write·MTU/long-write 실제 경로는 하드웨어 확인 필요 |
| Python 플랫폼 분리 | Darwin 공유 접근은 macOS에서만 호출. 공통 실행 스크립트, 시스템 글꼴, OS 중립 상태/디버깅 문구 |
| Windows ARM64 | Qt/PyInstaller ARM64 패키지와 ARM64 Python 사용. hidapi 0.15.0은 ARM64 휠 미제공이므로 MSVC로 네이티브 소스 빌드 |
| 디버깅 | Windows에서는 원본 Vendor 입력을 기록. 다른 TLC의 실제 출력은 GUI에 전달되지 않을 수 있음. 이를 키 입력 실패로 판단하지 않음 |

`mac_hid.c` 호환 진입점, `mac_*` 내부 식별자와 NVS의 `mac` 키는 기존 이름입니다. OS를 탐지하거나 Mac만 허용하는 조건이 아니며, 이름 변경으로 저장된 페어링을 잃지 않도록 유지했습니다. 표시 로그는 Host로 정리했습니다.

## 자동 검증

- Python/Qt: 기존 입력·매핑·플랫폼 검사와 연결 복구 검사. 16가지 modifier 조합, Command/Control 구분, OS별 기본값, 연결 시 저장 매핑 보존, Darwin API 격리.
- C: HID descriptor/Reference 길이, 암호화/구독, 저장·readback 프로토콜, 모든 modifier 조합의 실제 출력 및 해제, 모드 전환, 혼잡·재연결과 NimBLE RPA identity 회귀.
- CI: macOS ARM64/x86_64, Windows ARM64/x86_64에서 테스트 및 패키징. 압축 해제한 앱을 하드웨어 없는 self-test와 screenshot 모드로 실행.
- ESP-IDF 5.5.5: ESP32 빌드와 공통 펌웨어 아카이브 생성.

실행 결과는 해당 커밋의 [Actions](https://github.com/alphaorderly/Great-Portable-Monitor-Remote/actions)를 확인하세요. Windows의 Darwin 전용 테스트 한 개는 의도적으로 skip합니다.

## 남은 Windows 하드웨어 확인

Windows 11 x86_64 및 ARM64 각각에서 다음을 확인해야 실기 지원을 확정할 수 있습니다.

1. ESP32 앱을 설치하고 OS Bluetooth 설정에서 `ESP32 Remote Bridge`를 처음 페어링합니다.
2. GUI를 종료한 상태에서 방향키, Enter, 마우스 이동/클릭/휠, 볼륨을 확인합니다.
3. GUI에서 기기를 검색·연결하고 Feature 설정을 읽습니다. Windows 기본값을 두 모드에 불러와 저장·readback 완료를 확인합니다.
4. Ctrl+C, Alt+Tab, Win 키 조합을 각각 지정해 구분되는지 확인하고 키를 놓았을 때 해제되는지 확인합니다.
5. GUI를 닫고 ESP32 전원을 재시작하여 저장 유지와 자동 재연결을 확인합니다.
6. Windows 절전/복귀, Bluetooth 끄기/켜기, 리모컨 지연 기동, 호스트를 Mac↔Windows로 바꾸는 경우를 확인합니다.
7. 원본 Vendor 입력은 보이지만 호스트 출력 로그가 없는 경우, 메모장 등에서 실제 입력을 따로 확인합니다.

Hardware checklist (both Windows architectures): first pairing; native keys/mouse/volume without the GUI; Feature read/write and verified save; distinct Ctrl/Alt/Win shortcuts and releases; persistence after power cycle; sleep/resume and reconnection; switching hosts. Missing native-output debug rows alone do not imply failure.

## 공식 근거

- [Microsoft: keyboard and mouse HID client drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/keyboard-and-mouse-hid-client-drivers): OS의 키보드/마우스 TLC 독점과 별도 Vendor TLC 권장.
- [HIDAPI API](https://libusb.info/hidapi/group__API.html): Feature Report에서 첫 바이트 Report ID 사용.
- [hidapi 0.15.0 배포 파일](https://pypi.org/project/hidapi/0.15.0/#files): Windows ARM64 휠 부재.
- [PySide6 6.11.2](https://pypi.org/project/PySide6/6.11.2/#files), [PyInstaller 6.22.2](https://pypi.org/project/pyinstaller/6.22.2/#files): 각 플랫폼 배포 패키지.
- [GitHub hosted runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners): 네 플랫폼의 네이티브 실행 환경.

## 연결 복구 변경 검증 기록 — 2026-09-11

이번 변경은 호스트 연결 관리, HID 서비스, 입력 전송을 분리하고 보안 타임아웃·일반 광고 복귀·앱 자동 재연결을 추가했습니다. HID Report ID/길이, 기존 NVS 키, 리모컨 연결 정책과 보안 수준은 유지합니다. 설정 앱은 HID 설정 읽기 성공 뒤에만 사용 가능 상태를 표시하며 OS의 암호화 상태를 추정하지 않습니다.

로컬 자동 검증: macOS에서 Python/Qt 41개 검사와 앱 self-test, C sanitizer 테스트 6종, 실제 SDK RPA identity 회귀 검사가 통과했습니다. ESP-IDF 5.5.5 ESP32 빌드에서 본딩 4개·CCCD 16개가 반영된 것을 확인했습니다. macOS ARM64 앱 패키지를 만들고 압축을 해제한 앱의 self-test, Mach-O 아키텍처, HIDAPI 로드와 Qt 화면 렌더링도 확인했습니다. HID Report Map은 변경 전 커밋과 동일합니다. 새 C 연결 테스트는 실제 연결 모듈을 별도 컴파일하고, GAP 콜백과 가짜 시계로 암호화 완료/타임아웃 경합, 오래된 세대 이벤트, 부분 구독, 종료 실패 재시도, 재페어링 오류를 검사합니다.

네 플랫폼 CI 빌드 구성을 유지합니다. 이 로컬 기록은 Windows 또는 Intel Mac 네이티브 빌드 결과가 아닙니다. **이번 변경을 설치한 ESP32의 macOS·Windows 실기 시험은 모두 미실행**입니다. 현재 환경에 ESP32 USB 시리얼 포트와 Windows 실기 환경이 없으며, 과거 Mac 연결 성공 기록을 새 펌웨어의 검증 결과로 승계하지 않습니다.

### 실기 결과표

각 실행에서 OS/CPU·Bluetooth 어댑터·앱 버전·ESP32 부팅 버전·앱 실행 여부·복구 시간·실패 단계·Serial 로그 위치를 기록합니다. 복구 시간은 OS가 연결 가능한 상태부터 실제 키·마우스 입력이 동작할 때까지 측정합니다.

| 시험 | macOS | Windows | 합격 기준 |
|---|---|---|---|
| 최초 페어링 | 미실행 / 5회 | 미실행 / 5회 | 등록·설정 읽기·실제 입력 성공 |
| ESP32 재시작 | 미실행 / 20회 | 미실행 / 20회 | 유효한 기존 본드로 30초 이내 입력 복구 |
| 절전 복귀 | 미실행 / 10회 | 미실행 / 10회 | 30초 이내 입력 복구 |
| Bluetooth 끄기/켜기 | 미실행 / 10회 | 미실행 / 10회 | 30초 이내 입력 복구 |
| Mac → Windows / Windows → Mac | 각 방향 미실행 / 10회 | 각 방향 미실행 / 10회 | 이전 PC 연결 해제 후 30초 이내 입력 복구 |
| PC에서만 등록 삭제 후 재등록 | 미실행 | 미실행 | 다른 PC·리모컨 본딩과 매핑 유지 |
| 앱 종료 상태의 연속 입력 | 미실행 / 30분 | 미실행 / 30분 | assertion·재부팅·키 눌림 고착 없음 |
| 앱 실행 상태의 연속 입력 | 미실행 / 30분 | 미실행 / 30분 | assertion·재부팅·키 눌림 고착 없음 |

ARM64와 x86_64를 검증했다면 각각 별도로 기록합니다. 실패는 평균 복구 시간으로 숨기지 않고 횟수와 해당 단계 로그를 남깁니다. 자동 테스트의 성공으로 이 표를 통과 처리하지 않습니다.
