"""Own one HID handle. Called exclusively on the worker thread."""
import logging
from mapping import read_settings, save_settings
from transport import configure_shared_access

LOG = logging.getLogger("keymapper")


class HidSession:
    def __init__(self, device_factory):
        self.device_factory = device_factory
        self.device = None

    def open(self, path):
        self.close()
        candidate = self.device_factory()
        try:
            configure_shared_access()
            candidate.open_path(path)
        except Exception:
            candidate.close()
            raise
        self.device = candidate

    def close(self):
        device, self.device = self.device, None
        if device is not None:
            try:
                device.close()
            except OSError:
                LOG.warning("HID handle close failed", exc_info=True)

    def read_settings(self):
        return read_settings(self.device)

    def save(self, snapshot):
        return save_settings(self.device, snapshot)

    def read_input(self):
        return self.device.read(128, 100)
