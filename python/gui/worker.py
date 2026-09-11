"""Own all HID I/O on one worker thread, including configuration writes."""
import logging
import queue
import threading
import time
import hid
from PySide6.QtCore import QObject, Signal
from protocol import is_bridge
from transport import configure_shared_access
from mapping import read_settings, save_settings
from debugging import DebugBatch, DebugDecoder, MODES

LOG = logging.getLogger("keymapper")


class HidWorker(QObject):
    devices = Signal(object)
    connection = Signal(bool)
    settings = Signal(object, str)
    problem = Signal(str)
    debug_reports = Signal(object)

    def __init__(self):
        super().__init__()
        self.commands = queue.Queue()
        self.stopping = threading.Event()
        self.runner = threading.Thread(target=self.run, daemon=True)

    def run(self):
        device = None
        debug_mode = None
        decoder = DebugDecoder()
        pending = []
        dropped = 0
        last_emit = time.monotonic()
        try:
            while not self.stopping.is_set():
                try:
                    command, payload = self.commands.get(timeout=0 if device else 0.1)
                except queue.Empty:
                    command, payload = None, None
                try:
                    if command == "debug":
                        debug_mode = payload if payload in MODES else None
                        decoder = DebugDecoder()
                        pending.clear()
                        dropped = 0
                    elif command == "scan":
                        self.devices.emit([d for d in hid.enumerate() if is_bridge(d)])
                    elif command in ("connect", "disconnect"):
                        decoder = DebugDecoder()
                        pending.clear()
                        dropped = 0
                        if device:
                            device.close()
                            device = None
                        if command == "disconnect":
                            self.connection.emit(False)
                        if command == "connect":
                            matches = [d for d in hid.enumerate() if is_bridge(d) and d["path"] == payload]
                            if not matches:
                                raise OSError("ESP32 HID를 찾지 못했습니다. Bluetooth 연결 후 다시 검색하세요.")
                            candidate = hid.device()
                            try:
                                configure_shared_access()
                                candidate.open_path(payload)
                            except Exception:
                                candidate.close()
                                raise
                            device = candidate
                            self.connection.emit(True)
                            self.settings.emit(read_settings(device), "connect")
                    elif command in ("read", "apply"):
                        if not device:
                            raise OSError("ESP32에 먼저 연결하세요.")
                        result = read_settings(device) if command == "read" else save_settings(device, payload)
                        self.settings.emit(result, command)
                except Exception as exc:
                    LOG.exception("HID %s failed", command)
                    if command == "connect" and not device:
                        self.connection.emit(False)
                    if command == "scan":
                        self.devices.emit([])
                    self.problem.emit(str(exc))
                if device:
                    try:
                        data = device.read(128, 100)
                        now = time.monotonic()
                        if debug_mode:
                            events = decoder.consume(data, now)
                            room = max(0, 128-len(pending))
                            pending.extend(events[:room])
                            dropped += max(0, len(events)-room)
                            if now-last_emit >= 0.1:
                                self.debug_reports.emit(DebugBatch(debug_mode, tuple(pending), decoder.packets,
                                    decoder.buttons.gaps, decoder.invalid, dropped, decoder.remote_ready))
                                pending.clear()
                                last_emit = now
                    except Exception as exc:
                        device.close()
                        device = None
                        self.connection.emit(False)
                        self.problem.emit(f"ESP32 연결이 끊겼습니다: {exc}")
        finally:
            if device:
                device.close()

    def stop(self):
        self.stopping.set()
        if self.runner.is_alive():
            self.runner.join(timeout=2)
