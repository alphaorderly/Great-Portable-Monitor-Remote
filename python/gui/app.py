"""Remote Key Mapper: edit and persist mappings on the ESP32 over HID."""
import argparse
import logging
from pathlib import Path
import sys

import hid
from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QHBoxLayout, QHeaderView, QLabel,
    QMainWindow, QPushButton, QSlider, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget, QTabWidget,
)
from mapping import ACTIONS, defaults_for_host, Entry, KEY, Snapshot, SPEED_DEFAULT, SPEED_MAX
from platform_support import MODIFIERS, default_host
from protocol import BUTTONS, DEVICE_NAME, is_bridge
from worker import HidWorker
from connection import ConnectionEvent, ConnectionState
from debug_panel import DebugPanel

STYLE = """
QMainWindow { background: #f3f5f9; }
QWidget { color: #1e2b40; font-size: 13px; }
QLabel#eyebrow { color: #53758c; font-size: 11px; font-weight: 600; }
QLabel#title { font-size: 28px; font-weight: 700; }
QLabel#muted { color: #67778a; }
QLabel#status { background: #e5edf5; border-radius: 10px; padding: 12px; }
QPushButton { background: white; border: 1px solid #d5deea; border-radius: 8px; padding: 9px 15px; }
QPushButton:hover { background: #eaf1fa; }
QPushButton:disabled { color: #a4afbd; background: #f1f3f6; }
QPushButton#apply { background: #176b55; color: white; border: none; font-weight: 600; }
QPushButton#apply:disabled { background: #b4c9c1; }
QComboBox { background: white; border: 1px solid #d5deea; border-radius: 6px; padding: 5px 10px; }
QTableWidget { background: white; alternate-background-color: #f8fafc; border: 1px solid #dde4ed; border-radius: 10px; gridline-color: #edf1f5; }
QHeaderView::section { background: #eaf0f6; border: none; padding: 10px 6px; font-weight: 600; }
QCheckBox::indicator { width: 18px; height: 18px; }
"""


def label(text, name=None):
    result = QLabel(text)
    if name:
        result.setObjectName(name)
    result.setWordWrap(True)
    return result


class KeyMapper(QMainWindow):
    def __init__(self, start_worker=True, host_platform=None):
        super().__init__()
        self.setWindowTitle("Remote Key Mapper — ESP32")
        self.resize(1060, 900)
        self.setMinimumSize(960, 720)
        self.connected = False
        self.connection_state = ConnectionState.DISCONNECTED
        self.connection_generation = 0
        self.disconnect_requested = False
        self.busy = False
        self.revision = None
        self.host = default_host(host_platform)
        self.baseline = defaults_for_host(self.host)
        self.baseline_speed = SPEED_DEFAULT
        self.drafts = list(self.baseline)
        self.edit_mode = 0
        self.dirty = False
        self.loading = False
        self.controls = {}
        self.worker = HidWorker()
        self.worker.devices.connect(self.on_devices)
        self.worker.connection.connect(self.on_connection)
        self.worker.settings.connect(self.on_settings)
        self.worker.problem.connect(self.on_problem)

        root = QWidget()
        self.setCentralWidget(root)
        layout = QVBoxLayout(root)
        layout.setContentsMargins(28, 22, 28, 22)
        layout.setSpacing(12)
        layout.addWidget(label("REMOTE CONFIGURATION", "eyebrow"))
        layout.addWidget(label("Remote Key Mapper", "title"))
        layout.addWidget(label("리모컨 버튼에 사용할 키와 조합 키를 선택하세요. ESP32에 저장하면 프로그램을 종료해도 유지됩니다.", "muted"))

        connection = QHBoxLayout()
        self.devices = QComboBox()
        self.devices.addItem("Bluetooth에서 ESP32를 연결하세요", None)
        connection.addWidget(self.devices, 1)
        self.scan = QPushButton("기기 찾기")
        self.scan.clicked.connect(self.scan_devices)
        connection.addWidget(self.scan)
        self.connect_button = QPushButton("연결")
        self.connect_button.clicked.connect(self.connect_device)
        connection.addWidget(self.connect_button)
        self.disconnect_button = QPushButton("연결 해제")
        self.disconnect_button.clicked.connect(self.disconnect_device)
        connection.addWidget(self.disconnect_button)
        layout.addLayout(connection)

        outer_layout = layout
        self.tabs = QTabWidget()
        outer_layout.addWidget(self.tabs, 1)
        mapping_page = QWidget()
        self.tabs.addTab(mapping_page, "키 매핑")
        layout = QVBoxLayout(mapping_page)
        layout.setContentsMargins(0, 6, 0, 0)

        self.mapping_mode = QComboBox()
        self.mapping_mode.addItems(["일반 모드 키 할당", "마우스 커서 모드 키 할당"])
        self.mapping_mode.currentIndexChanged.connect(self.switch_mapping_mode)
        mode_row = QHBoxLayout()
        mode_row.addWidget(self.mapping_mode, 1)
        mode_row.addWidget(QLabel("기본값 대상"))
        self.host_preset = QComboBox()
        for name, value in (("macOS", "macos"), ("Windows", "windows"), ("Linux", "linux")):
            self.host_preset.addItem(name, value)
        self.host_preset.setCurrentIndex(self.host_preset.findData(self.host))
        self.host_preset.setToolTip("‘현재 모드 기본값’을 누를 때만 사용합니다. 저장된 매핑은 자동 변환하지 않습니다.")
        mode_row.addWidget(self.host_preset)
        layout.addLayout(mode_row)

        self.mouse_settings = QWidget()
        speed_layout = QVBoxLayout(self.mouse_settings)
        speed_layout.setContentsMargins(4, 0, 4, 0)
        speed_row = QHBoxLayout()
        speed_row.addWidget(QLabel("마우스 속도"))
        speed_row.addWidget(QLabel("느리게"))
        self.mouse_speed = QSlider(Qt.Orientation.Horizontal)
        self.mouse_speed.setRange(1, SPEED_MAX)
        self.mouse_speed.setValue(SPEED_DEFAULT)
        self.mouse_speed.setAccessibleName("마우스 속도")
        self.mouse_speed.setToolTip("0.25배~3배 · 기본 1배 · ESP32에 적용·저장을 누르면 반영됩니다.")
        speed_row.addWidget(self.mouse_speed, 1)
        speed_row.addWidget(QLabel("빠르게"))
        self.speed_label = QLabel("1배")
        self.speed_label.setMinimumWidth(52)
        speed_row.addWidget(self.speed_label)
        speed_layout.addLayout(speed_row)
        speed_layout.addWidget(label("홈 버튼은 손 위치 조정 전용입니다. 누르는 동안 커서가 멈추며, 그동안의 움직임은 버립니다.", "muted"))
        self.mouse_speed.valueChanged.connect(self.speed_changed)
        self.mouse_settings.setVisible(False)
        layout.addWidget(self.mouse_settings)

        self.table = QTableWidget(len(BUTTONS), 6)
        self.table.setHorizontalHeaderLabels(["리모컨 버튼", "할당할 동작", *[name for _, name in MODIFIERS]])
        self.table.verticalHeader().hide()
        self.table.setAlternatingRowColors(True)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.table.setSelectionMode(QTableWidget.SelectionMode.NoSelection)
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Fixed)
        self.table.setColumnWidth(0, 144)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        for col in range(2, 6):
            header.setSectionResizeMode(col, QHeaderView.ResizeMode.Fixed)
            self.table.setColumnWidth(col, 122)
        for row, (button, name) in enumerate(BUTTONS.items()):
            self.table.setRowHeight(row, 37)
            self.table.setItem(row, 0, QTableWidgetItem("  " + name))
            combo = QComboBox()
            for title, kind, key in ACTIONS:
                combo.addItem(title, (kind << 8) | key)
            self.table.setCellWidget(row, 1, combo)
            checks = {}
            for col, (bit, title) in enumerate(MODIFIERS, 2):
                checkbox = QCheckBox()
                checkbox.setAccessibleName(f"{name} {title}")
                container = QWidget()
                center = QHBoxLayout(container)
                center.setContentsMargins(0, 0, 0, 0)
                center.addWidget(checkbox, alignment=Qt.AlignmentFlag.AlignCenter)
                self.table.setCellWidget(row, col, container)
                checks[bit] = checkbox
                checkbox.toggled.connect(self.edited)
            self.controls[button] = (combo, checks)
            if button == 0x6A:
                combo.clear()
                combo.addItem("커서 모드 전환 전용 (키 입력 없음)", 0)
                combo.setEnabled(False)
            combo.currentIndexChanged.connect(lambda index, code=button: self.action_changed(code))
        layout.addWidget(self.table, 1)

        layout.addWidget(label("예: 일반 모드 왼쪽 → ← 왼쪽 / 마우스 모드 왼쪽 → 마우스 오른쪽 클릭\n위 선택은 편집할 설정입니다. 실제 적용 모드는 ESP32가 센서 신호로 구분하며, 저장 시 두 모드가 함께 저장됩니다.", "muted"))
        actions = QHBoxLayout()
        self.defaults_button = QPushButton("현재 모드 기본값")
        self.defaults_button.clicked.connect(self.load_defaults)
        actions.addWidget(self.defaults_button)
        self.read_button = QPushButton("장치에서 읽기")
        self.read_button.clicked.connect(self.read_device)
        actions.addWidget(self.read_button)
        actions.addStretch()
        self.change_label = label("기본 매핑", "muted")
        actions.addWidget(self.change_label)
        self.apply_button = QPushButton("ESP32에 적용·저장", objectName="apply")
        self.apply_button.clicked.connect(self.apply_settings)
        actions.addWidget(self.apply_button)
        layout.addLayout(actions)
        self.debug_panel = DebugPanel()
        self.tabs.addTab(self.debug_panel, "입력 디버깅")
        self.debug_panel.changed.connect(lambda mode: self.worker.commands.put(("debug", mode)))
        self.worker.debug_reports.connect(self.debug_panel.on_batch)
        self.status = label("기기를 연결하면 저장된 매핑을 읽습니다. 처음 사용 시 키 매핑용 펌웨어를 먼저 플래시하세요.", "status")
        outer_layout.addWidget(self.status)
        self.populate(self.drafts[0])
        self.refresh_enabled()
        if start_worker:
            self.worker.runner.start()
            QTimer.singleShot(100, self.scan_devices)

    def entries(self):
        result = []
        for button, (combo, checks) in self.controls.items():
            value = combo.currentData()
            kind, key = value >> 8, value & 0xFF
            mods = sum(bit for bit, box in checks.items() if box.isChecked()) if kind == KEY else 0
            result.append(Entry(button, kind, key, mods))
        return tuple(result)

    def profiles(self):
        self.drafts[self.edit_mode] = self.entries()
        return tuple(self.drafts)

    def switch_mapping_mode(self, mode):
        self.drafts[self.edit_mode] = self.entries()
        self.edit_mode = mode
        self.mouse_settings.setVisible(mode == 1)
        self.populate(self.drafts[mode])
        self.edited()

    def populate(self, entries):
        self.loading = True
        try:
            for entry in entries:
                combo, checks = self.controls[entry.button]
                reserved = entry.button == 0x6A or (self.edit_mode == 1 and entry.button == 0x4A)
                combo.setEnabled(not reserved)
                if entry.button == 0x4A:
                    combo.setItemText(combo.findData(0), "누르는 동안 커서 정지 (키 입력 없음)" if reserved else "동작 없음")
                value = 0 if reserved else (entry.kind << 8) | entry.key
                index = combo.findData(value)
                if index < 0:
                    combo.addItem(f"HID key 0x{entry.key:02X}", value)
                    index = combo.count() - 1
                combo.setCurrentIndex(index)
                for bit, box in checks.items():
                    box.setChecked(not reserved and bool(entry.modifiers & bit))
                    box.setEnabled(not reserved and entry.kind == KEY)
        finally:
            self.loading = False

    def action_changed(self, button):
        if self.loading:
            return
        combo, checks = self.controls[button]
        is_key = combo.currentData() >> 8 == KEY
        for box in checks.values():
            box.setEnabled(is_key)
            if not is_key:
                box.setChecked(False)
        self.edited()

    def edited(self, *args):
        if self.loading:
            return
        self.dirty = self.profiles() != self.baseline or self.mouse_speed.value() != self.baseline_speed
        self.change_label.setText("저장하지 않은 변경 있음" if self.dirty else "변경 없음")
        self.refresh_enabled()

    def speed_changed(self, value):
        self.speed_label.setText(f"{value / SPEED_DEFAULT:g}배")
        self.edited()

    def refresh_enabled(self):
        self.connect_button.setEnabled(not self.busy and not self.connected and self.connection_state != ConnectionState.RETRY_WAIT and bool(self.devices.currentData()))
        self.disconnect_button.setEnabled(self.connection_state in (
            ConnectionState.CONNECTING, ConnectionState.READING, ConnectionState.READY, ConnectionState.RETRY_WAIT))
        self.devices.setEnabled(not self.busy and not self.connected and self.connection_state != ConnectionState.RETRY_WAIT)
        self.scan.setEnabled(not self.busy and not self.connected and self.connection_state != ConnectionState.RETRY_WAIT)
        self.table.setEnabled(not self.busy)
        self.mapping_mode.setEnabled(not self.busy)
        self.mouse_speed.setEnabled(not self.busy)
        self.host_preset.setEnabled(not self.busy)
        self.defaults_button.setEnabled(not self.busy)
        self.read_button.setEnabled(self.connected and not self.busy)
        self.apply_button.setEnabled(self.connected and self.revision is not None and self.dirty and not self.busy)

    def scan_devices(self):
        self.busy = True
        self.status.setText("ESP32 HID를 찾는 중…")
        self.refresh_enabled()
        self.worker.commands.put(("scan", None))

    def on_devices(self, devices):
        self.devices.clear()
        for index, device in enumerate(devices, 1):
            self.devices.addItem(f"{DEVICE_NAME} · {device.get('serial_number') or index}", device["path"])
        if not devices:
            self.devices.addItem("ESP32 HID를 찾지 못했습니다", None)
        self.busy = False
        self.status.setText("기기를 선택하고 연결하세요." if devices else "운영체제의 Bluetooth 설정에서 ESP32를 연결한 뒤 다시 검색하세요.")
        self.refresh_enabled()

    def connect_device(self):
        path = self.devices.currentData()
        if path:
            self.disconnect_requested = False
            self.busy = True
            self.status.setText("ESP32에 연결하고 저장된 매핑을 읽는 중…")
            self.refresh_enabled()
            self.worker.request_connect(path)

    def disconnect_device(self):
        self.busy = True
        self.refresh_enabled()
        self.disconnect_requested = True
        self.worker.request_disconnect()

    def on_connection(self, event: ConnectionEvent):
        if event.generation < self.connection_generation:
            return
        if self.disconnect_requested and event.state != ConnectionState.DISCONNECTED:
            return
        self.connection_generation = event.generation
        self.connection_state = event.state
        self.connected = event.state == ConnectionState.READY
        self.debug_panel.set_connected(self.connected)
        if not self.connected:
            self.revision = None
        if event.state == ConnectionState.DISCONNECTED:
            self.disconnect_requested = False
        self.busy = event.state in (ConnectionState.CONNECTING, ConnectionState.READING)
        self.status.setText(event.reason or {
            ConnectionState.DISCONNECTED: "연결 해제됨 · 편집 중인 매핑은 화면에 남아 있습니다.",
            ConnectionState.CONNECTING: "ESP32 HID에 연결하는 중…",
            ConnectionState.READING: "저장된 매핑을 읽는 중…",
            ConnectionState.READY: "설정 읽기 완료 · HID 통신 사용 가능",
            ConnectionState.RETRY_WAIT: "ESP32에 다시 연결하는 중…",
            ConnectionState.ERROR: "연결 오류 · 기기와 앱·펌웨어 버전을 확인하세요.",
        }[event.state])
        self.refresh_enabled()

    def on_settings(self, snapshot, operation):
        if self.disconnect_requested:
            return
        preserve_draft = operation in ("connect", "reconnect", "recover_save") and self.dirty
        self.revision = snapshot.revision
        self.baseline = (snapshot.entries, snapshot.mouse_entries)
        self.baseline_speed = snapshot.mouse_speed
        if not preserve_draft:
            self.drafts = list(self.baseline)
            self.populate(self.drafts[self.edit_mode])
            self.loading = True
            self.mouse_speed.setValue(snapshot.mouse_speed)
            self.loading = False
        self.busy = False
        self.edited()
        self.status.setText(
            "ESP32 저장 완료 · 장치에서 다시 읽어 매핑이 일치함을 확인했습니다." if operation == "apply" else
            "장치 설정을 읽었습니다. 연결 전에 편집한 내용은 유지했습니다." if preserve_draft else
            "장치에 저장된 매핑을 읽었습니다. 버튼별 키를 수정하고 적용·저장을 누르세요."
        )

    def on_problem(self, message):
        self.busy = False
        self.revision = None  # A failed write can have an unknown outcome; require a fresh read.
        self.status.setText(f"{message} · 사용 가능한 연결에서 ‘장치에서 읽기’로 설정을 확인하세요.")
        self.refresh_enabled()

    def read_device(self):
        self.busy = True
        self.status.setText("장치에 저장된 매핑을 읽는 중…")
        self.refresh_enabled()
        self.worker.commands.put(("read", None))

    def load_defaults(self):
        self.populate(defaults_for_host(self.host_preset.currentData())[self.edit_mode])
        if self.edit_mode == 1:
            self.mouse_speed.setValue(SPEED_DEFAULT)
        self.edited()
        self.status.setText("기본값을 편집 화면에 불러왔습니다. 장치에 반영하려면 적용·저장을 누르세요.")

    def apply_settings(self):
        if self.revision is None:
            return
        snapshot = Snapshot(self.revision, *self.profiles(), mouse_speed=self.mouse_speed.value())
        snapshot.encode()
        self.busy = True
        self.status.setText("ESP32에 매핑 저장 중… 저장 후 장치의 설정을 다시 읽어 확인합니다.")
        self.refresh_enabled()
        self.worker.commands.put(("apply", snapshot))

    def closeEvent(self, event):
        self.worker.stop()
        event.accept()


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s: %(message)s")
    parser = argparse.ArgumentParser(description="ESP32 리모컨 키 매핑 설정")
    parser.add_argument("--screenshot", type=Path, help="장치를 열지 않고 설정 화면 PNG 저장")
    parser.add_argument("--list", action="store_true", help="ESP32 설정용 HID 목록")
    parser.add_argument("--debug", action="store_true", help="입력 디버깅 탭에서 기록을 켜고 시작")
    parser.add_argument("--self-test", action="store_true", help="하드웨어 없이 패키지·Qt·HIDAPI 실행 확인 후 종료")
    args = parser.parse_args()
    if args.list:
        matches = [d for d in hid.enumerate() if is_bridge(d)]
        for device in matches:
            print({key: device.get(key) for key in ("product_string", "usage_page", "usage", "path")})
        if not matches:
            print("ESP32 HID가 없습니다. 운영체제의 Bluetooth 설정에서 먼저 연결하세요.")
        return 0
    app = QApplication(sys.argv[:1])
    app.setApplicationName("Remote Key Mapper")
    # Let Qt select the system font and its Korean fallback on every platform.
    app.setStyleSheet(STYLE)
    window = KeyMapper(start_worker=not (args.screenshot or args.self_test))
    if args.debug:
        window.tabs.setCurrentWidget(window.debug_panel)
        window.debug_panel.enabled.setChecked(True)
    window.show()
    if args.self_test:
        from transport import configure_shared_access
        configure_shared_access()
        Snapshot(0, *defaults_for_host(default_host())).encode()
        QTimer.singleShot(100, window.close)
    if args.screenshot:
        def capture():
            args.screenshot.parent.mkdir(parents=True, exist_ok=True)
            ok = window.grab().save(str(args.screenshot))
            window.close()
            app.exit(0 if ok else 1)
        QTimer.singleShot(300, capture)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
