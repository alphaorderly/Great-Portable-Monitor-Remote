# 펌웨어 C 테스트

ESP32-S3 없이 컴퓨터에서 실행하는 C 테스트 6종과 NimBLE 주소 처리 테스트입니다. macOS·Linux·WSL에서 C 컴파일러(`cc`), Python, 프로젝트 패치가 적용된 ESP-IDF SDK가 필요합니다. 저장소 루트에서 실행하세요.

```sh
python firmware/tests/native/run_tests.py --idf-path /path/to/esp-idf
```

`/path/to/esp-idf`를 SDK 설치 경로로 바꾸세요. `--idf-path`를 생략하면 `IDF_PATH` 환경 변수, 저장소의 `.tools/esp-idf` 순으로 경로를 선택합니다.

스크립트는 AddressSanitizer/UndefinedBehaviorSanitizer와 `-Wall -Wextra -Werror` 옵션으로 컴파일합니다. 테스트에 쓰는 임시 바이너리는 종료 시 삭제합니다.

- `hid_client_test`: 보안 게이트, 실제 Report Map 읽기, Report 선택/CCCD, 키·마우스 수신, 연결 종료 후 두 번째 GATT 탐색.
- `input_codec_test`: 기본 키 매핑, 해제, 마우스 부호와 패딩 검증.
- `reconnect_test`: 늦은 기동, 본딩된 이름 없는 광고, 스캔/연결 실패 및 연결 종료/resync 후 재시도.
- `bond_store_test`: FULL 사전 알림, 실제 OVERFLOW 후 보호 대상을 제외한 순환 교체, 보안 기록·CCCD별 용량, 주소 정규화, 조회·삭제·재시도 실패와 기기별 최종 저장 결과를 검사합니다.
- `keymap_test`: 사용자 매핑의 키 변환, 잘못된 설정과 revision 충돌 거부, 저장 실패 시 유지, 재초기화 후 설정 복원.

C의 기본 키 매핑을 `shared/fixtures/keymap-default.hex`와 대조합니다. Python 테스트도 같은 파일을 사용합니다.

- `test_nimble_identity.py`: 설치된 SDK의 `ble_hs_resolv_rpa_addr()` 함수를 추출해 RPA 캐시 갱신 후에도 저장된 PUBLIC/RANDOM identity 타입이 유지되는지 검사합니다. 패치하지 않은 SDK에서는 PUBLIC 타입 보존 검사가 실패합니다.

저장 장치와 NimBLE/FreeRTOS 호출, IRK 매칭은 모의 구현을 사용하므로 무선 동작이나 AES 암호화는 검사하지 않습니다. ABI 호환성은 ESP-IDF 빌드로, USB Feature 통신·전원 재시작 후 설정 보존·컨트롤러 ROM assertion 발생 여부는 실제 하드웨어에서 확인해야 합니다.

- `usb_hid_test`: USB 어댑터와 입력 엔진의 실제 C 코드를 모의 USB/작업 큐로 실행합니다. descriptor, 65바이트 Feature 교환의 payload, 저장·readback·revision·NVS 실패·요청 타임아웃, 큐 포화·버튼 해제·재연결·절전·리셋 복구를 검사합니다. 실제 USB 열거와 전기적 연결은 검사하지 않습니다.

- `macro_test`: 모든 출력 가능한 ASCII·Shift·Enter·Tab, 단계 검증, revision 충돌·NVS 실패·부분 업로드 취소·만료, 고정/랜덤 간격의 양 끝값, 반복 횟수, 전송 정체, 중지와 시간 카운터 wraparound를 검사합니다.
- `usb_hid_test`의 매크로 항목: Feature ID 6 저장·readback, 저장 직후 첫 버튼 입력, 빠른 재입력과 실행 중 재입력의 취소, USB 실패와 재연결 후 자동 재실행 방지를 검사합니다.
- `python/tests/test_macros.py`: C 컴파일러가 있는 macOS/Linux에서는 실제 `macro.c`를 공유 라이브러리로 빌드해 Python의 청크 전송·저장·전체 재읽기를 교차 검증합니다. 저장 직후 응답 유실과 중간 취소에서 자동 쓰기 재시도가 없는지도 확인합니다.
