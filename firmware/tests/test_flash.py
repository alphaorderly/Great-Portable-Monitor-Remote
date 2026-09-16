"""Flash selection and ROM verification, without opening a serial device."""
import contextlib
import importlib.util
import io
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location("flash", Path(__file__).resolve().parents[1] / "scripts/flash.py")
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


def port(device):
    return SimpleNamespace(device=device, description="USB UART", vid=0x10C4,
                           pid=0xEA60, serial_number="test-device")


class FlashMenuTests(unittest.TestCase):
    def run_menu(self, answers, scans, build_status=0, chips=None):
        output = io.StringIO()
        with patch("builtins.input", side_effect=answers), \
                patch.object(flash, "discover_ports", side_effect=scans), \
                patch.object(flash, "detect_chip", side_effect=chips or ["esp32s3", "esp32s3"]), \
                patch.object(flash.subprocess, "call", return_value=build_status) as build, \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            status = flash.main()
        return status, build, output.getvalue()

    def test_default_full_flash_uses_selected_port(self):
        ports = [port("COM3"), port("COM10")]
        status, build, _ = self.run_menu(["2", "", ""], [ports, ports])
        self.assertEqual(status, 0)
        self.assertEqual(build.call_args.args[0][2:], ["-B", "build", "-p", "COM10", "flash"])

    def test_s3_app_flash_with_monitor_preserves_failure_status(self):
        ports = [port("/dev/cu.usbserial-test")]
        status, build, output = self.run_menu(["1", "2", "2"], [ports, ports],
                                             build_status=7, chips=["esp32s3", "esp32s3"])
        self.assertEqual(status, 7)
        self.assertEqual(build.call_args.args[0][2:], ["-B", "build", "-p", ports[0].device, "app-flash", "monitor"])
        self.assertIn("UART/COM", output)

    def test_mismatch_unsupported_or_failed_probe_never_builds(self):
        for chip in ("esp32", "esp32c3", "esp32s2", OSError("probe failed")):
            status, build, output = self.run_menu(["1", "1"], [[port("COM3")]], chips=[chip])
            self.assertEqual(status, 1)
            build.assert_not_called()
            self.assertIn("실패", output)

    def test_port_replaced_during_menu_never_builds(self):
        ports = [port("COM3")]
        status, build, _ = self.run_menu(["1", "", ""], [ports, ports], chips=["esp32s3", "esp32"])
        self.assertEqual(status, 1)
        build.assert_not_called()

    def test_refresh_and_invalid_selections(self):
        ports = [port("COM3")]
        status, build, output = self.run_menu(
            ["bad", "1", "r", "0", "9", "bad", "1", "3", "1", "bad", "1"], [[], ports, ports])
        self.assertEqual(status, 0)
        build.assert_called_once()
        self.assertIn("포트를 찾지 못했습니다", output)

    def test_quit_at_each_menu_never_flashes(self):
        for answers in (["q"], ["1", "q"], ["1", "1", "q"]):
            with self.subTest(answers=answers):
                status, build, _ = self.run_menu(answers, [[port("COM3")]])
                self.assertEqual(status, 0)
                build.assert_not_called()

    def test_eof_and_interrupt_never_flash(self):
        for error, expected in ((EOFError(), 0), (KeyboardInterrupt(), 130)):
            status, build, _ = self.run_menu([error], [[port("COM3")]])
            self.assertEqual(status, expected)
            build.assert_not_called()

    def test_disconnected_port_never_flashes(self):
        status, build, _ = self.run_menu(["1", "", ""], [[port("COM3")], []])
        self.assertEqual(status, 1)
        build.assert_not_called()

    def test_discovery_errors_never_flash(self):
        for error in (ImportError("pyserial"), OSError("enumeration failed")):
            status, build, _ = self.run_menu(["1"], [error])
            self.assertEqual(status, 1)
            build.assert_not_called()

    def test_probe_uses_esptool_chip_name_and_always_closes_port(self):
        for name, target in (("ESP32", "esp32"), ("ESP32-S3", "esp32s3"), (None, None)):
            connection = Mock()
            esptool = SimpleNamespace(detect_chip=Mock(return_value=SimpleNamespace(CHIP_NAME=name)))
            serial = SimpleNamespace(serial_for_url=Mock(return_value=connection))
            if name is None:
                esptool.detect_chip.side_effect = OSError("sync failed")
            with patch.dict("sys.modules", {"esptool": esptool, "serial": serial}):
                if name:
                    self.assertEqual(flash.detect_chip("COM3"), target)
                else:
                    with self.assertRaises(OSError):
                        flash.detect_chip("COM3")
            esptool.detect_chip.assert_called_once_with(port=connection, baud=115200)
            connection.close.assert_called_once()
            self.assertFalse(connection.dtr)
            self.assertFalse(connection.rts)


if __name__ == "__main__":
    unittest.main()
