"""Verify monitor-only behavior without opening real hardware."""
import contextlib
import importlib.util
import io
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("monitor", Path(__file__).resolve().parents[1]/"scripts/monitor.py")
monitor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(monitor)


class MonitorTests(unittest.TestCase):
    def test_inactive_lines_before_open_no_uart_writes_and_log_append(self):
        encoded = "연결됨\n".encode()
        steps = []
        class Device:
            def __setattr__(self, name, value):
                steps.append((name, value))
                object.__setattr__(self, name, value)
            def open(self):
                steps.append(("open", self.port))
                self.chunks = iter([encoded[:2], b"", encoded[2:]])
            def read(self, size):
                assert size == 4096
                try:
                    return next(self.chunks)
                except StopIteration:
                    raise KeyboardInterrupt
            def write(self, data):
                raise AssertionError("Monitor must not transmit")
            def close(self):
                steps.append(("close", None))
        def make(**kwargs):
            self.assertIsNone(kwargs["port"])
            self.assertEqual(kwargs["baudrate"], 115200)
            self.assertFalse(kwargs["rtscts"])
            self.assertFalse(kwargs["dsrdtr"])
            self.assertFalse(kwargs["xonxoff"])
            return Device()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"serial.log"
            path.write_bytes(b"previous\n")
            output = io.StringIO()
            with contextlib.redirect_stdout(output), self.assertRaises(KeyboardInterrupt):
                monitor.receive(SimpleNamespace(Serial=make), "COM3", 115200, path)
            self.assertEqual(path.read_bytes(), b"previous\n"+encoded)
            self.assertIn("연결됨", output.getvalue())
            self.assertEqual(steps[:4], [("dtr", False), ("rts", False), ("port", "COM3"), ("open", "COM3")])
            self.assertEqual(steps[-1], ("close", None))

    def test_open_failure_closes_handle(self):
        from unittest.mock import Mock
        device = Mock()
        device.open.side_effect = OSError("port busy")
        with self.assertRaises(OSError):
            monitor.receive(SimpleNamespace(Serial=lambda **kwargs: device), "COM3", 115200)
        device.close.assert_called_once()
        device.read.assert_not_called()
        device.write.assert_not_called()

    def test_refresh_selection_and_quit(self):
        ports = SimpleNamespace(comports=lambda: [SimpleNamespace(device="COM3", description="USB UART")])
        with patch("builtins.input", side_effect=["bad", "r", "1"]), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(monitor.choose_port(ports), "COM3")
        with patch("builtins.input", return_value="q"), self.assertRaises(EOFError), contextlib.redirect_stdout(io.StringIO()):
            monitor.choose_port(ports)

    def test_cli_passes_options_and_handles_disconnect(self):
        # Installed pyserial is not needed for testing argument/error behavior.
        import sys
        fake = {"serial": SimpleNamespace(), "serial.tools": SimpleNamespace(list_ports=object())}
        with patch.dict(sys.modules, fake), patch.object(monitor, "receive", side_effect=OSError("unplugged")) as receive, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(monitor.main(["--port", "COM10", "--baud", "9600"]), 1)
            self.assertEqual(receive.call_args.args[1:], ("COM10", 9600, None))

    def test_platform_launcher_help_from_another_directory(self):
        import subprocess
        import sys
        root = Path(__file__).resolve().parents[2]
        if sys.platform == "win32":
            command = ["cmd", "/d", "/c", str(root/"monitor-windows.cmd"), "--help"]
        elif sys.platform == "darwin":
            command = ["bash", str(root/"monitor-macos.sh"), "--help"]
        else:
            self.skipTest("macOS and Windows launchers")
        with tempfile.TemporaryDirectory(prefix="monitor help ") as tmp:
            result = subprocess.run(command, cwd=tmp, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--port", result.stdout)
        self.assertIn("--log", result.stdout)

    def test_help_and_invalid_baud_do_not_open_device(self):
        with patch.object(monitor, "receive") as receive, contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            for arguments, status in [(["--help"], 0), (["--baud", "0"], 2)]:
                with self.assertRaises(SystemExit) as exc:
                    monitor.main(arguments)
                self.assertEqual(exc.exception.code, status)
            receive.assert_not_called()
