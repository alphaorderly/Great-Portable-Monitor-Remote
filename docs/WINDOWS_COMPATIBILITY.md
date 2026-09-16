[한국어](../README.md) | [English](../README.en.md)

# ESP32-S3 USB HID 호환성

ESP32-S3는 리모컨과 BLE로 연결하고 PC에는 USB HID로 입력을 전달합니다. PC Bluetooth 페어링은 사용하지 않습니다. **Windows·macOS USB 실기 호환성은 아직 검증하지 않았습니다.** 과거 ESP32 Bluetooth 시험 결과를 이 보드의 검증 결과로 간주하지 않습니다.

앱은 `ESP32-S3 Remote Bridge`의 Vendor FF00/1 collection만 열고, 키보드·마우스 collection은 열지 않습니다. 설정 Feature ID 5는 Report ID 포함 65바이트이며 TinyUSB HID 버퍼는 128바이트입니다. 저장 후 readback으로 내용과 revision을 확인합니다.

## 자동 검증

- Python/Qt: 장치 검색, 설정 프로토콜, 저장 확인, 재연결, 편집 내용 유지, OS별 키 기본값.
- 네이티브 C: 리모컨 보안·검색·재연결, 입력 매핑, 본드 저장, USB 큐·설정·절전·리셋 복구.
- CI: macOS/Windows 각각 ARM64와 x86_64 앱, ESP-IDF 5.5.5 ESP32-S3 빌드.

## 하드웨어 확인 절차

각 OS/CPU와 앱·펌웨어 버전, USB 포트, 결과와 UART 로그를 기록하세요.

1. USB/OTG 연결 후 `ESP32-S3 Remote Bridge`가 열거되는지 확인합니다.
2. 리모컨을 연결하고 앱 종료 상태에서 키보드·마우스·볼륨을 확인합니다.
3. 앱에서 설정을 읽고 두 모드의 매핑·속도·홈 일시 정지를 저장하여 readback을 확인합니다.
4. Ctrl, Alt, Win/Command 조합과 키 해제를 확인합니다. Windows 기본값은 Alt+Tab입니다.
5. 보드 재부팅 후 매핑 보존과 리모컨 재연결을 확인합니다.
6. USB 분리·재연결, PC 절전·복귀, Mac↔Windows 이동을 확인합니다.
7. 편집 중 USB가 끊겨도 편집 내용이 남고 저장 명령이 자동 반복되지 않는지 확인합니다.

검색 문제가 있으면 앱의 **검색 진단 저장** 또는 `RemoteKeyMapper-windows-x86_64.exe --diagnostics hid-diagnostics.json`을 사용하세요. ARM64는 파일명의 `x86_64`를 `arm64`로 바꾸세요. 열거 원본·제외 이유와 오류를 기록하며 장치 설정을 변경하지 않습니다.
