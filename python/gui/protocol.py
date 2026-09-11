"""The versioned vendor HID report shared with main/mac_hid.c."""
from dataclasses import dataclass

DEVICE_NAME = "ESP32 Remote Bridge"
USAGE_PAGE = 0xFF00
USAGE = 1
BUTTONS = {
    0x66: "전원", 0x69: "기어", 0x6A: "마우스커서", 0x6B: "설정",
    0x52: "위", 0x51: "아래", 0x50: "왼쪽", 0x4F: "오른쪽",
    0x28: "OK", 0xF1: "뒤로가기", 0x4A: "홈", 0x76: "앱목록",
    0x80: "볼륨상", 0x81: "볼륨하",
}


def button_name(code: int) -> str:
    return BUTTONS.get(code, f"알 수 없는 버튼 0x{code:02X}")


def is_bridge(device: dict) -> bool:
    """Match only this collection, never open a general keyboard or mouse."""
    return (
        device.get("usage_page") == USAGE_PAGE
        and device.get("usage") == USAGE
        and (DEVICE_NAME.casefold() in (device.get("product_string") or "").casefold()
             or device.get("manufacturer_string") == "Local Remote Bridge")
    )


@dataclass(frozen=True)
class Report:
    sequence: int
    remote_ready: bool
    input_event: bool
    keyboard: bytes

    @classmethod
    def decode(cls, raw: bytes) -> "Report":
        # HIDAPI normally includes a nonzero Report ID. Accept a backend that
        # supplies just the 12-byte payload, but never strip an arbitrary byte.
        if len(raw) == 13:
            if raw[0] != 1:
                raise ValueError(f"지원하지 않는 HID Report ID: {raw[0]}")
            raw = raw[1:]
        if len(raw) != 12:
            raise ValueError(f"HID Report 길이 {len(raw)}: 12 또는 13 bytes 필요")
        if raw[0] != 1 or raw[1] & ~3:
            raise ValueError("지원하지 않는 프로토콜 버전 또는 flags")
        return cls(int.from_bytes(raw[2:4], "little"), bool(raw[1] & 1), bool(raw[1] & 2), raw[4:])

    @property
    def keys(self) -> frozenset[int]:
        return frozenset(code for code in self.keyboard[2:] if code)


class ButtonState:
    def __init__(self):
        self.keys: frozenset[int] = frozenset()
        self.sequence: int | None = None
        self.gaps = 0

    def update(self, report: Report) -> list[tuple[str, int]]:
        if self.sequence is not None:
            delta = (report.sequence - self.sequence) & 0xFFFF
            if delta == 0:
                return []
            if delta > 1:
                self.gaps += delta - 1
        self.sequence = report.sequence
        new_keys = report.keys if report.remote_ready else frozenset()
        # A heartbeat is a state snapshot, not a new physical button press.
        action = "누름" if report.input_event else "상태 복구"
        released = "해제" if report.input_event else "상태 해제"
        events = [(released, code) for code in sorted(self.keys - new_keys)]
        events += [(action, code) for code in sorted(new_keys - self.keys)]
        if report.input_event and new_keys == self.keys and new_keys:
            events += [("반복", code) for code in sorted(new_keys)]
        self.keys = new_keys
        return events
