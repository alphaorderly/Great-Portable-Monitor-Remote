[한국어](../README.md) | [English](../README.en.md)

# ESP32-S3 펌웨어 / Firmware

ESP-IDF **5.5.5**, **ESP32-S3 N16R8**(16MB flash, 8MB PSRAM) 전용 단일 프로젝트입니다. PSRAM은 사용하지 않습니다. 소스는 `firmware/main/`, 설정은 `firmware/sdkconfig.defaults`, 빌드 결과는 `firmware/build/`에 있습니다.

```text
리모컨 ── BLE ── ESP32-S3 ── USB HID ── macOS / Windows
```

## 포트와 설치 / Ports and flashing

- **UART/COM**: 펌웨어 설치 및 115200 baud 로그.
- **USB/OTG**: PC 입력 및 설정 앱. `ESP32-S3 Remote Bridge · USB`를 선택하세요.

네이티브 USB(GPIO19=D−, GPIO20=D+)를 사용합니다. USB-UART만 연결하면 HID가 동작하지 않습니다. 포트 인쇄를 확인하세요. 정상 사용에는 USB/OTG만 연결할 수 있고 로그가 필요하면 UART/COM도 연결합니다. PC Bluetooth 페어링은 필요하지 않습니다.

macOS에서는 `./flash-macos.sh`, Windows ESP-IDF 환경의 Git Bash에서는 `bash flash-windows.sh`를 실행하세요. 포트 → 전체 설치/앱만 갱신 → 모니터 순서로 선택합니다. 칩을 두 번 확인하며 ESP32-S3가 아니면 설치하지 않습니다. 앱만 갱신은 기존 파티션 레이아웃이 같을 때만 사용하세요.

## 소스 빌드 / Build

ESP-IDF 5.5.5 환경과 ESP32-S3 툴체인을 준비한 뒤 저장소 루트에서 실행합니다. 로컬 macOS SDK는 `source firmware/env.zsh`로 활성화할 수 있습니다.

```sh
python firmware/scripts/build.py build
python firmware/scripts/build.py -p /dev/cu.usbserial-0001 flash
python scripts/package_firmware.py
```

Windows에서는 포트를 `COM3` 같은 이름으로 바꾸세요. 빌드 대상은 `esp32s3`로 고정되며 `--target` 선택 옵션은 없습니다. `-B build`는 `firmware/build/`를 뜻합니다. 다른 칩 또는 이전 프로젝트의 캐시를 발견하면 중단합니다. 이전 ESP32의 `sdkconfig`와 빌드 캐시는 재사용하지 마세요. 필요한 로그를 보관한 후 해당 캐시를 이동하고 새로 빌드하세요.

빌드 래퍼는 NimBLE RPA identity 패치를 멱등 적용합니다. USB 의존성은 `esp_tinyusb==2.0.0`과 `firmware/dependencies.lock`으로 고정합니다. 아카이브는 생성된 flash metadata의 주소와 파일을 그대로 사용하며 `release/RemoteKeyMapper-firmware-esp32s3.zip`에 저장합니다.

This is a single ESP32-S3 project. Build with `python firmware/scripts/build.py build`; package with `python scripts/package_firmware.py`. Use UART/COM to flash and USB/OTG for PC HID. Foreign build caches and non-S3 chips are rejected.

## 리모컨과 호스트 연결

부팅 시 리모컨을 검색합니다. 최초 연결은 `main.c`의 `TARGET_NAME`과 HID Service UUID가 일치하는 광고를 사용합니다. 리모컨을 제조사 페어링 모드로 설정하고, 컴퓨터가 리모컨을 직접 연결하고 있으면 그 연결을 해제하세요. 이후에는 저장된 본드 identity로 재연결합니다. 모델별 페어링 버튼 조합은 확정하지 않았습니다.

PC는 USB로 연결합니다. 컴퓨터를 바꾸려면 USB/OTG 케이블을 새 PC에 연결하세요. 앱의 **연결 해제**는 설정 앱의 HID 접근과 재시도만 종료하며 키보드·마우스 입력은 계속 동작합니다.

리모컨 본딩과 매핑은 NVS에 저장합니다. 저장 실패 시 전체 NVS를 초기화하지 않습니다. 본드 저장 공간이 부족하면 등록된 리모컨·현재 연결 기기·저장 대상을 보호하며 오래된 비활성 상대 기록을 교체합니다.

## USB HID와 검증

Vendor collection FF00/1에서 입력 ID 1과 설정 Feature ID 5를 제공합니다. 키보드·마우스·볼륨은 ID 2·3·4입니다. 설정 payload는 64바이트(Report ID 포함 65바이트)이며, NVS 저장 후 읽어서 revision과 내용을 확인합니다. USB와 BLE 사이의 설정 변경은 NimBLE 작업에서 직렬 처리합니다. 앱은 저장을 자동 반복하지 않습니다.

macOS·Windows에서 USB 열거, 리모컨 최초 페어링·재연결, 실제 입력, 매핑 저장·재부팅 유지, USB 재연결·절전 복귀를 확인해야 합니다. **ESP32-S3 실기 동작은 아직 미검증입니다.** 자동 테스트는 USB/FreeRTOS를 모의하며 실제 PHY나 OS 드라이버 동작을 증명하지 않습니다.

## 플래시 없이 시리얼 로그만 보기

저장소 루트에서 다음 스크립트를 실행하고 포트를 선택하세요. 기본 속도는 115200 baud이며 **Ctrl+C**로 종료합니다. SDK 빌드·패치·플래시를 실행하지 않고, 키보드 입력을 ESP32-S3로 전송하지 않습니다.

macOS:

```sh
./monitor-macos.sh
# 포트를 직접 지정하고 로그를 파일에 추가 저장
./monitor-macos.sh --port /dev/cu.usbserial-0001 --log esp32s3-monitor.log
```

Windows PowerShell 또는 CMD:

```powershell
.\monitor-windows.cmd
.\monitor-windows.cmd --port COM3 --log esp32s3-monitor.log
```

`--baud`로 속도를 바꿀 수 있고 `--help`로 옵션을 확인할 수 있습니다. 로그 파일은 기존 내용을 덮어쓰지 않고 수신 원본 바이트를 추가합니다. 파일 저장을 원하지 않으면 `--log`를 생략하세요.

Python 3와 `pyserial`이 필요합니다. macOS 스크립트는 프로젝트의 기존 ESP-IDF Python을 자동으로 사용하며, Windows에서는 활성화한 ESP-IDF Python 또는 PATH의 Python을 사용합니다. 별도 Python을 쓴다면 해당 Python으로 `python -m pip install pyserial==3.5`를 한 번 실행하세요. 스크립트는 의존성을 자동 설치하지 않습니다. 이미 다른 시리얼 모니터가 포트를 사용 중이면 먼저 닫으세요.

리셋 명령을 보내지 않고 포트를 열기 전에 DTR/RTS를 비활성화합니다. 다만 USB 드라이버가 포트를 여는 순간 제어선을 변경할 수 있어, 모든 보드에서 물리적인 리셋 방지를 보장할 수는 없습니다. 스크립트를 열기 전의 부팅 로그는 소급해서 읽을 수 없습니다.

## 문자열 매크로와 USB 제품명

새 USB 제품명은 `USB Keyboard & Mouse`이며 제조사·고유 일련번호와 전용 설정용 HID를 유지합니다. Feature ID 5는 기존 키 설정, 새 ID 6은 버튼별 매크로 전송입니다. 매크로는 NVS `remote_macro`에 독립 저장됩니다. 문자열은 미국식 QWERTY의 ASCII와 Enter·Tab을 지원합니다. 업로드 중인 데이터를 실행하지 않고, COMMIT 시 전체 검증·revision 확인·NVS 저장을 완료한 뒤 활성화합니다.

실행기는 NimBLE 소유 태스크의 타이머에서 동작하며 대기 중에도 취소·연결 이벤트를 처리합니다. 키보드 전송은 기존 USB 큐와 진행 중 전송이 비었을 때만 진행해 과거 입력을 몰아서 재생하지 않습니다. USB 전송 실패, 연결 변경, 절전, 리모컨 연결 종료와 모드 변경은 매크로 및 임시 업로드를 취소합니다. 중지 시 키를 해제하고 재연결 후 자동 재개하지 않습니다. 실제 USB 열거·입력 간격·대상 프로그램 호환성은 기기로 확인해야 합니다. 상세 사용법과 제한은 [파이썬 앱 문서](../python/README.md#문자열-매크로)를 참고하세요.
