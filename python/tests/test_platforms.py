"""Cross-host shortcuts and transport boundaries, without real HID devices."""
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gui"))
from mapping import DEFAULTS, MOUSE_DEFAULTS, Snapshot, defaults_for_host
from platform_support import MODIFIERS, default_host, debug_note


class PlatformTests(unittest.TestCase):
    def test_host_presets_only_change_app_switch(self):
        for platform, host, bit in (("darwin", "macos", 8), ("win32", "windows", 4), ("linux", "linux", 4)):
            with self.subTest(host=host):
                self.assertEqual(default_host(platform), host)
                profiles = defaults_for_host(host)
                self.assertEqual(Snapshot.decode(Snapshot(23, *profiles).encode()), Snapshot(23, *profiles))
                for actual, original in zip(profiles, (DEFAULTS, MOUSE_DEFAULTS)):
                    for a, b in zip(actual, original):
                        if a.button == 0x76:
                            self.assertEqual((a.key, a.modifiers), (0x2B, bit))
                        else:
                            self.assertEqual(a, b)
        self.assertEqual(DEFAULTS[11].modifiers, 8)

    def test_windows_and_linux_never_load_darwin_library(self):
        from transport import configure_shared_access
        for platform in ("win32", "linux"):
            with patch("transport.sys.platform", platform), patch("transport.ctypes.CDLL") as library:
                configure_shared_access()
                library.assert_not_called()

    def test_all_hid_modifiers_are_distinct_on_every_host(self):
        self.assertEqual({bit for bit, _ in MODIFIERS}, {1, 2, 4, 8})
        from dataclasses import replace
        from debugging import DebugDecoder
        for modifiers in range(16):
            entries = (replace(DEFAULTS[0], modifiers=modifiers), *DEFAULTS[1:])
            self.assertEqual(Snapshot.decode(Snapshot(0, entries).encode()).entries[0].modifiers, modifiers)
        events = DebugDecoder().consume(bytes([2, 9, 0, 4, 0, 0, 0, 0, 0]), 0)
        names = {e.name for e in events}
        self.assertIn("왼쪽 Ctrl / Control (0xE0)", names)
        self.assertIn("왼쪽 Win / Command (0xE3)", names)

    def test_windows_gui_preserves_device_mapping_until_explicit_reset(self):
        from PySide6.QtWidgets import QApplication
        from app import KeyMapper
        app = QApplication.instance() or QApplication([])
        window = KeyMapper(start_worker=False, host_platform="win32")
        try:
            self.assertEqual(window.entries()[11].modifiers, 4)
            window.on_connection(True)
            window.on_settings(Snapshot(7, DEFAULTS, MOUSE_DEFAULTS), "connect")
            self.assertEqual(window.entries()[11].modifiers, 8)
            self.assertFalse(window.dirty)
            window.load_defaults()
            self.assertEqual(window.entries()[11].modifiers, 4)
            self.assertEqual(window.profiles()[1], MOUSE_DEFAULTS)
            self.assertTrue(window.dirty)
            self.assertTrue(window.worker.commands.empty())
            window.host_preset.setCurrentIndex(window.host_preset.findData("macos"))
            self.assertEqual(window.entries()[11].modifiers, 4)
            self.assertIn("Windows", debug_note("win32"))
        finally:
            window.close()


if __name__ == "__main__":
    unittest.main()
