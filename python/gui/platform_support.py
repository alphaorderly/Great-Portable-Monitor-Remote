"""Host presentation and explicit presets; HID modifier bits never change."""
import sys

# USB HID: Control=1, Shift=2, Alt=4, GUI=8. Command is NOT Control.
MODIFIERS = ((1, "Ctrl / Control"), (2, "Shift"),
             (4, "Alt / Option"), (8, "Win / Command"))
MODIFIER_NAMES = ("Ctrl / Control", "Shift", "Alt / Option", "Win / Command")


def default_host(platform=None):
    platform = sys.platform if platform is None else platform
    return "macos" if platform == "darwin" else "windows" if platform == "win32" else "linux"


def debug_note(platform=None):
    platform = sys.platform if platform is None else platform
    if platform == "win32":
        return ("Windows에서는 리모컨 원본을 기록합니다. OS가 키보드·마우스 컬렉션을 별도로 관리하므로 "
                "실제 출력 기록은 이 연결에 전달되지 않을 수 있습니다. 실제 키 입력은 계속 동작합니다.")
    return ("리모컨 원본은 매핑 전 버튼입니다. 호스트 키보드·마우스·볼륨은 이 HID 연결에 "
            "전달되는 경우에만 기록합니다. 실제 키 입력은 계속 동작합니다.")
