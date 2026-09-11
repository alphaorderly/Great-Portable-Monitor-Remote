[한국어](../README.md) | [English](../README.en.md)

# ESP32 펌웨어 / Firmware

원래 ESP32 칩용 ESP-IDF **5.5.5** 프로젝트입니다. 호스트가 macOS/Windows, ARM64/x86_64 어느 조합이든 ESP32에 설치하는 펌웨어는 같습니다. ESP32-S2/S3/C3용으로 플래시하지 마세요.

This ESP-IDF 5.5.5 project targets the original ESP32. The same firmware serves every host OS/CPU. It is not a firmware image for S2/S3/C3 chips.

## 릴리스 BIN 설치 / Flash release binaries

릴리스의 `firmware-esp32` ZIP을 풀고, Python과 `esptool==4.12.0`을 설치한 터미널에서 그 폴더로 이동합니다.

```sh
python -m pip install esptool==4.12.0
# 최초 설치: bootloader, partition table, application
python -m esptool --chip esp32 --port PORT write_flash @flash_args
# 동일한 partition table을 사용하는 기존 장치의 앱만 갱신
python -m esptool --chip esp32 --port PORT write_flash 0x10000 remote_ble_scan.bin
```

`PORT`는 Windows의 `COM3` 또는 macOS의 `/dev/cu.usbserial-...` 등 실제 연결 포트로 바꾸세요. PowerShell에서는 `@flash_args`를 문자열로 인식하도록 `"@flash_args"`로 입력합니다. 기존 동일 레이아웃에서 앱만 갱신하면 NVS의 페어링·매핑을 유지합니다. 이 명령은 `erase_flash`를 실행하지 않습니다.

Extract the firmware ZIP, enter that directory, install esptool 4.12.0 and replace `PORT` with your serial port. Quote `"@flash_args"` in PowerShell. The app-only command preserves NVS when updating an existing installation with the same partition layout.

## 소스 빌드 / Build from source

[Espressif 설치 문서](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32/get-started/index.html)를 따라 ESP-IDF 5.5.5의 ESP32 도구를 설치하고 환경을 활성화합니다. Windows에서는 ESP-IDF 터미널 또는 `export.ps1`, macOS/Linux에서는 `export.sh`를 사용합니다.

저장소 루트에서 / From the repository root:

```sh
python firmware/scripts/build.py -B build build
python firmware/scripts/build.py -B build -p PORT app-flash monitor
```

`build.py`는 OS 공통 진입점입니다. 활성화한 `IDF_PATH`의 NimBLE RPA identity 수정 패치를 멱등 적용한 뒤 `idf.py`를 실행합니다. SDK의 예상 코드가 다르면 빌드를 중단합니다. 다른 버전의 SDK에 패치를 강제로 적용하지 마세요. 기존 로컬 macOS 설치는 `firmware/env.zsh`도 계속 사용할 수 있습니다.

The wrapper applies the included NimBLE identity fix to the activated SDK, then runs `idf.py`. It stops if the expected SDK source does not match. Use the pinned version. Windows ARM64 GUI support does not imply native Windows ARM64 support for Espressif's compiler; use Espressif's supported build environment or download the CI firmware.

## 리모컨과 호스트 연결

ESP32는 부팅 시 호스트용 `ESP32 Remote Bridge`를 광고하면서 리모컨을 검색합니다. 최초 연결은 펌웨어 `main.c`의 `TARGET_NAME`과 HID Service UUID가 일치하는 리모컨 광고를 사용합니다. 리모컨을 제조사의 Bluetooth 페어링 모드로 설정하고, 기존 컴퓨터가 리모컨을 직접 잡고 있으면 그 연결을 해제하세요. 저장된 리모컨은 본드 identity로 재연결합니다. 모델별 페어링 버튼 조합은 이 프로젝트에서 확정하지 않았습니다.

컴퓨터 Bluetooth 설정에서 **ESP32 Remote Bridge**를 페어링하세요. 브리지는 한 번에 한 호스트에 입력을 전달합니다. 다른 컴퓨터로 바꿀 때 기존 호스트 연결을 해제하세요. 저장된 리모컨 identity가 없는 아주 오래된 펌웨어에서 이전하면 리모컨을 다시 페어링 모드로 설정해야 할 수 있습니다.

At boot, the ESP32 advertises `ESP32 Remote Bridge` to the host and scans for the remote. First discovery matches `TARGET_NAME` and the HID service; subsequent connections use the stored bond identity. Put the remote in its manufacturer's pairing mode and disconnect any computer directly holding it. Pair the bridge in your computer's Bluetooth settings. Only one host receives input at a time.

## 연결 복구와 PC 전환

브리지는 Mac과 Windows의 페어링을 저장하되 한 번에 한 PC에 연결합니다. PC를 바꿀 때는 기존 PC의 Bluetooth 연결을 먼저 해제하세요. 앱의 **연결 해제**는 설정 앱의 HID 접근만 닫으며 OS Bluetooth 연결을 해제하지 않습니다. 기존 PC가 자동으로 다시 연결되면 그 PC의 Bluetooth를 잠시 끄고 새 PC에서 연결하세요.

- 부팅 시 마지막으로 연결했던 PC를 대상으로 3초간 광고한 뒤 일반 광고를 유지합니다. 연결 종료가 확인되면 1초 후 일반 광고를 재개합니다.
- PC가 연결된 뒤 15초 안에 암호화·본딩이 확인되지 않으면 기존 키를 보존하고 연결을 종료합니다. 종료 요청 실패는 1초 간격으로 최대 3회 재시도하며, 전체 스택이나 NVS를 자동 초기화하지 않습니다.
- 암호화 후 HID 구독이 늦거나 일부만 있어도 연결을 끊지 않습니다. 키보드·마우스·볼륨은 각각 구독된 입력을 계속 전달합니다. 설정 앱의 구독은 키 입력의 필수 조건이 아닙니다.
- 페어링 오류가 반복되면 **문제가 발생한 PC의 Bluetooth 설정에서 ESP32 Remote Bridge만 제거하고 다시 페어링**하세요. ESP32는 현재 PC의 재페어링 요청에서 보안 수준을 검사한 후 해당 PC 기록만 교체합니다. 다른 PC·리모컨·키 매핑을 초기화하지 않습니다. 재페어링이 되지 않아도 `erase_flash`를 복구 기본 절차로 사용하지 마세요.

기본 저장 한도는 본딩 4개, CCCD 16개입니다. CCCD는 PC별 입력 알림 구독을 저장하는 항목이며, 동시에 연결할 수 있는 PC 수를 뜻하지 않습니다. 저장소가 가득 차면 기존 기록을 자동으로 지우지 않고 오류를 기록합니다. 기존 `sdkconfig`는 기본값보다 우선하므로 소스 업데이트 후 `CONFIG_BT_NIMBLE_MAX_BONDS=4`, `CONFIG_BT_NIMBLE_MAX_CCCDS=16`인지 확인하세요.

Serial 진단에는 부팅 시 빌드 버전과 `host-recovery-1`, `HOST_LINK`의 단계·세대·경과 시간·상태 코드·종료 재시도, 입력별 구독 상태가 남습니다. `Repeated host security failure`는 같은 peer에서 보안 실패가 세 번 연속 발생했음을 뜻합니다. 이 횟수는 부팅 동안만 유지됩니다. `subscription storage failed`와 보안 키 저장 실패를 구분해 확인하세요. 암호 키 값은 출력하지 않습니다.

Switch hosts by disconnecting the previous PC in OS Bluetooth settings; closing the mapper's HID session does not disconnect Bluetooth. Boot advertising targets the last host for 3 seconds, then remains open. Security has a 15-second deadline; partial HID subscriptions do not cause disconnect loops. To repair pairing, remove only the bridge on the affected PC and pair again. Other bonds and mappings are retained. This recovery implementation still requires the hardware tests below.
