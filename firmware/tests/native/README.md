# 펌웨어 C 테스트

ESP32 없이 컴퓨터에서 실행하는 C 테스트 6종과 NimBLE 주소 처리 테스트입니다. macOS·Linux·WSL에서 C 컴파일러(`cc`), Python, 프로젝트 패치가 적용된 ESP-IDF SDK가 필요합니다. 저장소 루트에서 실행하세요.

```sh
python firmware/tests/native/run_tests.py --idf-path /path/to/esp-idf
```

`/path/to/esp-idf`를 SDK 설치 경로로 바꾸세요. `--idf-path`를 생략하면 `IDF_PATH` 환경 변수, 저장소의 `.tools/esp-idf` 순으로 경로를 선택합니다.

스크립트는 AddressSanitizer/UndefinedBehaviorSanitizer와 `-Wall -Wextra -Werror` 옵션으로 컴파일합니다. 테스트에 쓰는 임시 바이너리는 종료 시 삭제합니다.

- `hid_client_test`: 보안 게이트, 실제 Report Map 읽기, Report 선택/CCCD, 키·마우스 수신, 연결 종료 후 두 번째 GATT 탐색.
- `host_link_test`: 별도 컴파일한 연결 모듈을 GAP 콜백과 가짜 시계로 검사합니다. 15초 보안 제한, 완료/타임아웃 경합, 세대가 지난 구독·종료 이벤트, 부분 구독, 종료 재시도 상한, 반복 실패 안내, 재페어링 저장 오류를 다룹니다.
- `mac_hid_test`: Report Map 비트 길이, Input/Feature Reference, 암호화·구독 조건, 이전 이동 입력의 재전송 방지, 광고 전환, 설정 읽기·쓰기와 눌린 키 해제. 오래된 본드 교체, 리모컨·정상 본드 보호, 보안 수준 하향 거부, 저장 실패 및 암호화 실패 후 광고 재개도 검사합니다.
- `input_codec_test`: 기본 키 매핑, 해제, 마우스 부호와 패딩 검증.
- `reconnect_test`: 늦은 기동, 본딩된 이름 없는 광고, 스캔/연결 실패 및 연결 종료/resync 후 재시도.
- `keymap_test`: 사용자 매핑의 키 변환, 잘못된 설정과 revision 충돌 거부, 저장 실패 시 유지, 재초기화 후 설정 복원.

C의 기본 키 매핑을 `shared/fixtures/keymap-default.hex`와 대조합니다. Python 테스트도 같은 파일을 사용합니다.

- `test_nimble_identity.py`: 설치된 SDK의 `ble_hs_resolv_rpa_addr()` 함수를 추출해 RPA 캐시 갱신 후에도 저장된 PUBLIC/RANDOM identity 타입이 유지되는지 검사합니다. 패치하지 않은 SDK에서는 PUBLIC 타입 보존 검사가 실패합니다.

저장 장치와 NimBLE/FreeRTOS 호출, IRK 매칭은 모의 구현을 사용하므로 무선 동작이나 AES 암호화는 검사하지 않습니다. ABI 호환성은 ESP-IDF 빌드로, BLE Feature 통신·전원 재시작 후 설정 보존·컨트롤러 ROM assertion 발생 여부는 실제 하드웨어에서 확인해야 합니다.
