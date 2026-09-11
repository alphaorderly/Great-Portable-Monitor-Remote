"""Version 2 of the ESP32 keymap Feature Report (ID 5)."""
from dataclasses import dataclass, replace
from protocol import BUTTONS

REPORT_ID = 5
PAYLOAD_LENGTH = 64
NONE, KEY, VOLUME, MOUSE = 0, 1, 2, 3


@dataclass(frozen=True)
class Entry:
    button: int
    kind: int
    key: int
    modifiers: int = 0


DEFAULTS = tuple(
    Entry(button, kind, key, modifiers)
    for button, kind, key, modifiers in (
        (0x66, KEY, 0x6B, 0), (0x69, KEY, 0x6C, 0), (0x6A, NONE, 0, 0), (0x6B, KEY, 0x6E, 0),
        (0x52, KEY, 0x52, 0), (0x51, KEY, 0x51, 0), (0x50, KEY, 0x50, 0), (0x4F, KEY, 0x4F, 0),
        (0x28, KEY, 0x28, 0), (0xF1, KEY, 0x29, 0), (0x4A, KEY, 0x4A, 0), (0x76, KEY, 0x2B, 8),
        (0x80, VOLUME, 1, 0), (0x81, VOLUME, 2, 0),
    )
)


MOUSE_DEFAULTS = tuple(Entry(e.button, MOUSE, 2 if e.button == 0x50 else 1)
                       if e.button in (0x50, 0x28) else e for e in DEFAULTS)


def defaults_for_host(host):
    """Return explicit app-switch presets, leaving the firmware fixture intact."""
    if host not in ("macos", "windows", "linux"):
        raise ValueError(f"지원하지 않는 호스트: {host}")
    modifier = 8 if host == "macos" else 4  # Command+Tab / Alt+Tab
    return tuple(tuple(replace(e, modifiers=modifier) if e.button == 0x76 else e
                       for e in entries) for entries in (DEFAULTS, MOUSE_DEFAULTS))


@dataclass(frozen=True)
class Snapshot:
    revision: int
    entries: tuple[Entry, ...]
    mouse_entries: tuple[Entry, ...] = MOUSE_DEFAULTS

    def encode(self, with_id=True):
        if not 0 <= self.revision <= 0xFFFF:
            raise ValueError("설정 버전이 올바르지 않습니다.")
        data = bytearray(b"KM\x02\x0e" + self.revision.to_bytes(2, "little") + b"\x02\0")
        for entries in (self.entries, self.mouse_entries):
            if len(entries) != len(BUTTONS):
                raise ValueError("버튼 수가 올바르지 않습니다.")
            for button, entry in zip(BUTTONS, entries):
                if entry.button != button:
                    raise ValueError("버튼 순서/코드가 올바르지 않습니다.")
                valid = (
                    (entry.kind == NONE and entry.key == 0 and entry.modifiers == 0)
                    or (entry.kind == KEY and 4 <= entry.key <= 0x73 and 0 <= entry.modifiers <= 15)
                    or (entry.kind == VOLUME and entry.key in (1, 2) and entry.modifiers == 0)
                    or (entry.kind == MOUSE and entry.key in (1, 2, 4) and entry.modifiers == 0)
                )
                if not valid or (button == 0x6A and entry.kind != NONE):
                    raise ValueError(f"{BUTTONS[button]}의 키 매핑이 지원 범위를 벗어났습니다.")
                value = entry.key | (entry.kind << 7) | (entry.modifiers << 9)
                data.extend(value.to_bytes(2, "little"))
        return (bytes([REPORT_ID]) if with_id else b"") + bytes(data)

    @classmethod
    def decode(cls, raw):
        raw = bytes(raw)
        if len(raw) == PAYLOAD_LENGTH + 1:
            if raw[0] != REPORT_ID:
                raise ValueError("키 매핑 Feature Report가 아닙니다.")
            raw = raw[1:]
        if len(raw) != PAYLOAD_LENGTH or raw[:4] != b"KM\x02\x0e" or raw[6:8] != b"\x02\0":
            raise ValueError("모드별 키 매핑을 지원하는 새 ESP32 펌웨어가 필요합니다.")
        modes = []
        for mode in range(2):
            entries = []
            for i, button in enumerate(BUTTONS):
                offset = 8 + 2 * (mode * len(BUTTONS) + i)
                value = int.from_bytes(raw[offset:offset+2], "little")
                if value & 0xE000:
                    raise ValueError("키 매핑 예약 비트가 올바르지 않습니다.")
                entries.append(Entry(button, (value >> 7) & 3, value & 127, (value >> 9) & 15))
            modes.append(tuple(entries))
        snapshot = cls(int.from_bytes(raw[4:6], "little"), *modes)
        snapshot.encode()
        return snapshot


def read_settings(device):
    return Snapshot.decode(device.get_feature_report(REPORT_ID, PAYLOAD_LENGTH + 1))


def save_settings(device, desired):
    current = read_settings(device)
    if current.revision != desired.revision:
        raise ValueError("장치 설정이 다른 곳에서 변경됐습니다. 장치에서 다시 읽은 뒤 수정하세요.")
    packet = desired.encode()
    if device.send_feature_report(packet) != len(packet):
        raise OSError("설정 전송 결과를 확인하지 못했습니다. 장치에서 다시 읽어 확인하세요.")
    actual = read_settings(device)
    expected = replace(desired, revision=(desired.revision + 1) & 0xFFFF)
    if actual != expected:
        raise OSError("저장 후 읽은 설정이 다릅니다. 저장 성공으로 처리하지 않았습니다.")
    return actual


# Values are HID usages, not OS-specific keycodes. Four modifier checkboxes
# express shortcuts without requiring global keyboard monitoring permissions.
ACTIONS = [("동작 없음", NONE, 0)]
ACTIONS += [(name, KEY, code) for name, code in (
    ("Enter", 0x28), ("Esc", 0x29), ("Tab", 0x2B), ("Space", 0x2C), ("Backspace", 0x2A),
    ("↑ 위", 0x52), ("↓ 아래", 0x51), ("← 왼쪽", 0x50), ("→ 오른쪽", 0x4F),
    ("Home", 0x4A), ("End", 0x4D), ("Page Up", 0x4B), ("Page Down", 0x4E), ("Delete", 0x4C),
)]
ACTIONS += [(chr(65+i), KEY, 4+i) for i in range(26)]
ACTIONS += [(str(i), KEY, 0x1D+i) for i in range(1, 10)] + [("0", KEY, 0x27)]
ACTIONS += [(f"F{i}", KEY, 0x39+i if i <= 12 else 0x5B+i) for i in range(1, 25)]
ACTIONS += [(name, KEY, code) for name, code in (
    ("-", 0x2D), ("=", 0x2E), ("[", 0x2F), ("]", 0x30), ("\\", 0x31),
    (";", 0x33), ("'", 0x34), ("`", 0x35), (",", 0x36), (".", 0x37), ("/", 0x38),
)]
ACTIONS += [("볼륨 높이기", VOLUME, 1), ("볼륨 낮추기", VOLUME, 2)]

ACTIONS += [("마우스 왼쪽 클릭", MOUSE, 1), ("마우스 오른쪽 클릭", MOUSE, 2), ("마우스 가운데 클릭", MOUSE, 4)]
