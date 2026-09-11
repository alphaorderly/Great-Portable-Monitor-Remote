[한국어](../README.md) | [English](../README.en.md)

# Remote Key Mapper

ESP32에 연결된 리모컨의 버튼 동작을 설정하는 macOS·Windows 앱입니다. 설치 방법은 [프로젝트 README](../README.md)를 참고하세요.

## 실행과 매핑

```sh
python python/run_keymapper.py
python python/run_keymapper.py --debug
python python/run_keymapper.py --list
python python/run_keymapper.py --self-test
python python/run_keymapper.py --screenshot preview.png
```

Python 3.12에 `python/requirements.txt`의 의존성을 설치한 뒤 저장소 루트에서 실행하세요. macOS용 `run_keymapper.zsh`와 Windows용 `run_keymapper.cmd`로 실행하면 루트의 `.venv-gui`를 사용합니다. `--self-test`와 `--screenshot`은 장치를 연결하지 않고 사용할 수 있습니다.

1. **기기 찾기 → 연결**: ESP32의 현재 매핑을 읽습니다.
2. **일반 모드 / 마우스 커서 모드**: 설정할 모드를 선택합니다. 다른 모드로 이동해도 편집 내용은 유지됩니다.
3. **기본값 대상 → 현재 모드 기본값**: 선택한 OS의 기본값을 현재 편집 모드에 불러옵니다. 앱목록은 macOS Command+Tab, Windows/Linux Alt+Tab입니다. OS 선택만으로 기존 매핑을 바꾸지는 않습니다.
4. **ESP32에 적용·저장**: 두 모드를 함께 저장하고, 장치에서 다시 읽어 저장 결과를 확인합니다.

- **장치에서 읽기**는 편집 내용을 장치의 설정으로 바꿉니다. 연결 전에 수정한 내용은 최초 연결 시 유지합니다.
- 저장 중 오류가 나면 **장치에서 읽기**로 저장 상태를 확인한 뒤 다시 편집하세요.
- 방향키, Enter/Esc/Tab/Space, 영문·숫자·기호, F1~F24, 탐색 키, 볼륨과 마우스 좌/우/가운데 클릭을 지원합니다.
- 조합 키는 Ctrl/Control, Shift, Alt/Option, Win/Command를 선택할 수 있습니다. Command와 Control은 별개의 키이므로 사용하는 OS에 맞게 지정하세요.
- 실제 문자는 호스트 키보드 배열과 입력기에 따라 달라집니다. 클릭과 볼륨에는 조합 키를 붙이지 않습니다.
- 일반 모드 기본값은 방향키→방향키, OK→Enter입니다. 마우스 모드는 왼쪽→우클릭, OK→좌클릭입니다.
- 커서 버튼은 모드 전환 전용으로 키 입력을 보내지 않습니다.

![설정 화면](gui/keymapper-preview.png)

## 모드 전환과 설정 저장

앱에서 모드를 선택하면 해당 모드의 버튼 설정을 편집할 수 있습니다. 사용 중 모드 전환은 리모컨의 센서 신호에 따라 ESP32가 처리합니다.

- ID 91의 센서 START는 마우스 모드를 나타냅니다. 커서 단독 버튼 뒤의 STOP은 일반 모드로 전환합니다.
- 방향키 사용 중 STOP→방향키→START→방향키+6A 순서는 일시 정지로 처리합니다.
- 모드 전환 시 기존 키·클릭을 해제합니다. 재연결 후 모드를 모르면 일반 모드에서 시작합니다.
- v1 설정을 불러오면 두 모드에 복사하며, 마우스 모드의 OK 버튼은 좌클릭으로 유지합니다.
- 새 설정은 `remote_keymap/map_v2`에 저장하고 `map_v1`은 남깁니다. 구형 GUI는 v2 설정을 덮어쓰지 못합니다.

## 입력 디버깅

**입력 디버깅 → 디버깅 모드 · 입력 기록**을 켜거나 `--debug`로 시작하세요. 리모컨의 실제 모드를 맞춘 뒤 **테스트할 모드**를 선택합니다. 이 선택은 기록 분류용이며 ESP32의 실제 모드를 변경하지 않습니다.

- **리모컨 원본**은 설정한 동작으로 변환하기 전의 버튼 입력입니다.
- **호스트 키보드 / 마우스 / 볼륨**에는 앱이 수신한 출력 보고서를 기록합니다. Windows에서는 이 기록이 보이지 않을 수 있으므로, 입력이 동작하는지는 메모장 등의 앱에서 확인하세요.
- 전역 입력을 가로채거나 OS가 독점한 키보드·마우스를 열지 않습니다. 실제 입력은 기록 중에도 계속 동작합니다.
- 기록을 시작할 때 이미 눌려 있던 입력은 상태 복구로 표시합니다. 비교 횟수에는 버튼 누름과 초기 상태만 포함합니다.
- 기록은 최대 300줄, 마우스 이동은 초당 최대 10회입니다. 누락·해석 제외 수를 별도로 표시합니다.

![디버깅 화면](gui/debug-preview.png)

## HID 설정 통신

설정 통신에는 전용 Vendor HID 컬렉션 `FF00 / Usage 1`을 사용합니다. macOS에서는 HIDAPI의 공유 접근을 활성화합니다. 모든 HID I/O는 하나의 작업 스레드에서 처리합니다.

Feature Report ID 5의 payload는 64 bytes입니다. GATT에서는 ID를 제외한 64 bytes, HIDAPI에서는 ID 포함 65 bytes를 사용합니다. 읽기에서는 backend가 ID를 생략한 64 bytes도 허용합니다.

| Payload | 의미 |
|---|---|
| 0–3 | `4B 4D 02 0E`: KM, v2, 버튼 14개 |
| 4–5 | revision, little-endian 16-bit |
| 6–7 | 모드 수 2, 예약 0 |
| 8–35 | 일반 모드 14개 × 16-bit |
| 36–63 | 마우스 모드 14개 × 16-bit |

각 항목은 bit 0–6=키, 7–8=종류, 9–12=조합 키, 13–15=예약 0입니다. 종류는 없음=0, 키보드=1, 볼륨=2, 마우스=3이며, 조합 키는 Control=1, Shift=2, Alt=4, Win/Command=8입니다. ESP32는 연결 암호화 여부, 데이터 길이, 지원 값과 revision을 확인하고, 전체 설정을 NVS에 저장한 뒤 적용합니다.

## 테스트

```sh
python -m unittest discover -s python/tests -v
python python/run_keymapper.py --self-test
```

프로젝트 루트에서 실행합니다. 장치 연결 없이 통신 프로토콜, 저장 결과 확인, 두 모드 편집, 조합 키, Windows 기본값, OS별 API 호출을 검사합니다. 펌웨어 테스트와 Windows 실기 확인 항목은 [Windows 호환성 검토](../docs/WINDOWS_COMPATIBILITY.md)를 참고하세요.
