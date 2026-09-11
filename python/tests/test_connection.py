"""Recovery policy with fake HID endpoints and a monotonic clock, without sleeps."""
import os
from pathlib import Path
import sys
import unittest
from dataclasses import replace
from unittest.mock import patch
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gui"))
from connection import ConnectionEvent, ConnectionState as State, DeviceIdentity, DeviceSelectionRequired, ReconnectPolicy
from mapping import Snapshot, DEFAULTS
from worker import HidWorker


def device_info(path=b"bridge", serial="SERIAL-1"):
    return dict(path=path, serial_number=serial, product_string="ESP32 Remote Bridge", usage_page=0xff00, usage=1)


class Endpoint:
    def __init__(self):
        self.snapshot = Snapshot(4, DEFAULTS)
        self.read_error = None
        self.input_error = None
        self.open_error = None
        self.write_failure = None
        self.opens = self.closes = self.writes = self.reads = 0
        self.on_open = None
        self.on_settings = None

    def open_path(self, path):
        self.opens += 1
        self.path = path
        if self.on_open:
            self.on_open()
        if self.open_error:
            raise self.open_error

    def close(self):
        self.closes += 1

    def get_feature_report(self, report_id, length):
        self.reads += 1
        if self.on_settings:
            self.on_settings()
        if self.read_error:
            raise self.read_error
        return self.snapshot.encode()

    def send_feature_report(self, packet):
        self.writes += 1
        requested = Snapshot.decode(packet)
        if self.write_failure == "lost_before_commit":
            raise OSError("radio disconnected before commit")
        self.snapshot = replace(requested, revision=(requested.revision + 1) & 0xffff)
        if self.write_failure == "lost_reply":
            raise OSError("radio disconnected after commit")
        if self.write_failure == "invalid_readback":
            self.read_error = ValueError("bad readback")
        return len(packet)

    def read(self, length, timeout):
        if self.input_error:
            raise self.input_error
        return []


class WorkerRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.now = 0
        self.inventory = [device_info()]
        self.endpoint = Endpoint()
        self.worker = HidWorker(clock=lambda: self.now, enumerate_devices=lambda: self.inventory,
                                device_factory=lambda: self.endpoint)
        self.events = []
        self.settings = []
        self.problems = []
        self.worker.connection.connect(self.events.append)
        self.worker.settings.connect(lambda snapshot, operation: self.settings.append((snapshot, operation)))
        self.worker.problem.connect(self.problems.append)
        self.shared = patch("hid_session.configure_shared_access")
        self.shared.start()

    def tearDown(self):
        self.worker.session.close()
        self.shared.stop()

    def connect(self):
        self.worker.command("connect", b"bridge")

    def due(self):
        self.now = self.worker.retry.retry_at
        self.worker.poll()

    def test_ready_requires_initial_settings(self):
        self.endpoint.on_settings = lambda: self.assertNotIn(State.READY, [e.state for e in self.events])
        self.connect()
        self.assertEqual([e.state for e in self.events], [State.CONNECTING, State.READING, State.READY])
        self.assertEqual(self.settings[-1][1], "connect")

    def test_initial_read_failure_closes_handle_and_retries_once_ready(self):
        self.endpoint.read_error = OSError("temporary")
        self.connect()
        self.assertEqual(self.events[-1].state, State.RETRY_WAIT)
        self.assertEqual(self.endpoint.closes, 1)
        self.assertFalse(self.settings)
        self.assertNotIn(State.READY, [e.state for e in self.events])
        self.endpoint.read_error = None
        self.due()
        self.assertEqual(self.events[-1].state, State.READY)
        self.assertEqual(self.worker.retry.attempts, 0)

    def test_backoff_caps_and_recovers_without_writes(self):
        self.inventory = []
        self.connect()
        for delay in [1, 2, 4, 8, 10, 10]:
            self.assertEqual(self.worker.retry.retry_at-self.now, delay)
            self.due()
        self.inventory = [device_info()]
        self.due()
        self.assertEqual(self.events[-1].state, State.READY)
        self.assertEqual(self.endpoint.writes, 0)

    def test_cancel_before_queued_connect_never_opens(self):
        self.worker.request_connect(b"bridge")
        self.worker.request_disconnect()
        while not self.worker.commands.empty():
            command, payload = self.worker.commands.get_nowait()
            self.worker.command(command, payload)
        self.worker.poll()
        self.assertEqual(self.endpoint.opens, 0)
        self.assertEqual(self.events[-1].state, State.DISCONNECTED)

    def test_cancel_pending_retry_and_reconnect_explicitly(self):
        self.inventory = []
        self.connect()
        self.worker.request_disconnect()
        self.now = 100
        self.worker.poll()
        self.assertEqual(self.endpoint.opens, 0)
        self.worker.command(*self.worker.commands.get_nowait())
        self.assertIsNone(self.worker.retry.retry_at)
        self.inventory = [device_info()]
        self.worker.request_connect(b"bridge")
        self.worker.command(*self.worker.commands.get_nowait())
        self.assertEqual(self.events[-1].state, State.READY)

    def test_cancel_during_open_closes_without_reading(self):
        self.endpoint.on_open = self.worker.request_disconnect
        self.connect()
        self.assertEqual(self.endpoint.reads, 0)
        self.assertEqual(self.endpoint.closes, 1)
        self.assertNotIn(State.READY, [e.state for e in self.events])

    def test_stop_during_initial_read_does_not_emit_ready(self):
        self.endpoint.on_settings = self.worker.stop
        self.connect()
        self.assertFalse(self.settings)
        self.assertNotIn(State.READY, [e.state for e in self.events])
        self.assertIsNone(self.worker.session.device)

    def test_path_change_requires_unique_serial(self):
        self.connect()
        self.endpoint.input_error = OSError("removed")
        self.worker.poll()
        self.endpoint.input_error = None
        self.inventory = [device_info(b"new-path")]
        self.due()
        self.assertEqual(self.endpoint.path, b"new-path")
        self.assertEqual(self.settings[-1][1], "reconnect")

    def test_ambiguous_or_unknown_replacement_is_not_opened(self):
        for original, replacements in [
            (device_info(serial=""), [device_info(b"changed", "")]),
            (device_info(), [device_info(b"first"), device_info(b"second")]),
            (device_info(), [device_info(serial="OTHER")]),
        ]:
            with self.subTest(replacements=replacements):
                self.inventory = [original]
                self.connect()
                count = self.endpoint.opens
                self.endpoint.input_error = OSError("gone")
                self.worker.poll()
                self.endpoint.input_error = None
                self.inventory = replacements
                self.due()
                self.assertEqual(self.events[-1].state, State.ERROR)
                self.assertEqual(self.endpoint.opens, count)
                self.assertIsNone(self.worker.retry.retry_at)

    def test_invalid_settings_stop_retries(self):
        self.endpoint.read_error = ValueError("unsupported version")
        self.connect()
        self.assertEqual(self.events[-1].state, State.ERROR)
        self.assertIn("앱·펌웨어", self.events[-1].reason)
        self.now = 100
        self.worker.poll()
        self.assertEqual(self.endpoint.opens, 1)

    def test_no_input_does_not_trigger_reconnect(self):
        self.connect()
        for _ in range(20):
            self.now += 60
            self.worker.poll()
        self.assertEqual(self.endpoint.opens, 1)
        self.assertEqual(self.events[-1].state, State.READY)

    def test_write_disconnect_readback_never_resends(self):
        for failure in ("lost_reply", "lost_before_commit", "invalid_readback"):
            with self.subTest(failure=failure):
                self.endpoint.read_error = None
                self.connect()
                requested = replace(self.endpoint.snapshot, mouse_speed=8)
                writes = self.endpoint.writes
                self.endpoint.write_failure = failure
                self.worker.command("apply", requested)
                self.assertEqual(self.events[-1].state, State.RETRY_WAIT)
                self.assertNotEqual(self.settings[-1][1], "apply")
                self.endpoint.write_failure = self.endpoint.read_error = None
                self.due()
                self.assertEqual(self.endpoint.writes, writes+1)
                self.assertEqual(self.settings[-1][1], "recover_save")
                self.assertEqual(self.events[-1].state, State.READY)
                if failure == "lost_before_commit":
                    self.assertIn("요청과 달라", self.events[-1].reason)
                else:
                    self.assertIn("요청한 설정을 확인", self.events[-1].reason)

    def test_revision_conflict_does_not_close_or_retry(self):
        self.connect()
        self.worker.command("apply", replace(self.endpoint.snapshot, revision=0))
        self.assertTrue(self.problems)
        self.assertEqual(self.endpoint.writes, 0)
        self.assertIsNotNone(self.worker.session.device)
        self.assertIsNone(self.worker.retry.retry_at)
        self.assertIsNone(self.worker.pending_save)


class ConnectionGuiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def test_retry_preserves_draft_and_allows_cancel(self):
        from app import KeyMapper
        window = KeyMapper(start_worker=False)
        try:
            initial = Snapshot(4, DEFAULTS)
            window.on_settings(initial, "connect")
            window.on_connection(ConnectionEvent(State.READY, generation=1))
            window.mouse_speed.setValue(8)
            window.on_connection(ConnectionEvent(State.RETRY_WAIT, retry_at=2, generation=1))
            self.assertTrue(window.disconnect_button.isEnabled())
            self.assertFalse(window.apply_button.isEnabled())
            self.assertFalse(window.connected)
            window.on_settings(replace(initial, revision=5), "reconnect")
            window.on_connection(ConnectionEvent(State.READY, generation=2))
            self.assertEqual(window.mouse_speed.value(), 8)
            self.assertTrue(window.dirty)
            self.assertEqual(window.revision, 5)
            window.on_connection(ConnectionEvent(State.ERROR, generation=1))
            self.assertTrue(window.connected)
            self.assertTrue(window.worker.commands.empty())
        finally:
            window.close()

    def test_cancel_ignores_inflight_ready_and_settings(self):
        from app import KeyMapper
        window = KeyMapper(start_worker=False)
        try:
            window.on_connection(ConnectionEvent(State.READING, generation=1))
            window.disconnect_device()
            window.on_settings(Snapshot(4, DEFAULTS), "connect")
            window.on_connection(ConnectionEvent(State.READY, generation=1))
            self.assertFalse(window.connected)
            self.assertIsNone(window.revision)
            window.on_connection(ConnectionEvent(State.DISCONNECTED, generation=2))
            self.assertFalse(window.disconnect_requested)
        finally:
            window.close()
