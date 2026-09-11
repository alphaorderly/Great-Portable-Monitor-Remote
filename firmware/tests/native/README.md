# 펌웨어 호스트 모의 테스트

`firmware/` 폴더에서 C 테스트 5종과 SDK 주소 해석 회귀 테스트를 실행한다.

```zsh
zsh tests/native/run_tests.zsh
```

스크립트는 임시 디렉터리에서 AddressSanitizer/UndefinedBehaviorSanitizer 및 `-Wall -Wextra -Werror`로 컴파일한다. 종료 시 임시 바이너리를 지운다.

- `hid_client_test`: 보안 게이트, 실제 Report Map 읽기, Report 선택/CCCD, 키·마우스 수신, 연결 종료 후 두 번째 GATT 탐색.
- `mac_hid_test`: 실제 Report Map 비트 길이, Input/Feature Reference, 암호화/구독 게이트, 이동 재생 방지, 광고 전환, 설정 읽기·쓰기와 기존 눌림 해제. Mac의 오래된 본드 교체, 리모컨/정상 본드 보호, 보안 하향 거부, 저장 실패 및 암호화 실패 후 재광고도 검사한다.
- `input_codec_test`: 기본 키 매핑, 해제, 마우스 부호와 패딩 검증.
- `reconnect_test`: 늦은 기동, 본딩된 이름 없는 광고, 스캔/연결 실패 및 연결 종료/resync 후 재시도.
- `keymap_test`: 사용자 매핑의 키 변환, 잘못된 설정과 revision 충돌 거부, 저장 실패 시 유지, 재초기화 후 설정 복원.

C 기본값을 `../shared/fixtures/keymap-default.hex`와 대조하며 Python 테스트도 같은 fixture를 검사한다. 저장소와 NimBLE/FreeRTOS 무선 동작은 호스트 대역이다. 실제 ABI는 ESP-IDF 빌드로 확인하고, BLE Feature 통신/플래시 보존/컨트롤러 ROM assertion은 하드웨어 시험으로 확인해야 한다.

- `test_nimble_identity.py`: 설치된 SDK의 실제 `ble_hs_resolv_rpa_addr()` 함수를 추출해 RPA 캐시를 갱신해도 저장 PUBLIC/RANDOM identity 타입이 바뀌지 않는지 검사한다. 원본 SDK에서는 PUBLIC 보존 검사가 실패한다. IRK 매칭은 대역이며 AES 검증은 아니다.
