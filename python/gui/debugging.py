"""Decode only the selected ESP32's reports; never infer the remote's mode."""
from dataclasses import dataclass
from mapping import ACTIONS, KEY
from protocol import ButtonState, Report, button_name
from platform_support import MODIFIER_NAMES

MODES = ("일반 모드", "마우스 커서 모드")
KEY_NAMES = {code: name for name, kind, code in ACTIONS if kind == KEY}
KEY_NAMES.update({0xE0+i: f"{side} {name}" for i, (side, name) in enumerate(
    (side, name) for side in ("왼쪽", "오른쪽") for name in MODIFIER_NAMES)})


@dataclass(frozen=True)
class DebugEvent:
    source: str
    name: str
    action: str
    raw: str


@dataclass(frozen=True)
class DebugBatch:
    mode: str
    events: tuple[DebugEvent, ...]
    packets: int
    gaps: int
    invalid: int
    dropped: int
    remote_ready: bool | None


class DebugDecoder:
    def __init__(self):
        self.buttons = ButtonState()
        self.native = {}
        self.packets = self.invalid = 0
        self.remote_ready = None
        self.last_motion = float("-inf")

    def transitions(self, source, keys, names, raw):
        previous = self.native.get(source)
        self.native[source] = keys
        old = previous or frozenset()
        events = [DebugEvent(source, names(code), "해제", raw) for code in sorted(old-keys)]
        events += [DebugEvent(source, names(code), "누름" if previous is not None else "상태 복구", raw)
                   for code in sorted(keys-old)]
        return events

    def consume(self, packet, now):
        data = bytes(packet)
        if not data:
            return []
        self.packets += 1
        raw = data.hex(" ").upper()
        if len(data) in (12, 13):
            try:
                report = Report.decode(data)
            except ValueError:
                self.invalid += 1
                return []
            # The first frame establishes a baseline, even if an input event
            # happened before debug recording began.
            first = self.buttons.sequence is None
            self.remote_ready = report.remote_ready
            events = [DebugEvent("리모컨 원본", f"{button_name(code)} (0x{code:02X})",
                                 "상태 복구" if first and action == "누름" else action, raw)
                      for action, code in self.buttons.update(report)]
            if not report.remote_ready:
                self.native.clear()
            return events
        if len(data) == 9 and data[0] == 2:
            if data[2] or any(0 < code < 4 for code in data[3:]):
                self.invalid += 1  # HID rollover/error is not a button press.
                return []
            keys = frozenset([code for code in data[3:] if code] +
                             [0xE0+i for i in range(8) if data[1] & (1 << i)])
            return self.transitions("호스트 키보드", keys,
                                    lambda c: f"{KEY_NAMES.get(c, '알 수 없는 키')} (0x{c:02X})", raw)
        if len(data) == 5 and data[0] == 3 and not data[1] & ~7:
            keys = frozenset(bit for bit in (1, 2, 4) if data[1] & bit)
            names = {1: "왼쪽 클릭", 2: "오른쪽 클릭", 4: "가운데 클릭"}
            events = self.transitions("호스트 마우스", keys, lambda c: names[c], raw)
            x, y, wheel = (v-256 if v > 127 else v for v in data[2:])
            if (x or y or wheel) and now-self.last_motion >= 0.1:
                self.last_motion = now
                events.append(DebugEvent("호스트 마우스", f"이동 X={x:+d}, Y={y:+d}, 휠={wheel:+d}", "이동", raw))
            return events
        if len(data) == 2 and data[0] == 4 and not data[1] & ~3:
            keys = frozenset(bit for bit in (1, 2) if data[1] & bit)
            return self.transitions("호스트 볼륨", keys, lambda c: "볼륨 높이기" if c == 1 else "볼륨 낮추기", raw)
        self.invalid += 1
        return []
