"""Serialize user commands and HID I/O; recover only the selected device."""
from dataclasses import replace
import logging
import queue
import threading
import time
import hid
from PySide6.QtCore import QObject, Signal
from discovery import discover
from connection import ConnectionEvent, ConnectionState, DeviceIdentity, DeviceSelectionRequired, ReconnectPolicy
from hid_session import HidSession
from debugging import DebugBatch, DebugDecoder, MODES

LOG = logging.getLogger("keymapper")


class HidWorker(QObject):
    devices = Signal(object)
    discovery = Signal(object)
    connection = Signal(object)
    settings = Signal(object, str)
    problem = Signal(str)
    debug_reports = Signal(object)

    def __init__(self, *, clock=None, enumerate_devices=None, device_factory=None):
        super().__init__()
        self.commands = queue.Queue()
        self.stopping = threading.Event()
        self.cancel_requested = threading.Event()
        self.runner = threading.Thread(target=self.run, daemon=True)
        self.clock = clock or time.monotonic
        self.enumerate_devices = enumerate_devices or hid.enumerate
        self.session = HidSession(device_factory or hid.device)
        self.retry = ReconnectPolicy()
        self.target = None
        self.active = False
        self.generation = 0
        self.pending_save = None
        self.ever_ready = False
        self.last_state = None
        self.debug_mode = None
        self._reset_debug()

    def _reset_debug(self):
        self.decoder = DebugDecoder()
        self.pending = []
        self.dropped = 0
        self.last_emit = self.clock()

    def _state(self, state, reason="", retry_at=None, device=None):
        event = ConnectionEvent(state, reason, retry_at, self.generation,
                                device["path"] if device else None,
                                DeviceIdentity.serial_of(device) if device else "")
        # Repeated 10-second retries remain visible in the UI without flooding logs.
        if (state, reason) != self.last_state:
            log = LOG.debug if self.retry.attempts > 4 else LOG.info
            log("HID state=%s generation=%d reason=%s", state.value, self.generation, reason)
            self.last_state = (state, reason)
        self.connection.emit(event)

    def _devices(self):
        result = discover(self.enumerate_devices)
        self.discovery.emit(result)
        if result.error:
            raise OSError(result.message)
        return result.devices

    def _cancelled(self):
        return self.cancel_requested.is_set() or self.stopping.is_set()

    def _transport_failed(self, exc):
        self.session.close()
        self._reset_debug()
        if self.active and not self._cancelled():
            retry_at = self.retry.failed(self.clock())
            delay = round(retry_at - self.clock())
            self._state(ConnectionState.RETRY_WAIT,
                        f"다시 연결 중 · {max(1, delay)}초 후 재시도. {exc}", retry_at)

    def _fatal(self, exc):
        self.active = False
        self.retry.reset()
        self.session.close()
        self._state(ConnectionState.ERROR, str(exc))

    def _attempt(self):
        if not self.active or self._cancelled():
            return
        self.retry.retry_at = None
        self._state(ConnectionState.CONNECTING, "선택한 설정용 HID에 연결하는 중…")
        try:
            candidate = self.target.select(self._devices())
            if self._cancelled():
                return
            self.target.path = candidate["path"]
            if not self.target.serial:
                self.target.serial = DeviceIdentity.serial_of(candidate)
            self.session.open(self.target.path)
            if self._cancelled():
                self.session.close()
                return
            self._state(ConnectionState.READING, "장치에 저장된 설정을 확인하는 중…")
            snapshot = self.session.read_settings()
            if self._cancelled():
                self.session.close()
                return
            operation = "reconnect" if self.ever_ready else "connect"
            reason = "설정 읽기 완료 · HID 통신 사용 가능"
            if self.pending_save is not None:
                expected = replace(self.pending_save, revision=(self.pending_save.revision + 1) & 0xffff)
                if snapshot == expected:
                    reason = "재연결 후 장치에서 요청한 설정을 확인했습니다."
                else:
                    reason = "장치 설정을 다시 읽었습니다. 이전 저장 결과가 요청과 달라 자동으로 다시 저장하지 않았습니다."
                operation = "recover_save"
                self.pending_save = None
            self.ever_ready = True
            self.retry.reset()
            self._reset_debug()
            # Settings must be applied before READY; opening a handle isn't readiness.
            self.settings.emit(snapshot, operation)
            self._state(ConnectionState.READY, reason, device=candidate)
        except OSError as exc:
            self._transport_failed(exc)
        except ValueError as exc:
            self._fatal(exc if isinstance(exc, DeviceSelectionRequired) else
                        ValueError(f"ESP32 설정 프로토콜 불일치: 지원 장치인지 또는 앱·펌웨어 버전이 맞는지 확인하세요. {exc}"))
        except Exception as exc:
            LOG.exception("HID initialization failed")
            self._fatal(exc)

    def command(self, command, payload=None):
        """One worker-thread command; exposed for deterministic unit tests."""
        if command == "debug":
            self.debug_mode = payload if payload in MODES else None
            self._reset_debug()
        elif command == "scan":
            result = discover(self.enumerate_devices)
            self.discovery.emit(result)
            self.devices.emit(result.devices)
        elif command == "disconnect":
            self.active = False
            self.generation += 1
            self.retry.reset()
            self.session.close()
            self._reset_debug()
            self._state(ConnectionState.DISCONNECTED, "연결 해제됨 · 편집 내용은 유지됩니다.")
        elif command == "connect":
            self.session.close()
            self.generation += 1
            self.active = True
            self.retry.reset()
            self.target = DeviceIdentity(payload)
            self.pending_save = None
            self.ever_ready = False
            self._reset_debug()
            self._attempt()
        elif command in ("read", "apply"):
            if not self.session.device or self._cancelled():
                self.problem.emit("사용 가능한 ESP32에 먼저 연결하세요.")
                return
            try:
                if command == "apply":
                    self.pending_save = payload
                    snapshot = self.session.save(payload)
                    self.pending_save = None
                else:
                    snapshot = self.session.read_settings()
                if not self._cancelled():
                    self.settings.emit(snapshot, command)
            except OSError as exc:
                self._transport_failed(exc)
            except ValueError as exc:
                self.pending_save = None
                if command == "read":
                    self._fatal(exc)
                else:
                    # Revision/validation errors require a fresh read, not reconnect.
                    self.problem.emit(str(exc))
            except Exception as exc:
                LOG.exception("HID %s failed", command)
                self._fatal(exc)

    def poll(self):
        if self._cancelled():
            return
        if self.active and self.retry.due(self.clock()):
            self._attempt()
        if not self.session.device or self._cancelled():
            return
        try:
            data = self.session.read_input()
        except OSError as exc:
            self._transport_failed(exc)
            return
        if self.debug_mode:
            now = self.clock()
            events = self.decoder.consume(data, now)
            room = max(0, 128-len(self.pending))
            self.pending.extend(events[:room])
            self.dropped += max(0, len(events)-room)
            if now-self.last_emit >= 0.1:
                self.debug_reports.emit(DebugBatch(self.debug_mode, tuple(self.pending), self.decoder.packets,
                    self.decoder.buttons.gaps, self.decoder.invalid, self.dropped, self.decoder.remote_ready))
                self.pending.clear()
                self.last_emit = now

    def run(self):
        try:
            while not self.stopping.is_set():
                try:
                    command, payload = self.commands.get(timeout=0 if self.session.device else 0.1)
                except queue.Empty:
                    command = None
                if command:
                    self.command(command, payload)
                self.poll()
        except Exception as exc:
            LOG.exception("HID worker stopped unexpectedly")
            self._fatal(exc)
        finally:
            self.session.close()

    def request_connect(self, path):
        self.cancel_requested.clear()
        self.commands.put(("connect", path))

    def request_disconnect(self):
        # Cancellation intent is visible even while an OS HID call is in progress.
        self.cancel_requested.set()
        self.commands.put(("disconnect", None))

    def stop(self):
        self.stopping.set()
        self.cancel_requested.set()
        if self.runner.is_alive():
            self.runner.join(timeout=2)
