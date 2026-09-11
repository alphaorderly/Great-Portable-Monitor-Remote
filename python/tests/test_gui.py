"""Protocol/state and offscreen GUI checks; no Bluetooth or HID device required."""
import os
from pathlib import Path
import sys
import time
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gui"))
from connection import ConnectionEvent, ConnectionState
from protocol import BUTTONS, ButtonState, Report, is_bridge


def frame(sequence, keys=(), ready=True, event=True, with_id=True):
    keyboard = bytes([0, 0, *keys]).ljust(8, b"\0")
    assert len(keyboard) == 8
    data = bytes([1, ready | (event << 1)]) + sequence.to_bytes(2, "little") + keyboard
    return (b"\x01" if with_id else b"") + data


class ProtocolTests(unittest.TestCase):
    def test_composite_native_reports_do_not_enter_gui_protocol(self):
        from transport import is_native_report
        for packet in (bytes([2]) + bytes(8), bytes([3, 1, 255, 2, 0]), bytes([4, 1])):
            self.assertTrue(is_native_report(packet))
        for packet in (frame(1), frame(1, with_id=False), b"", bytes([1, 0]), bytes([3, 0])):
            self.assertFalse(is_native_report(packet))

    @unittest.skipUnless(sys.platform == "darwin", "Darwin HIDAPI option")
    def test_installed_hidapi_supports_shared_open(self):
        import ctypes
        import hid
        from transport import configure_shared_access
        configure_shared_access()
        self.assertEqual(ctypes.CDLL(hid.__file__).hid_darwin_get_open_exclusive(), 0)

    def test_report_id_and_strict_validation(self):
        a = Report.decode(frame(24, (0x50,)))
        self.assertEqual(a, Report.decode(frame(24, (0x50,), with_id=False)))
        self.assertEqual(a.keys, {0x50})
        for raw in (b"", bytes(8), b"\x02" + frame(1)[1:], b"\x01\x02" + frame(1)[2:], b"\x01\x01\x80" + frame(1)[3:]):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                Report.decode(raw)

    def test_actual_button_codes_press_release(self):
        state = ButtonState()
        sequence = 0
        for code in BUTTONS:
            sequence += 1
            self.assertEqual(state.update(Report.decode(frame(sequence, (code,)))), [("누름", code)])
            sequence += 1
            self.assertEqual(state.update(Report.decode(frame(sequence))), [("해제", code)])
        self.assertEqual(state.gaps, 0)

    def test_snapshot_not_physical_press_and_lost_release_recovery(self):
        state = ButtonState()
        self.assertEqual(state.update(Report.decode(frame(10, (0x50,)))), [("누름", 0x50)])
        self.assertEqual(state.update(Report.decode(frame(11, (0x50,), event=False))), [])
        self.assertEqual(state.update(Report.decode(frame(13, event=False))), [("상태 해제", 0x50)])
        self.assertEqual(state.gaps, 1)
        self.assertEqual(state.update(Report.decode(frame(14, (0x52,), event=False))), [("상태 복구", 0x52)])
        self.assertEqual(state.update(Report.decode(frame(15, ready=False, event=False))), [("상태 해제", 0x52)])

    def test_multikey_repeat_duplicate_wrap_unknown(self):
        state = ButtonState()
        state.update(Report.decode(frame(65535, (0x50, 0x52))))
        self.assertEqual(state.update(Report.decode(frame(0, (0x50, 0x52)))), [("반복", 0x50), ("반복", 0x52)])
        self.assertEqual(state.update(Report.decode(frame(0, (0x50, 0x52)))), [])
        self.assertEqual(state.gaps, 0)
        self.assertEqual(Report.decode(frame(1, (0xAB,))).keys, {0xAB})

    def test_only_dedicated_collection_matches(self):
        target = {"product_string": "ESP32 Remote Bridge", "usage_page": 0xFF00, "usage": 1}
        self.assertTrue(is_bridge(target))
        self.assertFalse(is_bridge({**target, "usage_page": 7}))
        self.assertFalse(is_bridge({**target, "product_string": "Other vendor device"}))
        self.assertFalse(is_bridge({}))


class GuiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def test_mapper_defaults_and_explicit_verified_save(self):
        from app import KeyMapper
        from mapping import DEFAULTS, Snapshot, KEY
        window = KeyMapper(start_worker=False, host_platform="darwin")
        try:
            self.assertEqual(window.entries(), DEFAULTS)
            self.assertFalse(window.controls[0x6A][0].isEnabled())
            self.assertEqual(window.entries()[2].kind, 0)
            self.assertTrue(all(not box.isEnabled() for box in window.controls[0x6A][1].values()))
            self.assertEqual(window.controls[0x66][0].currentText(), "F16")
            self.assertFalse(window.apply_button.isEnabled())
            window.on_connection(ConnectionEvent(ConnectionState.READY))
            window.on_settings(Snapshot(7, DEFAULTS), "connect")
            combo, modifiers = window.controls[0x28]
            combo.setCurrentIndex(combo.findData((KEY << 8) | 4))
            modifiers[8].setChecked(True)
            self.assertTrue(window.dirty)
            self.assertTrue(window.apply_button.isEnabled())
            window.apply_settings()
            command, snapshot = window.worker.commands.get_nowait()
            self.assertEqual(command, "apply")
            self.assertEqual(snapshot.revision, 7)
            self.assertEqual(snapshot.entries[8].key, 4)
            self.assertEqual(snapshot.entries[8].modifiers, 8)
            self.assertFalse(window.table.isEnabled())
            self.assertNotIn("저장 완료", window.status.text())
            window.on_settings(Snapshot(8, snapshot.entries), "apply")
            self.assertFalse(window.dirty)
            self.assertEqual(window.revision, 8)
            self.assertIn("저장 완료", window.status.text())
            window.load_defaults()
            self.assertTrue(window.dirty)
            self.assertTrue(window.worker.commands.empty())
            window.on_problem("저장 확인 실패")
            self.assertIsNone(window.revision)
            self.assertFalse(window.apply_button.isEnabled())
            self.assertTrue(window.read_button.isEnabled())
        finally:
            window.close()

    def test_mouse_speed_save_read_defaults_and_reserved_home(self):
        from dataclasses import replace
        from app import KeyMapper
        from mapping import DEFAULTS, Snapshot, SPEED_DEFAULT
        window = KeyMapper(start_worker=False, host_platform="darwin")
        try:
            window.on_connection(ConnectionEvent(ConnectionState.READY))
            window.on_settings(Snapshot(9, DEFAULTS, mouse_speed=6), "connect")
            self.assertEqual(window.mouse_speed.value(), 6)
            self.assertEqual(window.speed_label.text(), "1.5배")
            self.assertFalse(window.dirty)
            window.mapping_mode.setCurrentIndex(1)
            self.assertFalse(window.mouse_settings.isHidden())
            combo, checks = window.controls[0x4A]
            self.assertFalse(combo.isEnabled())
            self.assertIn("커서 정지", combo.currentText())
            self.assertTrue(all(not box.isEnabled() for box in checks.values()))
            self.assertEqual(window.entries()[10].kind, 0)
            window.mouse_speed.setValue(1)
            self.assertEqual(window.speed_label.text(), "0.25배")
            self.assertTrue(window.dirty)
            window.mapping_mode.setCurrentIndex(0)
            self.assertTrue(combo.isEnabled())
            self.assertEqual(window.entries()[10].key, 0x4A)
            window.load_defaults()  # Normal defaults do not reset mouse speed.
            self.assertEqual(window.mouse_speed.value(), 1)
            window.apply_settings()
            command, snapshot = window.worker.commands.get_nowait()
            self.assertEqual(command, "apply")
            self.assertEqual(snapshot.mouse_speed, 1)
            self.assertFalse(window.mouse_speed.isEnabled())
            window.on_settings(replace(snapshot, revision=10), "apply")
            self.assertFalse(window.dirty)
            window.mapping_mode.setCurrentIndex(1)
            window.load_defaults()
            self.assertEqual(window.mouse_speed.value(), SPEED_DEFAULT)
            self.assertTrue(window.dirty)
            window.on_settings(replace(snapshot, revision=10), "read")
            self.assertEqual(window.mouse_speed.value(), 1)
            self.assertFalse(window.dirty)
        finally:
            window.close()

    def test_mouse_speed_offline_draft_survives_connection(self):
        from app import KeyMapper
        from mapping import DEFAULTS, Snapshot
        window = KeyMapper(start_worker=False)
        try:
            window.mouse_speed.setValue(12)
            window.on_connection(ConnectionEvent(ConnectionState.READY))
            window.on_settings(Snapshot(1, DEFAULTS, mouse_speed=2), "connect")
            self.assertEqual(window.mouse_speed.value(), 12)
            self.assertTrue(window.dirty)
            window.on_connection(ConnectionEvent(ConnectionState.DISCONNECTED))
            self.assertEqual(window.mouse_speed.value(), 12)
            window.on_settings(Snapshot(1, DEFAULTS, mouse_speed=2), "read")
            self.assertEqual(window.mouse_speed.value(), 2)
            self.assertFalse(window.dirty)
        finally:
            window.close()

    def test_offline_draft_survives_connect_and_media_disables_modifiers(self):
        from app import KeyMapper
        from mapping import DEFAULTS, Snapshot, KEY, VOLUME
        window = KeyMapper(start_worker=False, host_platform="darwin")
        try:
            combo, modifiers = window.controls[0x66]
            combo.setCurrentIndex(combo.findData((KEY << 8) | 5))
            modifiers[8].setChecked(True)
            self.assertTrue(window.dirty)
            window.on_connection(ConnectionEvent(ConnectionState.READY))
            window.on_settings(Snapshot(2, DEFAULTS), "connect")
            self.assertEqual(window.entries()[0].key, 5)
            self.assertEqual(window.entries()[0].modifiers, 8)
            combo.setCurrentIndex(combo.findData((VOLUME << 8) | 1))
            self.assertEqual(window.entries()[0].modifiers, 0)
            self.assertFalse(modifiers[8].isEnabled())
            window.on_connection(ConnectionEvent(ConnectionState.DISCONNECTED))
            self.assertEqual(window.entries()[0].kind, VOLUME)
            self.assertFalse(window.apply_button.isEnabled())
            window.on_devices([])
            self.assertFalse(window.connect_button.isEnabled())
        finally:
            window.close()

    def test_independent_mode_drafts_and_atomic_save(self):
        from app import KeyMapper
        from mapping import DEFAULTS, MOUSE_DEFAULTS, Snapshot, KEY, MOUSE
        window = KeyMapper(start_worker=False, host_platform="darwin")
        try:
            window.on_connection(ConnectionEvent(ConnectionState.READY))
            window.on_settings(Snapshot(10, DEFAULTS), "connect")
            normal = window.controls[0x50][0]
            normal.setCurrentIndex(normal.findData((KEY << 8) | 4))
            window.mapping_mode.setCurrentIndex(1)
            self.assertEqual(window.entries(), MOUSE_DEFAULTS)
            combo, modifiers = window.controls[0x50]
            combo.setCurrentIndex(combo.findData((MOUSE << 8) | 4))
            self.assertTrue(all(not box.isEnabled() for box in modifiers.values()))
            window.mapping_mode.setCurrentIndex(0)
            self.assertEqual(window.entries()[6].key, 4)
            window.apply_settings()
            command, snapshot = window.worker.commands.get_nowait()
            self.assertEqual(command, "apply")
            self.assertEqual(snapshot.entries[6].kind, KEY)
            self.assertEqual(snapshot.mouse_entries[6].kind, MOUSE)
            self.assertEqual(snapshot.mouse_entries[6].key, 4)
            self.assertEqual(len(snapshot.encode()), 65)
            window.on_settings(Snapshot(11, snapshot.entries, snapshot.mouse_entries), "apply")
            self.assertFalse(window.dirty)
            window.mapping_mode.setCurrentIndex(1)
            window.load_defaults()
            self.assertEqual(window.entries(), MOUSE_DEFAULTS)
            window.mapping_mode.setCurrentIndex(0)
            self.assertEqual(window.entries()[6].key, 4)  # Other draft preserved.
        finally:
            window.close()

    def test_debug_mode_comparison_pause_and_bounded_history(self):
        from app import KeyMapper
        from debugging import DebugBatch, DebugEvent, MODES
        window = KeyMapper(start_worker=False, host_platform="darwin")
        try:
            panel = window.debug_panel
            panel.set_connected(True)
            event = DebugEvent("리모컨 원본", "OK (0x28)", "누름", "01 01 03 02 00 00 00 28 00 00 00 00 00")
            def batch(mode, events=(event,)):
                return DebugBatch(mode, events, 3, 0, 0, 0, True)
            panel.on_batch(batch(MODES[0]))
            self.assertEqual(panel.history.rowCount(), 0)
            panel.enabled.setChecked(True)
            self.assertEqual(window.worker.commands.get_nowait(), ("debug", MODES[0]))
            panel.on_batch(batch(MODES[0]))
            panel.mode.setCurrentIndex(1)
            panel.on_batch(batch(MODES[0]))  # Ignore a delayed batch from previous mode.
            panel.on_batch(batch(MODES[1]))
            self.assertEqual(panel.counts["리모컨 원본 · OK (0x28)"], [1, 1])
            motion = DebugEvent("호스트 마우스", "이동 X=+1", "이동", "03 00 01 00 00")
            panel.on_batch(batch(MODES[1], (motion,)*(panel.MAX_ROWS+10)))
            self.assertEqual(panel.history.rowCount(), panel.MAX_ROWS)
            self.assertEqual(len(panel.counts), 1)
            panel.set_connected(False)
            panel.on_batch(batch(MODES[1]))
            self.assertEqual(panel.counts["리모컨 원본 · OK (0x28)"], [1, 1])
            panel.clear()
            self.assertEqual(panel.history.rowCount(), 0)
            self.assertFalse(panel.counts)
            self.assertFalse(window.dirty)  # Observing never edits/saves mappings.
        finally:
            window.close()


class DebugTests(unittest.TestCase):
    def test_original_buttons_heartbeat_unknown_and_duplicate(self):
        from debugging import DebugDecoder
        decoder = DebugDecoder()
        self.assertEqual(decoder.consume(frame(10, event=False), 0), [])
        events = decoder.consume(frame(11, (0x28,)), 1)
        self.assertEqual([(e.name, e.action) for e in events], [("OK (0x28)", "누름")])
        self.assertEqual(decoder.consume(frame(11, (0x28,)), 2), [])
        self.assertEqual(decoder.consume(frame(12, (0x28,), event=False), 3), [])
        self.assertEqual(decoder.consume(frame(14, event=False), 4)[0].action, "상태 해제")
        self.assertEqual(decoder.buttons.gaps, 1)
        self.assertIn("알 수 없는 버튼 0xAB", decoder.consume(frame(15, (0xAB,)), 5)[0].name)

    def test_mouse_click_is_not_ok_key_and_motion_is_throttled(self):
        from debugging import DebugDecoder
        decoder = DebugDecoder()
        decoder.consume(bytes([3, 0, 0, 0, 0]), 0)
        events = decoder.consume(bytes([3, 1, 255, 2, 0]), 1)
        self.assertEqual(events[0].name, "왼쪽 클릭")
        self.assertEqual(events[0].action, "누름")
        self.assertEqual(events[1].name, "이동 X=-1, Y=+2, 휠=+0")
        self.assertEqual(decoder.consume(bytes([3, 1, 255, 2, 0]), 1.01), [])
        events = decoder.consume(bytes([3, 0, 0, 0, 0]), 1.02)
        self.assertEqual([(e.name, e.action) for e in events], [("왼쪽 클릭", "해제")])
        self.assertIsNone(decoder.buttons.sequence)

    def test_native_keyboard_modifiers_volume_and_invalid_input(self):
        from debugging import DebugDecoder
        decoder = DebugDecoder()
        decoder.consume(bytes([2])+bytes(8), 0)
        events = decoder.consume(bytes([2, 8, 0, 0x2B, 0, 0, 0, 0, 0]), 1)
        self.assertEqual({e.name for e in events}, {"Tab (0x2B)", "왼쪽 Win / Command (0xE3)"})
        self.assertTrue(all(e.source == "호스트 키보드" and e.action == "누름" for e in events))
        decoder.consume(bytes([4, 0]), 1)
        self.assertEqual(decoder.consume(bytes([4, 1]), 2)[0].name, "볼륨 높이기")
        for packet in (b"\x03\x00", bytes([3, 128, 0, 0, 0]), bytes([4, 255]), bytes([2, 0, 0, 1, 0, 0, 0, 0, 0])):
            self.assertEqual(decoder.consume(packet, 3), [])
        self.assertEqual(decoder.invalid, 4)

    def test_starting_with_held_input_is_a_snapshot_not_a_new_press(self):
        from debugging import DebugDecoder
        decoder = DebugDecoder()
        self.assertEqual(decoder.consume(frame(5, (0x28,)), 0)[0].action, "상태 복구")
        self.assertEqual(decoder.consume(bytes([3, 1, 0, 0, 0]), 0)[0].action, "상태 복구")

    def test_worker_reads_selected_device_and_debug_never_writes_settings(self):
        from unittest.mock import patch
        import itertools
        from worker import HidWorker
        from mapping import Snapshot, DEFAULTS
        from debugging import MODES
        worker = HidWorker()
        batches = []
        worker.debug_reports.connect(batches.append)
        packets = iter([frame(1, event=False), frame(2, (0x28,)), frame(3),
                        bytes([3, 0, 0, 0, 0]), bytes([3, 1, 0, 0, 0])])
        class Device:
            closed = False
            def open_path(self, path):
                self.path = path
            def get_feature_report(self, report_id, length):
                return Snapshot(1, DEFAULTS).encode()
            def send_feature_report(self, data):
                raise AssertionError("Debugging must never write a Feature report")
            def read(self, length, timeout):
                packet = next(packets, None)
                if packet is None:
                    worker.stopping.set()
                    return []
                return packet
            def close(self):
                self.closed = True
        device = Device()
        worker.commands.put(("debug", MODES[1]))
        worker.commands.put(("connect", b"only-bridge"))
        clock = itertools.count()
        with patch.object(worker, "enumerate_devices", return_value=[{"product_string": "ESP32 Remote Bridge",
                   "usage_page": 0xFF00, "usage": 1, "path": b"only-bridge"}]), \
             patch.object(worker.session, "device_factory", return_value=device), \
             patch("hid_session.configure_shared_access"), \
             patch.object(worker, "clock", side_effect=lambda: next(clock)*0.2):
            worker.run()
        self.assertEqual(device.path, b"only-bridge")
        self.assertTrue(device.closed)
        names = {e.name for batch in batches for e in batch.events}
        self.assertIn("OK (0x28)", names)
        self.assertIn("왼쪽 클릭", names)
        self.assertTrue(all(batch.mode == MODES[1] for batch in batches))


class MappingTests(unittest.TestCase):
    def test_speed_roundtrip_validation_and_old_firmware_rejected(self):
        from dataclasses import replace
        from mapping import DEFAULTS, Snapshot, Entry, KEY
        for speed in range(1, 13):
            snapshot = Snapshot(5, DEFAULTS, mouse_speed=speed)
            self.assertEqual(Snapshot.decode(snapshot.encode()), snapshot)
        for speed in (0, 13, 255, -1, 1.5):
            with self.subTest(speed=speed), self.assertRaises(ValueError):
                Snapshot(0, DEFAULTS, mouse_speed=speed).encode()
        snapshot = Snapshot(0, DEFAULTS)
        bad_home = (*snapshot.mouse_entries[:10], Entry(0x4A, KEY, 4), *snapshot.mouse_entries[11:])
        with self.assertRaises(ValueError):
            replace(snapshot, mouse_entries=bad_home).encode()
        old = bytes.fromhex((Path(__file__).resolve().parents[2] / "shared/fixtures/keymap-v2-default.hex").read_text())
        with self.assertRaisesRegex(ValueError, "새 ESP32 펌웨어"):
            Snapshot.decode(old)

    def test_protocol_roundtrip_and_shared_c_defaults(self):
        from mapping import DEFAULTS, Snapshot
        snapshot = Snapshot(0, DEFAULTS)
        golden = bytes.fromhex((Path(__file__).resolve().parents[2] / "shared/fixtures/keymap-default.hex").read_text())
        self.assertEqual(snapshot.encode(False), golden)
        self.assertEqual(Snapshot.decode(snapshot.encode()), snapshot)
        self.assertEqual(Snapshot.decode(snapshot.encode(False)), snapshot)

    def test_invalid_feature_and_mapping_rejected(self):
        from mapping import DEFAULTS, Snapshot
        good = Snapshot(1, DEFAULTS).encode()
        for raw in (b"", bytes(64), good[:-1], bytes([1])+good[1:]):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                Snapshot.decode(raw)
        for offset in (1, 3, 4, 7, 8, 9, 10, 11, 12):
            raw = bytearray(good)
            raw[offset] = 255
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                Snapshot.decode(raw)

    def test_save_requires_revision_and_readback(self):
        from dataclasses import replace
        from mapping import DEFAULTS, Entry, Snapshot, save_settings, REPORT_ID
        class Device:
            def __init__(self):
                self.snapshot = Snapshot(3, DEFAULTS)
                self.writes = 0
                self.bad_readback = False
            def get_feature_report(self, report_id, length):
                self.asserted = (report_id, length)
                return self.snapshot.encode()
            def send_feature_report(self, raw):
                self.writes += 1
                parsed = Snapshot.decode(raw)
                if not self.bad_readback:
                    self.snapshot = replace(parsed, revision=(parsed.revision+1) & 0xFFFF)
                return len(raw)
        device = Device()
        entries = (Entry(0x66, 1, 4, 8), *DEFAULTS[1:])
        with self.assertRaises(ValueError):
            save_settings(device, Snapshot(2, entries))
        self.assertEqual(device.writes, 0)
        saved = save_settings(device, Snapshot(3, entries, mouse_speed=7))
        self.assertEqual(saved, Snapshot(4, entries, mouse_speed=7))
        self.assertEqual(device.asserted, (REPORT_ID, 65))
        device.bad_readback = True
        with self.assertRaises(OSError):
            save_settings(device, Snapshot(4, DEFAULTS))
        device.bad_readback = False
        device.snapshot = Snapshot(65535, DEFAULTS)
        self.assertEqual(save_settings(device, Snapshot(65535, entries)).revision, 0)


if __name__ == "__main__":
    unittest.main()
