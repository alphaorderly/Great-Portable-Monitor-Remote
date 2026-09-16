"""Discovery regressions without real devices or a Windows installation."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gui"))
from discovery import discover, save_diagnostics, device_label, NAMED, EXCLUDED
from connection import ConnectionState
from test_connection import Endpoint, device_info


def candidate(path=b"usb-candidate", **overrides):
    return dict(path=path, usage_page=0xff00, usage=1, product_string="ESP32-S3 Remote Bridge",
                manufacturer_string="Local Remote Bridge", bus_type=1) | overrides


class DiscoveryTests(unittest.TestCase):
    def test_metadata_matrix_and_usb_collision(self):
        cases = [
            (candidate(product_string="ESP32-S3 Remote Bridge"), NAMED),
            (candidate(product_string="esp32-s3 remote bridge"), NAMED),
            (candidate(manufacturer_string="Local Remote Bridge", bus_type=0), NAMED),
            (candidate(), NAMED),
            (candidate(bus_type=2), EXCLUDED),
            (candidate(product_string=None, manufacturer_string=None), EXCLUDED),
            (candidate(product_string="Bluetooth HID Device", manufacturer_string="Microsoft", bus_type=2), EXCLUDED),
            (candidate(product_string="Other USB", bus_type=1, vendor_id=0x046d, product_id=0xc548), EXCLUDED),
            (candidate(bus_type=0), NAMED),
            (candidate(bus_type=None), NAMED),
            (candidate(usage=2), EXCLUDED),
            (candidate(usage_page=1, usage=6, product_string="ESP32-S3 Remote Bridge"), EXCLUDED),
            (candidate(path=b""), EXCLUDED),
        ]
        inventory = [device for device, _ in cases]
        with patch("discovery.hid.device", side_effect=AssertionError("Search opened a device")):
            result = discover(lambda: inventory)
        self.assertIsNone(result.error)
        self.assertEqual([r["classification"] for r in result.records], [kind for _, kind in cases])
        self.assertEqual([r["metadata"] for r in result.records], inventory)
        self.assertEqual(len(result.devices), 6)
        self.assertTrue(all(r["reason"] for r in result.records))
        self.assertTrue(all("discovery_kind" not in d for d in inventory))

    def test_s3_usb_only_offers_vendor_collection_and_excludes_bluetooth(self):
        usb = candidate(path=b"usb-s3", product_string="ESP32-S3 Remote Bridge", bus_type=1, serial_number="S3-1")
        native = [usb | {"path": b"keyboard", "usage_page": 1, "usage": 6},
                  usb | {"path": b"mouse", "usage_page": 1, "usage": 2}]
        ble = candidate(path=b"ble", product_string="ESP32 Remote Bridge", bus_type=2)
        inventory = [usb, ble, *native, candidate(path=b"unknown-usb", product_string="", bus_type=1)]
        result = discover(lambda: inventory)
        self.assertEqual([d["path"] for d in result.devices], [b"usb-s3"])
        self.assertIn("ESP32-S3 Remote Bridge · USB", device_label(result.devices[0]))

    def test_empty_and_partial_error_are_distinct(self):
        empty = discover(lambda: [])
        self.assertIsNone(empty.error)
        self.assertIn("후보가 없습니다", empty.message)
        def broken():
            yield candidate()
            raise OSError("enumeration failed")
        result = discover(broken)
        self.assertIn("OSError", result.error)
        self.assertIn("검색 오류", result.message)
        self.assertEqual(len(result.records), 1)
        self.assertEqual(result.devices, [])

    def test_json_preserves_raw_bytes_and_exclusions(self):
        inventory = [candidate(path=b"path\xff"), candidate(path=b"usb", product_string="Other USB", bus_type=1)]
        result = discover(lambda: inventory)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "검색.json"
            save_diagnostics(result, path)
            report = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(report["raw_count"], 2)
        self.assertEqual(report["candidate_count"], 1)
        self.assertEqual(bytes.fromhex(report["devices"][0]["metadata"]["path"]["hex"]), b"path\xff")
        self.assertFalse(report["devices"][1]["visible"])
        self.assertEqual(report["runtime"]["hid_package_version"], "0.15.0")
        self.assertTrue(report["runtime"]["hidapi_version"])
        self.assertNotEqual(report["app"]["build_id"], "unknown")
        self.assertNotIn("settings", report)

    def test_cli_diagnostics_never_starts_gui_or_worker(self):
        from app import main
        for inventory, error, expected_code in [([], None, 0), ([candidate()], None, 0), ([], OSError("backend failed"), 1)]:
            with self.subTest(expected_code=expected_code), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "report.json"
                with patch.object(sys, "argv", ["RemoteKeyMapper", "--diagnostics", str(path)]), \
                     patch("discovery.hid.enumerate", return_value=inventory, side_effect=error), \
                     patch("discovery.hid.device", side_effect=AssertionError("No device I/O")), \
                     patch("app.QApplication", side_effect=AssertionError("No GUI")), \
                     patch("app.HidWorker", side_effect=AssertionError("No worker")):
                    self.assertEqual(main(), expected_code)
                report = json.loads(path.read_text(encoding="utf-8"))
                self.assertEqual(bool(report["search_error"]), bool(error))
                self.assertEqual(report["raw_count"], len(inventory))

    def test_cli_list_uses_candidate_classification(self):
        from app import main
        output = io.StringIO()
        with patch.object(sys, "argv", ["RemoteKeyMapper", "--list"]), \
             patch("discovery.hid.enumerate", return_value=[candidate(), candidate(path=b"usb", product_string="Other USB", bus_type=1)]), \
             patch("app.QApplication", side_effect=AssertionError("No GUI")), contextlib.redirect_stdout(output):
            self.assertEqual(main(), 0)
        self.assertIn("ESP32-S3 Remote Bridge · USB", output.getvalue())
        self.assertIn("usb-candidate", output.getvalue())
        self.assertNotIn("b'usb'", output.getvalue())


class CandidateGuiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        from app import KeyMapper
        self.window = KeyMapper(start_worker=False)
        self.endpoint = Endpoint()
        self.inventory = [device_info(), candidate()]
        self.worker = self.window.worker
        self.worker.enumerate_devices = lambda: self.inventory
        self.worker.session.device_factory = lambda: self.endpoint
        self.shared = patch("hid_session.configure_shared_access")
        self.shared.start()

    def tearDown(self):
        self.window.close()
        self.worker.session.close()
        self.shared.stop()

    def select_candidate(self):
        self.worker.command("scan")
        self.window.devices.setCurrentIndex(1)

    def connect_selected(self):
        self.window.connect_button.click()
        self.worker.command(*self.worker.commands.get_nowait())

    def test_search_does_not_open_and_selected_candidate_verifies_before_ready(self):
        self.select_candidate()
        self.assertEqual((self.endpoint.opens, self.endpoint.reads, self.endpoint.writes), (0, 0, 0))
        self.assertIn("ESP32-S3 Remote Bridge · USB", self.window.devices.currentText())
        self.assertFalse(self.window.apply_button.isEnabled())
        def during_read():
            self.assertFalse(self.window.connected)
            self.assertIn("ESP32-S3 Remote Bridge · USB", self.window.devices.currentText())
        self.endpoint.on_settings = during_read
        self.connect_selected()
        self.assertEqual(self.endpoint.path, b"usb-candidate")
        self.assertEqual((self.endpoint.opens, self.endpoint.reads, self.endpoint.writes), (1, 1, 0))
        self.assertTrue(self.window.connected)
        self.assertTrue(self.window.table.isEnabled())
        self.assertIn("ESP32-S3 Remote Bridge", self.window.devices.currentText())
        self.window.mouse_speed.setValue(8)
        self.assertTrue(self.window.apply_button.isEnabled())
        self.endpoint.on_settings = None
        self.window.apply_button.click()
        self.worker.command(*self.worker.commands.get_nowait())
        self.assertEqual(self.endpoint.writes, 1)
        self.assertEqual(self.endpoint.snapshot.mouse_speed, 8)
        self.assertIn("저장 완료", self.window.status.text())

    def test_invalid_protocol_closes_and_never_enables_edit_or_save(self):
        self.select_candidate()
        with patch.object(self.endpoint, "get_feature_report", return_value=bytes(65)):
            self.connect_selected()
        self.assertEqual(self.window.connection_state, ConnectionState.ERROR)
        self.assertIn("프로토콜 불일치", self.window.status.text())
        self.assertFalse(self.window.apply_button.isEnabled())
        self.assertEqual(self.endpoint.closes, 1)
        self.assertEqual(self.endpoint.writes, 0)
        self.assertIsNone(self.worker.retry.retry_at)

    def test_candidate_reconnect_preserves_draft_and_uses_new_verified_path(self):
        self.inventory[1]["serial_number"] = "S3-2"
        self.select_candidate()
        self.connect_selected()
        self.window.mouse_speed.setValue(8)
        self.endpoint.input_error = OSError("disconnected")
        self.worker.poll()
        self.assertIn("ESP32-S3 Remote Bridge · USB", self.window.devices.currentText())
        self.assertFalse(self.window.apply_button.isEnabled())
        self.endpoint.input_error = None
        self.inventory[1] = candidate(path=b"new-usb-path", serial_number="S3-2")
        self.worker.retry.retry_at = 0
        self.worker.poll()
        self.assertTrue(self.window.connected)
        self.assertEqual(self.window.devices.currentData(), b"new-usb-path")
        self.assertEqual(self.window.mouse_speed.value(), 8)
        self.assertTrue(self.window.dirty)
        self.assertEqual(self.endpoint.writes, 0)

    def test_usb_replug_changes_path_preserves_draft_and_never_repeats_save(self):
        self.inventory = [candidate(path=b"usb-s3", product_string="ESP32-S3 Remote Bridge", bus_type=1, serial_number="S3-1")]
        self.worker.command("scan")
        self.connect_selected()
        self.assertIn("ESP32-S3 Remote Bridge · USB", self.window.devices.currentText())
        self.window.mouse_speed.setValue(8)
        self.endpoint.input_error = OSError("USB unplugged")
        self.worker.poll()
        self.assertFalse(self.window.connected)
        self.inventory[0]["path"] = b"replugged-usb-s3"
        self.endpoint.input_error = None
        self.worker.retry.retry_at = 0
        self.worker.poll()
        self.assertTrue(self.window.connected)
        self.assertEqual(self.window.devices.currentData(), b"replugged-usb-s3")
        self.assertIn("ESP32-S3 Remote Bridge · USB", self.window.devices.currentText())
        self.assertEqual(self.window.mouse_speed.value(), 8)
        self.assertTrue(self.window.dirty)
        self.assertEqual(self.endpoint.writes, 0)

    def test_search_failure_keeps_diagnostic_and_disables_connect(self):
        def fail():
            raise OSError("backend failed")
        self.worker.enumerate_devices = fail
        self.worker.command("scan")
        self.assertIn("검색 오류", self.window.status.text())
        self.assertTrue(self.window.diagnostics_button.isEnabled())
        self.assertFalse(self.window.connect_button.isEnabled())
        self.assertIn("backend failed", self.window.last_discovery.error)

    def test_export_last_search_does_not_scan_or_change_connection(self):
        self.select_candidate()
        self.connect_selected()
        last = self.window.last_discovery
        revision = self.window.revision
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "gui-search.json"
            with patch("app.QFileDialog.getSaveFileName", return_value=(str(path), "JSON")), \
                 patch("app.QMessageBox.information"), \
                 patch.object(self.worker, "enumerate_devices", side_effect=AssertionError("No rescan")):
                self.window.export_diagnostics()
            report = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(report["scanned_at"], last.scanned_at)
        self.assertEqual(report["devices"][1]["classification"], NAMED)
        self.assertEqual(self.window.revision, revision)
        self.assertTrue(self.window.connected)

    def test_diagnostic_save_error_does_not_invalidate_mapping(self):
        self.select_candidate()
        self.connect_selected()
        revision = self.window.revision
        with patch("app.QFileDialog.getSaveFileName", return_value=("unused.json", "JSON")), \
             patch("app.save_diagnostics", side_effect=OSError("cannot write")), \
             patch("app.QMessageBox.warning") as warning:
            self.window.export_diagnostics()
        warning.assert_called_once()
        self.assertEqual(self.window.revision, revision)
        self.assertTrue(self.window.connected)
