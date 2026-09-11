"""Connection states and deterministic retry/identity policy (no HID I/O)."""
from dataclasses import dataclass
from enum import Enum


class ConnectionState(Enum):
    DISCONNECTED = "disconnected"
    CONNECTING = "connecting"
    READING = "reading"
    READY = "ready"
    RETRY_WAIT = "retry_wait"
    ERROR = "error"


@dataclass(frozen=True)
class ConnectionEvent:
    state: ConnectionState
    reason: str = ""
    retry_at: float | None = None
    generation: int = 0


class DeviceSelectionRequired(ValueError):
    pass


@dataclass
class DeviceIdentity:
    path: bytes
    serial: str = ""

    @staticmethod
    def serial_of(device):
        value = (device.get("serial_number") or "").strip()
        return "" if value.casefold() in ("", "0", "unknown", "none", "n/a") else value

    def select(self, devices):
        same_path = [d for d in devices if d["path"] == self.path]
        if len(same_path) == 1:
            current = self.serial_of(same_path[0])
            if self.serial and current != self.serial:
                raise DeviceSelectionRequired("기기 식별 정보가 변경됐습니다. 기기를 다시 선택하세요.")
            return same_path[0]
        if len(same_path) > 1:
            raise DeviceSelectionRequired("같은 경로의 기기가 여러 개입니다. 기기를 다시 선택하세요.")
        if self.serial:
            matches = [d for d in devices if self.serial_of(d) == self.serial]
            if len(matches) == 1:
                return matches[0]
            if len(matches) > 1:
                raise DeviceSelectionRequired("같은 일련번호의 기기가 여러 개입니다. 기기를 다시 선택하세요.")
        if devices:
            raise DeviceSelectionRequired("이전 기기인지 확인할 수 없습니다. 기기를 다시 선택하세요.")
        raise OSError("ESP32 HID가 없습니다. OS Bluetooth 연결을 확인하세요.")


@dataclass
class ReconnectPolicy:
    attempts: int = 0
    retry_at: float | None = None

    def failed(self, now):
        delay = (1, 2, 4, 8, 10)[min(self.attempts, 4)]
        self.attempts += 1
        self.retry_at = now + delay
        return self.retry_at

    def reset(self):
        self.attempts = 0
        self.retry_at = None

    def due(self, now):
        return self.retry_at is not None and now >= self.retry_at
