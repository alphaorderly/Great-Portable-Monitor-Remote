"""Enumerate and classify metadata only; never open a HID endpoint here."""
from dataclasses import dataclass
from datetime import datetime, timezone
import json
from pathlib import Path
import platform
import sys

import hid
from build_info import get_build_info
from protocol import DEVICE_NAME, USB_DEVICE_NAME, USAGE_PAGE, USAGE, is_bridge

NAMED = "named_bridge"
EXCLUDED = "excluded"


def classify(device):
    if device.get("usage_page") != USAGE_PAGE or device.get("usage") != USAGE:
        return EXCLUDED, "different_usage"
    if not isinstance(device.get("path"), bytes) or not device["path"]:
        return EXCLUDED, "missing_hid_path"
    if is_bridge(device):
        return NAMED, "matching_s3_usb_name"
    return EXCLUDED, "not_esp32s3_usb_bridge"


def device_label(device, index=1):
    product = device.get("product_string") or DEVICE_NAME
    name = f"{USB_DEVICE_NAME if product == USB_DEVICE_NAME else DEVICE_NAME} · USB"
    return f"{name} · {device.get('serial_number') or index}"


@dataclass(frozen=True)
class DiscoveryResult:
    records: tuple
    error: str | None
    scanned_at: str

    @property
    def devices(self):
        # A failed/partial enumeration must not offer an incomplete selection.
        if self.error:
            return []
        return [dict(r["metadata"], discovery_kind=r["classification"])
                for r in self.records if r["visible"]]

    @property
    def message(self):
        if self.error:
            return f"HID 검색 오류: {self.error} · 검색 진단을 저장해 확인하세요."
        if not self.devices:
            return "설정용 장치 후보가 없습니다. USB 연결 상태를 확인하거나 검색 진단을 저장하세요."
        return "기기를 선택하고 연결하세요."


def discover(enumerate_devices=None):
    records = []
    error = None
    try:
        for device in (enumerate_devices or hid.enumerate)():
            metadata = dict(device)
            kind, reason = classify(metadata)
            records.append({"metadata": metadata, "classification": kind,
                            "visible": kind != EXCLUDED, "reason": reason})
    except Exception as exc:
        error = f"{type(exc).__name__}: {exc}"
    return DiscoveryResult(tuple(records), error, datetime.now(timezone.utc).isoformat())


def _json_value(value):
    if isinstance(value, bytes):
        return {"text": value.decode("utf-8", errors="replace"), "hex": value.hex()}
    if isinstance(value, dict):
        return {key: _json_value(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_value(item) for item in value]
    return value


def diagnostic_report(result):
    try:
        native_version = hid.version_str()
    except Exception as exc:
        native_version = f"unavailable: {exc}"
    return {"schema_version": 1, "scanned_at": result.scanned_at,
            "app": get_build_info(),
            "runtime": {"os": platform.system(), "os_release": platform.release(),
                        "os_version": platform.version(), "cpu": platform.machine(),
                        "python": platform.python_version(), "frozen": bool(getattr(sys, "frozen", False)),
                        "hid_package_version": getattr(hid, "__version__", "unknown"),
                        "hidapi_version": native_version},
            "search_error": result.error, "raw_count": len(result.records),
            "candidate_count": len(result.devices), "devices": _json_value(result.records)}


def save_diagnostics(result, path):
    Path(path).write_text(json.dumps(diagnostic_report(result), ensure_ascii=False, indent=2) + "\n",
                          encoding="utf-8")
