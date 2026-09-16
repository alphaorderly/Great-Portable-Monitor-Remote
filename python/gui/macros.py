"""ASCII string macros; chunked Feature 6 protocol with explicit, verified commit."""
from dataclasses import dataclass
import secrets
import struct
from protocol import BUTTONS

REPORT_ID = 6
MAX_SIZE = 512
HEADER = struct.Struct('<4sBBHHHHHHH')
READ, BEGIN, CHUNK, COMMIT, STOP, STATUS = range(1, 7)


@dataclass(frozen=True)
class Text:
    text: str


@dataclass(frozen=True)
class Wait:
    minimum: int = 500
    maximum: int = 1000


@dataclass(frozen=True)
class Macro:
    enabled: bool = False
    repeats: int = 1
    hold_ms: int = 40
    gap_min: int = 80
    gap_max: int = 180
    repeat_min: int = 0
    repeat_max: int = 0
    steps: tuple = ()

    def encode(self):
        if type(self.enabled) is not bool or not 1 <= self.repeats <= 100:
            raise ValueError('반복 횟수는 1~100회입니다.')
        if not 10 <= self.hold_ms <= 500:
            raise ValueError('키 누름 시간은 10~500ms입니다.')
        if not 0 <= self.gap_min <= self.gap_max <= 5000:
            raise ValueError('문자 간격은 0~5,000ms이며 최솟값이 최댓값 이하여야 합니다.')
        if not 0 <= self.repeat_min <= self.repeat_max <= 60000:
            raise ValueError('반복 간격은 0~60,000ms이며 최솟값이 최댓값 이하여야 합니다.')
        code = bytearray()
        has_text = False
        for step in self.steps:
            if isinstance(step, Text):
                if not step.text or any(not (' ' <= ch <= '~' or ch in '\n\t') for ch in step.text):
                    raise ValueError('빈 문자열은 제외하고 영문·숫자·기본 기호·줄바꿈·탭만 입력하세요.')
                raw = step.text.encode('ascii')
                if len(raw) > MAX_SIZE:
                    raise ValueError('문자열이 너무 깁니다. 매크로는 최대 512바이트입니다.')
                code += b'\x01' + struct.pack('<H', len(raw)) + raw
                has_text = True
            elif isinstance(step, Wait):
                if not 0 <= step.minimum <= step.maximum <= 60000:
                    raise ValueError('대기는 0~60,000ms이며 최솟값이 최댓값 이하여야 합니다.')
                code += b'\x02' + struct.pack('<HH', step.minimum, step.maximum)
            else:
                raise ValueError('지원하지 않는 매크로 단계입니다.')
        if self.enabled and not has_text:
            raise ValueError('매크로를 사용하려면 입력할 문자열을 추가하세요.')
        if len(code) + HEADER.size > MAX_SIZE:
            raise ValueError('매크로는 단계와 설정을 포함해 최대 512바이트입니다.')
        return HEADER.pack(b'MC\x01\x00', self.enabled, self.repeats, self.hold_ms,
                           self.gap_min, self.gap_max, self.repeat_min, self.repeat_max, len(code), 0) + code

    @classmethod
    def decode(cls, raw):
        if not HEADER.size <= len(raw) <= MAX_SIZE:
            raise ValueError('매크로 데이터 길이가 올바르지 않습니다.')
        magic, enabled, repeats, hold, low, high, rlow, rhigh, length, reserved = HEADER.unpack_from(raw)
        if magic != b'MC\x01\x00' or enabled > 1 or reserved or length != len(raw) - HEADER.size:
            raise ValueError('지원하지 않는 매크로 형식입니다.')
        steps, pos = [], HEADER.size
        try:
            while pos < len(raw):
                op = raw[pos]
                pos += 1
                if op == 1:
                    size, = struct.unpack_from('<H', raw, pos)
                    pos += 2
                    if pos + size > len(raw):
                        raise ValueError('문자열 데이터가 잘렸습니다.')
                    steps.append(Text(raw[pos:pos+size].decode('ascii')))
                    pos += size
                elif op == 2:
                    steps.append(Wait(*struct.unpack_from('<HH', raw, pos)))
                    pos += 4
                else:
                    raise ValueError('지원하지 않는 매크로 단계입니다.')
        except (struct.error, UnicodeDecodeError) as exc:
            raise ValueError('매크로 데이터가 손상되었습니다.') from exc
        result = cls(bool(enabled), repeats, hold, low, high, rlow, rhigh, tuple(steps))
        result.encode()
        return result


def slot_for(button, mode):
    if mode not in (0, 1) or button not in BUTTONS or button == 0x6A or (mode == 1 and button == 0x4A):
        raise ValueError('이 버튼에는 매크로를 지정할 수 없습니다.')
    return mode * len(BUTTONS) + tuple(BUTTONS).index(button)


class MacroProtocol:
    def __init__(self, device, cancelled=lambda: False):
        self.device = device
        self.cancelled = cancelled

    def exchange(self, command, slot=0, *, token=None, revision=0, offset=0, total=0, data=b''):
        if self.cancelled():
            raise OSError('매크로 작업을 취소했습니다. 다시 읽어 저장 상태를 확인하세요.')
        token = secrets.randbelow(65536) if token is None else token
        request = bytearray(64)
        request[:3] = b'MX\x01'
        request[3:5] = bytes((command, slot))
        struct.pack_into('<HHH', request, 6, token, revision, offset)
        struct.pack_into('<H', request, 12, total)
        request[14] = len(data)
        request[16:16+len(data)] = data
        if len(data) > 48:
            raise ValueError('매크로 전송 청크가 너무 큽니다.')
        if self.device.send_feature_report(bytes((REPORT_ID,)) + request) != 65:
            raise OSError('매크로 명령 전송을 확인하지 못했습니다. 자동 재전송하지 않습니다.')
        response = bytes(self.device.get_feature_report(REPORT_ID, 65))
        if len(response) == 65 and response[0] == REPORT_ID:
            response = response[1:]
        if len(response) != 64 or response[:5] != request[:5] or response[6:8] != request[6:8] or response[10:12] != request[10:12] or response[14] > 48:
            raise ValueError('매크로 응답을 확인할 수 없습니다. 앱과 새 펌웨어를 함께 사용하고 다시 읽어주세요.')
        if response[5]:
            raise ValueError({1: '매크로 요청이 올바르지 않거나 전송이 만료되었습니다.',
                              2: '장치의 매크로가 변경되었습니다. 다시 읽어주세요.',
                              3: '장치에 매크로를 저장하지 못했습니다.',
                              4: '매크로 실행을 중지한 뒤 저장하세요.'}.get(response[5], '매크로 명령을 처리하지 못했습니다.'))
        return response

    def read(self, slot):
        data, revision, total = bytearray(), None, None
        while total is None or len(data) < total:
            reply = self.exchange(READ, slot, offset=len(data))
            rev, = struct.unpack_from('<H', reply, 8)
            size, = struct.unpack_from('<H', reply, 12)
            if not HEADER.size <= size <= MAX_SIZE or (revision is not None and (rev != revision or total != size)):
                raise ValueError('읽는 도중 매크로가 바뀌었습니다. 다시 읽어주세요.')
            count = reply[14]
            if count != min(48, size-len(data)):
                raise ValueError('매크로 읽기 길이가 일치하지 않습니다.')
            revision, total = rev, size
            data += reply[16:16+count]
        return revision, Macro.decode(data)

    def save(self, slot, revision, macro):
        raw = macro.encode()
        token = secrets.randbelow(65536)
        args = dict(token=token, revision=revision, total=len(raw))
        self.exchange(BEGIN, slot, **args)
        for offset in range(0, len(raw), 48):
            self.exchange(CHUNK, slot, offset=offset, data=raw[offset:offset+48], **args)
        self.exchange(COMMIT, slot, **args)
        actual_revision, actual = self.read(slot)
        if actual_revision != (revision + 1) % 65536 or actual != macro:
            raise OSError('저장한 매크로를 확인하지 못했습니다. 다시 읽어주세요. 자동으로 재저장하지 않습니다.')
        return actual_revision, actual

    def stop(self):
        self.exchange(STOP)

    def status(self):
        reply = self.exchange(STATUS)
        if reply[14] != 5:
            raise ValueError('매크로 상태 응답이 올바르지 않습니다.')
        return int.from_bytes(reply[16:20], 'little'), reply[20]
