"""Exercise interactive flashing without opening or writing a serial device."""
import contextlib
import importlib.util
import io
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "flash", Path(__file__).resolve().parents[1] / "scripts/flash.py")
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


def port(device):
    return SimpleNamespace(device=device, description="USB UART", vid=0x10C4,
                           pid=0xEA60, serial_number="test-device")


class FlashMenuTests(unittest.TestCase):
    def run_menu(self, answers, scans, build_status=0):
        output = io.StringIO()
        with patch("builtins.input", side_effect=answers), \
                patch.object(flash, "discover_ports", side_effect=scans), \
                patch.object(flash.subprocess, "call", return_value=build_status) as build, \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            status = flash.main()
        return status, build, output.getvalue()

    def test_default_full_flash_uses_selected_port(self):
        ports = [port("COM3"), port("COM10")]
        status, build, _ = self.run_menu(["2", "", ""], [ports, ports])
        self.assertEqual(status, 0)
        self.assertEqual(build.call_args.args[0][2:], ["-B", "build", "-p", "COM10", "flash"])

    def test_app_flash_with_monitor_preserves_failure_status(self):
        ports = [port("/dev/cu.usbserial-test")]
        status, build, _ = self.run_menu(["1", "2", "2"], [ports, ports], build_status=7)
        self.assertEqual(status, 7)
        self.assertEqual(build.call_args.args[0][-3:], [ports[0].device, "app-flash", "monitor"])

    def test_refresh_after_connecting_and_invalid_selections(self):
        ports = [port("COM3")]
        status, build, output = self.run_menu(
            ["1", "r", "0", "9", "bad", "1", "3", "1", "bad", "1"],
            [[], ports, ports])
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
            with self.subTest(error=error):
                status, build, _ = self.run_menu([error], [[port("COM3")]])
                self.assertEqual(status, expected)
                build.assert_not_called()

    def test_disconnected_port_never_flashes(self):
        status, build, _ = self.run_menu(["1", "", ""], [[port("COM3")], []])
        self.assertEqual(status, 1)
        build.assert_not_called()

    def test_discovery_errors_never_flash(self):
        for error in (ImportError("pyserial"), OSError("enumeration failed")):
            with self.subTest(error=error):
                status, build, _ = self.run_menu([], [error])
                self.assertEqual(status, 1)
                build.assert_not_called()


if __name__ == "__main__":
    unittest.main()
