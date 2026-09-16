"""Cross-language macro protocol checks against the actual firmware engine."""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'python/gui'))
from macros import Macro, Text, Wait, MacroProtocol, REPORT_ID, READ, COMMIT, slot_for


class EncodingTests(unittest.TestCase):
    def test_ascii_and_steps_roundtrip(self):
        macro = Macro(True, 3, 40, 80, 180, 1000, 2000,
                      (Text(''.join(chr(i) for i in range(32, 127)) + '\n\t'), Wait(500, 800), Text('done')))
        self.assertEqual(Macro.decode(macro.encode()), macro)
        self.assertEqual(len(Macro().encode()), 20)
        self.assertEqual(slot_for(0x28, 1), 22)
        for button, mode in ((0x6A, 0), (0x6A, 1), (0x4A, 1)):
            with self.assertRaises(ValueError):
                slot_for(button, mode)

    def test_reject_invalid_text_ranges_and_oversized_program(self):
        for macro in (Macro(True), Macro(True, steps=(Text('한글'),)), Macro(True, steps=(Text('\x00'),)),
                      Macro(steps=(Text(''),)), Macro(gap_min=100, gap_max=50),
                      Macro(steps=(Wait(1000, 100),)), Macro(steps=(Text('x'*500),)), Macro(repeats=101)):
            with self.subTest(macro=macro), self.assertRaises(ValueError):
                macro.encode()
        raw = Macro(True, steps=(Text('Hello'),)).encode()
        for n in range(len(raw)):
            with self.assertRaises(ValueError):
                Macro.decode(raw[:n])


@unittest.skipUnless(shutil.which('cc') and sys.platform in ('darwin', 'linux'), 'native C compiler required')
class FirmwareProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='remote-macro-test-')
        library = Path(cls.temp.name) / 'macro.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT/'firmware/main'),
                        str(ROOT/'firmware/main/macro.c'),
                        str(ROOT/'firmware/tests/native/macro_storage_stub.c'),
                        str(ROOT/'firmware/tests/native/macro_runtime_stub.c'), '-o', str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.macro_validate.restype = ctypes.c_bool

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.lib.macro_init()
        self.writes = []
        owner = self
        class Endpoint:
            def send_feature_report(self, packet):
                owner.writes.append(bytes(packet))
                assert len(packet) == 65 and packet[0] == REPORT_ID
                owner.lib.macro_request(ctypes.create_string_buffer(packet[1:]), 0)
                return 65
            def get_feature_report(self, report_id, length):
                assert report_id == REPORT_ID and length == 65
                reply = ctypes.create_string_buffer(64)
                owner.lib.macro_response(reply)
                return bytes((REPORT_ID,)) + reply.raw
        self.device = Endpoint()
        self.protocol = MacroProtocol(self.device)

    def test_full_ascii_chunk_upload_commit_and_persistence(self):
        revision, _ = self.protocol.read(8)
        desired = Macro(True, 2, 25, 50, 100, 500, 800,
                        (Text(''.join(chr(i) for i in range(32, 127))*3), Wait(700, 1000), Text('\tOK\n')))
        actual_revision, actual = self.protocol.save(8, revision, desired)
        self.assertEqual(actual, desired)
        self.assertEqual(actual_revision, (revision+1) % 65536)
        self.assertEqual(sum(p[4] == COMMIT for p in self.writes), 1)
        self.lib.macro_init()
        self.assertEqual(self.protocol.read(8), (actual_revision, desired))
        mask, active = self.protocol.status()
        self.assertTrue(mask & (1 << 8))
        self.assertEqual(active, 255)
        self.assertTrue(self.lib.macro_validate(ctypes.create_string_buffer(desired.encode()), len(desired.encode())))

    def test_stale_revision_and_interrupted_upload_never_replace_saved_data(self):
        revision, original = self.protocol.read(9)
        desired = Macro(True, steps=(Text('New value'*20),))
        with self.assertRaisesRegex(ValueError, '변경'):
            self.protocol.save(9, (revision+1) % 65536, desired)
        self.assertEqual(self.protocol.read(9), (revision, original))
        protocol = MacroProtocol(self.device, lambda: len(self.writes) >= 6)
        with self.assertRaises(OSError):
            protocol.save(9, revision, desired)
        self.assertEqual(self.protocol.read(9), (revision, original))

    def test_lost_commit_response_is_not_retried_and_can_be_read_back(self):
        revision, _ = self.protocol.read(10)
        desired = Macro(True, steps=(Text('Saved once'),))
        original_get = self.device.get_feature_report
        def get(report_id, length):
            if self.writes[-1][4] == COMMIT:
                raise OSError('USB disconnected after commit')
            return original_get(report_id, length)
        with patch.object(self.device, 'get_feature_report', side_effect=get), self.assertRaises(OSError):
            self.protocol.save(10, revision, desired)
        self.assertEqual(sum(p[4] == COMMIT for p in self.writes), 1)
        self.assertEqual(self.protocol.read(10), ((revision+1) % 65536, desired))

    def test_failed_storage_and_reserved_slots_do_not_commit(self):
        revision, original = self.protocol.read(11)
        fail = ctypes.c_bool.in_dll(self.lib, 'test_macro_storage_failure')
        fail.value = True
        try:
            with self.assertRaisesRegex(ValueError, '저장하지 못'):
                self.protocol.save(11, revision, Macro(True, steps=(Text('Not saved'),)))
        finally:
            fail.value = False
        self.assertEqual(self.protocol.read(11), (revision, original))
        for slot in (2, 16, 24):
            revision, _ = self.protocol.read(slot)
            with self.assertRaises(ValueError):
                self.protocol.save(slot, revision, Macro(True, steps=(Text('Reserved'),)))

    def test_changed_revision_mid_read_and_wrong_reply_are_rejected(self):
        revision, _ = self.protocol.read(12)
        self.protocol.save(12, revision, Macro(True, steps=(Text('x'*200),)))
        original_get = self.device.get_feature_report
        def get(report_id, length):
            reply = bytearray(original_get(report_id, length))
            if self.writes[-1][4] == READ and self.writes[-1][11]:
                reply[9] ^= 1
            return reply
        with patch.object(self.device, 'get_feature_report', side_effect=get), self.assertRaises(ValueError):
            self.protocol.read(12)
        with patch.object(self.device, 'get_feature_report', return_value=bytes(65)), self.assertRaises(ValueError):
            self.protocol.read(12)


class MacroGuiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def test_edit_validate_reorder_save_and_disconnect_preserves_draft(self):
        from macro_panel import MacroPanel
        panel = MacroPanel()
        panel.set_connection(True)
        panel.result('macro_read', panel.slot, (5, Macro()))
        panel.enabled.setChecked(True)
        self.assertFalse(panel.save_button.isEnabled())
        panel.add_step(Text('Hello'))
        panel.add_step(Wait(200, 300))
        panel.add_step(Text('World!'))
        panel.move_step(-1)
        self.assertEqual(panel.value().steps, (Text('Hello'), Text('World!'), Wait(200, 300)))
        requests = []
        panel.requested.connect(lambda cmd, data: requests.append((cmd, data)))
        panel.save_button.click()
        self.assertEqual(requests[0], ('macro_save', (8, 5, panel.value())))
        panel.set_connection(False)
        self.assertIsNone(panel.revision)
        self.assertTrue(panel.dirty)
        self.assertFalse(panel.save_button.isEnabled())
        panel.set_connection(True)
        self.assertFalse(panel.save_button.isEnabled())
        old = panel.value()
        panel.read()  # Reconnect reads the revision without destroying the draft.
        panel.result('macro_read', 8, (6, Macro()))
        self.assertEqual(panel.value(), old)
        self.assertTrue(panel.save_button.isEnabled())
        panel.target.setCurrentIndex(0)
        self.assertEqual(panel.slot, 0)
        panel.result('macro_read', 0, (0, Macro()))
        panel.target.setCurrentIndex(panel.target.findData(8))
        panel.result('macro_read', 8, (6, Macro()))
        self.assertEqual(panel.value(), old)
        panel.close()

    def test_connection_and_selection_read_automatically_without_duplicate_requests(self):
        from macro_panel import MacroPanel
        panel = MacroPanel()
        self.addCleanup(panel.close)
        requests = []
        panel.requested.connect(lambda *args: requests.append(args))
        panel.set_connection(True, busy=True)
        self.app.processEvents()
        self.assertEqual(requests, [])
        panel.set_connection(True)
        panel.set_connection(True)
        self.app.processEvents()
        self.assertEqual(requests, [('macro_read', 8)])
        self.assertFalse(panel.add_text.isEnabled())
        self.assertFalse(panel.save_button.isEnabled())
        self.assertIn('확인하는 중', panel.state.text())
        stored = Macro(True, steps=(Text('Stored on device'),))
        panel.result('macro_read', 8, (3, stored))
        self.assertEqual(panel.value(), stored)
        self.assertFalse(panel.dirty)
        panel.set_connection(True)
        self.app.processEvents()
        self.assertEqual(len(requests), 1)
        panel.target.setCurrentIndex(panel.target.findData(22))
        self.assertEqual(requests[-1], ('macro_read', 22))
        panel.result('macro_read', 22, (4, Macro()))
        self.assertEqual(panel.value(), Macro())

    def test_offline_and_multiple_slot_drafts_survive_reconnect(self):
        from macro_panel import MacroPanel
        panel = MacroPanel()
        self.addCleanup(panel.close)
        panel.add_step(Text('Offline draft'))
        first = panel.value()
        panel.target.setCurrentIndex(0)
        panel.add_step(Text('Second draft'))
        second = panel.value()
        panel.set_connection(True)
        self.app.processEvents()
        panel.result('macro_read', 0, (7, Macro(steps=(Text('Device value'),))))
        self.assertEqual(panel.value(), second)
        panel.set_connection(False)
        panel.set_connection(True)
        self.app.processEvents()
        panel.result('macro_read', 0, (8, Macro()))
        self.assertEqual(panel.value(), second)
        self.assertEqual(panel.revision, 8)
        panel.target.setCurrentIndex(panel.target.findData(8))
        panel.result('macro_read', 8, (9, Macro()))
        self.assertEqual(panel.value(), first)
        self.assertTrue(panel.dirty)
        self.assertTrue(panel.save_button.isEnabled())

    def test_read_failure_exposes_retry_and_does_not_loop_or_erase_draft(self):
        from macro_panel import MacroPanel
        panel = MacroPanel()
        self.addCleanup(panel.close)
        panel.add_step(Text('Keep this'))
        draft = panel.value()
        requests = []
        panel.requested.connect(lambda *args: requests.append(args))
        panel.set_connection(True)
        self.app.processEvents()
        panel.problem('Read failed')
        panel.set_connection(True)
        self.app.processEvents()
        self.assertEqual(requests, [('macro_read', 8)])
        self.assertEqual(panel.value(), draft)
        self.assertIn('Read failed', panel.state.text())
        self.assertFalse(panel.retry_button.isHidden())
        self.assertFalse(panel.save_button.isEnabled())
        panel.retry_button.click()
        self.assertEqual(len(requests), 2)
        panel.result('macro_read', 8, (2, Macro()))
        self.assertEqual(panel.value(), draft)
        self.assertTrue(panel.save_button.isEnabled())
        self.assertTrue(panel.retry_button.isHidden())

    def test_restore_requires_confirmation_and_only_replaces_after_success(self):
        from macro_panel import MacroPanel
        panel = MacroPanel()
        self.addCleanup(panel.close)
        panel.set_connection(True)
        self.app.processEvents()
        panel.result('macro_read', 8, (1, Macro()))
        panel.add_step(Text('Local edit'))
        draft = panel.value()
        requests = []
        panel.requested.connect(lambda *args: requests.append(args))
        with patch.object(panel, 'confirm_replace', return_value=False):
            panel.restore_button.click()
        self.assertEqual(requests, [])
        with patch.object(panel, 'confirm_replace', return_value=True):
            panel.restore_button.click()
        self.assertEqual(panel.value(), draft)
        panel.problem('Read failed')
        self.assertEqual(panel.value(), draft)
        with patch.object(panel, 'confirm_replace', return_value=True):
            panel.restore_button.click()
        stored = Macro(steps=(Text('Device version'),))
        panel.result('macro_read', 8, (2, stored))
        self.assertEqual(panel.value(), stored)
        self.assertFalse(panel.dirty)
        self.assertTrue(panel.restore_button.isHidden())

    def test_app_connection_queues_read_and_keeps_controls_busy_until_result(self):
        from app import KeyMapper
        from connection import ConnectionEvent, ConnectionState
        window = KeyMapper(start_worker=False)
        self.addCleanup(window.close)
        window.on_connection(ConnectionEvent(ConnectionState.READY, generation=1))
        self.app.processEvents()
        self.assertEqual(window.worker.commands.get_nowait(), ('macro_read', 8))
        self.assertTrue(window.busy)
        self.assertFalse(window.macro_panel.target.isEnabled())
        window.on_macro_result('macro_read', 8, (4, Macro(steps=(Text('Device'),))))
        self.assertFalse(window.busy)
        self.assertTrue(window.macro_panel.target.isEnabled())
        self.assertEqual(window.macro_panel.value().steps, (Text('Device'),))
        self.app.processEvents()
        self.assertTrue(window.worker.commands.empty())

    def test_generic_device_name_requires_our_manufacturer_and_config_usage(self):
        from discovery import classify, NAMED, EXCLUDED
        device = dict(path=b'config', product_string='USB Keyboard & Mouse',
                      manufacturer_string='Local Remote Bridge', usage_page=0xff00, usage=1, bus_type=1)
        self.assertEqual(classify(device)[0], NAMED)
        for changes in ({'manufacturer_string':'Other'}, {'usage_page':1}, {'bus_type':2}):
            self.assertEqual(classify(dict(device, **changes))[0], EXCLUDED)
