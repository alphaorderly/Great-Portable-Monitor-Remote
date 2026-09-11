"""Shared HID access: the monitor must not seize native keyboard/mouse input."""
import ctypes
import sys

import hid


def configure_shared_access():
    if sys.platform != "darwin":
        return
    # cython-hidapi 0.15 embeds HIDAPI but does not wrap its public Darwin API.
    # Load the SAME extension so this changes the instance used by open_path.
    try:
        library = ctypes.CDLL(hid.__file__)
        initialize = library.hid_init
        initialize.argtypes = []
        initialize.restype = ctypes.c_int
        setter = library.hid_darwin_set_open_exclusive
        setter.argtypes = [ctypes.c_int]
        setter.restype = None
        getter = library.hid_darwin_get_open_exclusive
        getter.argtypes = []
        getter.restype = ctypes.c_int
        if initialize() != 0:
            raise RuntimeError("HIDAPI 초기화 실패")
        setter(0)
        if getter() != 0:
            raise RuntimeError("HID 공유 모드 설정 실패")
    except (AttributeError, OSError) as exc:
        raise RuntimeError("HID 공유 모드를 지원하는 프로젝트 의존성을 설치하거나 공식 배포 앱으로 실행해 주세요.") from exc


def is_native_report(data):
    # A composite IOHID device may deliver other collections to the same reader.
    # Leave native reports to the host OS and retain the existing GUI ID 1 protocol.
    return bool(data) and {2: 9, 3: 5, 4: 2}.get(data[0]) == len(data)
