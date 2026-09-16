"""Remote Key Mapper: edit and persist mappings on the ESP32-S3 over HID."""
import argparse
import logging
from pathlib import Path
import sys

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QFileDialog, QHBoxLayout, QHeaderView, QLabel, QMessageBox,
    QMainWindow, QPushButton, QSlider, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget, QTabWidget,
    QFrame, QGridLayout, QMenu, QStackedWidget, QTabBar, QWidgetAction,
)
from mapping import ACTIONS, defaults_for_host, Entry, KEY, Snapshot, SPEED_DEFAULT, SPEED_MAX
from platform_support import MODIFIERS, default_host
from protocol import BUTTONS
from discovery import discover, device_label, save_diagnostics
from worker import HidWorker
from connection import ConnectionEvent, ConnectionState
from debug_panel import DebugPanel
from macro_panel import MacroPanel

STYLE = """
QMainWindow, QWidget#root { background: #f5f5f5; }
QWidget { color: #252525; font-size: 13px; }
QLabel { background: transparent; }
QLabel#title { font-size: 17px; font-weight: 600; }
QLabel#sectionTitle { font-size: 20px; font-weight: 600; }
QLabel#muted, QLabel#status { color: #686868; }
QLabel#connectionState { color: #555555; font-size: 12px; }
QLabel#preview { background: #f6f6f6; border: 1px solid #e4e4e4; border-radius: 4px; padding: 16px; font-size: 20px; font-weight: 500; }
QFrame#editor { background: white; border: 1px solid #dedede; border-radius: 4px; }
QPushButton { background: #ffffff; border: 1px solid #cfcfcf; border-radius: 4px; padding: 6px 12px; min-height: 18px; }
QPushButton:hover { background: #f0f0f0; border-color: #aaaaaa; }
QPushButton:pressed { background: #e5e5e5; }
QPushButton:focus, QComboBox:focus { border: 1px solid #2868c7; }
QPushButton:disabled { color: #999999; background: #f3f3f3; border-color: #dddddd; }
QPushButton#apply { background: #2868c7; color: white; border-color: #2868c7; font-weight: 600; }
QPushButton#apply:hover { background: #215bad; }
QPushButton#apply:disabled { background: #ededed; color: #999999; border-color: #d9d9d9; }
QComboBox { background: white; border: 1px solid #cfcfcf; border-radius: 4px; padding: 6px 10px; min-height: 18px; }
QComboBox:disabled { color: #888888; background: #f5f5f5; }
QSpinBox { background: white; border: 1px solid #cfcfcf; border-radius: 4px; padding: 5px 8px; min-height: 18px; }
QPlainTextEdit { background: white; color: #252525; border: 1px solid #cfcfcf; border-radius: 4px; padding: 8px; }
QPlainTextEdit:focus, QSpinBox:focus { border: 1px solid #2868c7; }
QSpinBox:disabled { background: #f5f5f5; color: #888888; }
QComboBox QAbstractItemView { background: white; color: #252525; selection-background-color: #e8eff9; selection-color: #1b4d91; }
QTabWidget::pane { border: none; border-top: 1px solid #d8d8d8; top: -1px; }
QTabBar::tab { background: transparent; color: #666666; padding: 10px 16px; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: #252525; border-bottom: 2px solid #2868c7; }
QTabBar::tab:hover { color: #252525; background: #eeeeee; }
QTabBar#modeTabs::tab { padding: 7px 16px; background: #eeeeee; border: 1px solid #d6d6d6; color: #666666; }
QTabBar#modeTabs::tab:selected { background: white; color: #252525; font-weight: 600; }
QTableWidget { background: white; alternate-background-color: #fafafa; border: 1px solid #dedede; border-radius: 3px; gridline-color: #eeeeee; selection-background-color: #e8eff9; selection-color: #1b4d91; }
QTableWidget::item { padding: 4px 10px; border: none; }
QTableWidget::item:selected { background: #e8eff9; color: #1b4d91; }
QHeaderView::section { background: #f6f6f6; color: #666666; border: none; border-bottom: 1px solid #dedede; padding: 8px 10px; font-weight: 500; }
QCheckBox { spacing: 7px; }
QCheckBox::indicator { width: 16px; height: 16px; }
QCheckBox:disabled { color: #999999; }
QMenu { background: white; color: #252525; border: 1px solid #d6d6d6; padding: 5px; }
QMenu::item { padding: 7px 20px; }
QMenu::item:selected { background: #e8eff9; color: #1b4d91; }
QMenu::item:disabled { color: #999999; }
QToolTip { background: #ffffff; color: #252525; border: 1px solid #cccccc; padding: 5px; }
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
        self.setWindowTitle("Remote Key Mapper — ESP32-S3")
        self.resize(1000, 790)
        self.setMinimumSize(880, 700)
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
        self.macro_mask = 0
        self.last_discovery = None
        self.worker = HidWorker()
        self.worker.devices.connect(self.on_devices)
        self.worker.discovery.connect(self.on_discovery)
        self.worker.connection.connect(self.on_connection)
        self.worker.settings.connect(self.on_settings)
        self.worker.problem.connect(self.on_problem)

        root = QWidget(objectName="root")
        self.setCentralWidget(root)
        outer_layout = QVBoxLayout(root)
        outer_layout.setContentsMargins(24, 18, 24, 16)
        outer_layout.setSpacing(14)
        heading = QHBoxLayout()
        heading.addWidget(label("리모컨 설정", "title"))
        heading.addStretch()
        self.connection_label = label("연결 안 됨", "connectionState")
        heading.addWidget(self.connection_label)
        outer_layout.addLayout(heading)

        connection = QHBoxLayout()
        connection.setSpacing(8)
        connection.addWidget(QLabel("장치"))
        self.devices = QComboBox()
        self.devices.setAccessibleName("연결할 장치")
        self.devices.addItem("기기를 찾아 연결하세요", None)
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
        self.device_menu = QMenu(self)
        self.diagnostics_button = self.device_menu.addAction("검색 진단 저장…")
        self.diagnostics_button.triggered.connect(self.export_diagnostics)
        device_options = QPushButton("장치 메뉴")
        device_options.setMenu(self.device_menu)
        connection.addWidget(device_options)
        outer_layout.addLayout(connection)

        self.tabs = QTabWidget()
        outer_layout.addWidget(self.tabs, 1)
        mapping_page = QWidget()
        self.tabs.addTab(mapping_page, "버튼 설정")
        layout = QVBoxLayout(mapping_page)
        layout.setContentsMargins(0, 16, 0, 0)
        layout.setSpacing(12)

        mode_row = QHBoxLayout()
        self.mapping_mode = QTabBar(objectName="modeTabs")
        self.mapping_mode.addTab("일반 모드")
        self.mapping_mode.addTab("마우스 모드")
        self.mapping_mode.setAccessibleName("편집할 모드")
        self.mapping_mode.setToolTip("편집할 설정을 선택합니다. 리모컨의 실제 모드는 바뀌지 않습니다.")
        self.mapping_mode.currentChanged.connect(self.switch_mapping_mode)
        mode_row.addWidget(self.mapping_mode)
        mode_row.addStretch()
        self.settings_menu = QMenu(self)
        self.read_button = self.settings_menu.addAction("장치에서 읽기")
        self.read_button.triggered.connect(self.read_device)
        self.settings_menu.addSeparator()
        preset_widget = QWidget()
        preset_layout = QVBoxLayout(preset_widget)
        preset_layout.setContentsMargins(12, 8, 12, 10)
        preset_layout.addWidget(QLabel("현재 모드의 기본값 불러오기"))
        self.host_preset = QComboBox()
        self.host_preset.setAccessibleName("기본값 운영체제")
        for name, value in (("macOS", "macos"), ("Windows", "windows"), ("Linux", "linux")):
            self.host_preset.addItem(name, value)
        self.host_preset.setCurrentIndex(self.host_preset.findData(self.host))
        self.host_preset.setToolTip("기본값을 불러올 때만 사용하는 운영체제입니다.")
        preset_layout.addWidget(self.host_preset)
        self.defaults_button = QPushButton("기본값 불러오기")
        self.defaults_button.clicked.connect(self.load_defaults)
        self.defaults_button.clicked.connect(self.settings_menu.close)
        preset_layout.addWidget(self.defaults_button)
        preset_action = QWidgetAction(self.settings_menu)
        preset_action.setDefaultWidget(preset_widget)
        self.settings_menu.addAction(preset_action)
        settings_options = QPushButton("설정 메뉴")
        settings_options.setMenu(self.settings_menu)
        mode_row.addWidget(settings_options)
        layout.addLayout(mode_row)

        content = QHBoxLayout()
        content.setSpacing(16)
        self.table = QTableWidget(len(BUTTONS), 2)
        self.table.setAccessibleName("리모컨 버튼과 지정된 동작")
        self.table.setHorizontalHeaderLabels(["버튼", "지정된 동작"])
        self.table.verticalHeader().hide()
        self.table.setShowGrid(False)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.table.setSelectionMode(QTableWidget.SelectionMode.SingleSelection)
        self.table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.table.setWordWrap(False)
        header = self.table.horizontalHeader()
        header.setDefaultAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Fixed)
        self.table.setColumnWidth(0, 120)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        content.addWidget(self.table, 5)

        self.editor = QFrame(objectName="editor")
        editor_layout = QVBoxLayout(self.editor)
        editor_layout.setContentsMargins(24, 24, 24, 20)
        editor_layout.setSpacing(16)
        self.editor_title = label("", "sectionTitle")
        editor_layout.addWidget(self.editor_title)
        self.editor_hint = label("", "muted")
        editor_layout.addWidget(self.editor_hint)
        self.action_preview = label("", "preview")
        editor_layout.addWidget(self.action_preview)
        self.edit_macro_button = QPushButton("문자열 매크로 편집…")
        self.edit_macro_button.clicked.connect(self.open_button_macro)
        editor_layout.addWidget(self.edit_macro_button, alignment=Qt.AlignmentFlag.AlignLeft)
        self.editor_stack = QStackedWidget()
        self.modifier_groups = {}
        self.action_groups = {}
        modifier_names = {1: "Control", 2: "Shift", 4: "Option / Alt", 8: "Command / Win"} if self.host == "macos" else {1: "Ctrl", 2: "Shift", 4: "Alt / Option", 8: "Win / Command"}
        for row, (button, name) in enumerate(BUTTONS.items()):
            self.table.setRowHeight(row, 33)
            self.table.setItem(row, 0, QTableWidgetItem(name))
            self.table.setItem(row, 1, QTableWidgetItem())
            page = QWidget()
            page_layout = QVBoxLayout(page)
            page_layout.setContentsMargins(0, 0, 0, 0)
            page_layout.setSpacing(16)
            action_group = QWidget()
            action_layout = QVBoxLayout(action_group)
            action_layout.setContentsMargins(0, 0, 0, 0)
            action_layout.setSpacing(8)
            action_layout.addWidget(QLabel("실행할 동작"))
            combo = QComboBox()
            combo.setAccessibleName(f"{name}에 지정할 동작")
            combo.setMaxVisibleItems(16)
            for title, kind, key in ACTIONS:
                combo.addItem(title, (kind << 8) | key)
            action_layout.addWidget(combo)
            page_layout.addWidget(action_group)
            self.action_groups[button] = action_group
            modifier_group = QWidget()
            modifiers_layout = QVBoxLayout(modifier_group)
            modifiers_layout.setContentsMargins(0, 0, 0, 0)
            modifiers_layout.setSpacing(12)
            modifiers_layout.addWidget(QLabel("함께 누를 키"))
            checks_layout = QGridLayout()
            checks_layout.setHorizontalSpacing(16)
            checks_layout.setVerticalSpacing(12)
            checks = {}
            for index, (bit, title) in enumerate(MODIFIERS):
                checkbox = QCheckBox(modifier_names[bit])
                checkbox.setAccessibleName(f"{name} {title}")
                checks_layout.addWidget(checkbox, index // 2, index % 2)
                checks[bit] = checkbox
                checkbox.toggled.connect(self.edited)
            modifiers_layout.addLayout(checks_layout)
            page_layout.addWidget(modifier_group)
            self.modifier_groups[button] = modifier_group
            page_layout.addStretch()
            self.editor_stack.addWidget(page)
            self.controls[button] = (combo, checks)
            if button == 0x6A:
                combo.clear()
                combo.addItem("커서 모드 전환 전용 (키 입력 없음)", 0)
                combo.setEnabled(False)
            combo.currentIndexChanged.connect(lambda index, code=button: self.action_changed(code))
        editor_layout.addWidget(self.editor_stack, 1)

        self.mouse_settings = QWidget()
        speed_layout = QVBoxLayout(self.mouse_settings)
        speed_layout.setContentsMargins(0, 12, 0, 0)
        speed_layout.setSpacing(10)
        speed_row = QHBoxLayout()
        speed_row.addWidget(QLabel("마우스 속도"))
        speed_row.addStretch()
        self.speed_label = QLabel("1배")
        speed_row.addWidget(self.speed_label)
        speed_layout.addLayout(speed_row)
        self.mouse_speed = QSlider(Qt.Orientation.Horizontal)
        self.mouse_speed.setRange(1, SPEED_MAX)
        self.mouse_speed.setValue(SPEED_DEFAULT)
        self.mouse_speed.setAccessibleName("마우스 속도")
        self.mouse_speed.setToolTip("0.25배~3배 · 기본 1배 · 장치에 저장하면 반영됩니다.")
        self.mouse_speed.valueChanged.connect(self.speed_changed)
        speed_layout.addWidget(self.mouse_speed)
        speed_layout.addWidget(label("모든 버튼에 공통으로 적용됩니다. 휠 속도는 유지됩니다.", "muted"))
        self.mouse_settings.setVisible(False)
        editor_layout.addWidget(self.mouse_settings)
        editor_layout.addWidget(label("변경한 설정은 ‘장치에 저장’을 누르면 적용됩니다.", "muted"))
        content.addWidget(self.editor, 5)
        layout.addLayout(content, 1)
        self.table.currentCellChanged.connect(self.select_button)

        actions = QHBoxLayout()
        self.change_label = label("변경 없음", "muted")
        actions.addWidget(self.change_label)
        actions.addStretch()
        actions.addWidget(label("두 모드를 함께 저장합니다", "muted"))
        self.apply_button = QPushButton("장치에 저장", objectName="apply")
        self.apply_button.clicked.connect(self.apply_settings)
        actions.addWidget(self.apply_button)
        layout.addLayout(actions)
        self.macro_panel = MacroPanel()
        self.tabs.addTab(self.macro_panel, "문자열 매크로")
        self.macro_panel.requested.connect(self.request_macro)
        self.macro_panel.stop_requested.connect(self.worker.request_macro_stop)
        self.worker.macro_result.connect(self.on_macro_result)
        self.worker.macro_problem.connect(self.on_macro_problem)
        self.worker.macro_overrides.connect(self.on_macro_overrides)
        self.debug_panel = DebugPanel()
        self.tabs.addTab(self.debug_panel, "입력 확인")
        self.debug_panel.changed.connect(lambda mode: self.worker.commands.put(("debug", mode)))
        self.worker.debug_reports.connect(self.debug_panel.on_batch)
        self.status = label("장치를 연결하면 저장된 설정을 불러옵니다. 연결 전에도 버튼을 편집할 수 있습니다.", "status")
        outer_layout.addWidget(self.status)
        self.populate(self.drafts[0])
        self.table.setCurrentCell(8, 0)
        self.devices.currentIndexChanged.connect(self.refresh_enabled)
        self.refresh_enabled()
        if start_worker:
            self.worker.runner.start()
            QTimer.singleShot(100, self.scan_devices)

    def on_macro_overrides(self, mask):
        self.macro_mask = mask
        self.refresh_summaries()

    def open_button_macro(self):
        slot = self.edit_mode * len(BUTTONS) + self.table.currentRow()
        index = self.macro_panel.target.findData(slot)
        if index >= 0:
            self.macro_panel.target.setCurrentIndex(index)
            if self.macro_panel.slot == slot:
                self.tabs.setCurrentWidget(self.macro_panel)

    def request_macro(self, command, payload):
        self.busy = True
        self.refresh_enabled()
        self.macro_panel.state.setText("장치의 매크로를 읽는 중…" if command == "macro_read" else "매크로 저장 중…")
        self.worker.commands.put((command, payload))

    def on_macro_result(self, command, slot, payload):
        self.busy = False
        self.macro_panel.result(command, slot, payload)
        self.refresh_enabled()

    def on_macro_problem(self, message):
        self.busy = False
        self.macro_panel.problem(message)
        self.refresh_enabled()

    def action_summary(self, button):
        if button == 0x6A:
            return "모드 전환 · 고정"
        if self.edit_mode == 1 and button == 0x4A:
            return "커서 일시 정지 · 고정"
        if self.macro_mask & (1 << (self.edit_mode * len(BUTTONS) + tuple(BUTTONS).index(button))):
            return "문자열 매크로"
        combo, checks = self.controls[button]
        names = {1: "⌃", 2: "⇧", 4: "⌥", 8: "⌘"} if self.host == "macos" else {1: "Ctrl", 2: "Shift", 4: "Alt", 8: "Win"}
        modifiers = [names[bit] for bit, box in checks.items() if box.isChecked()]
        return " + ".join([*modifiers, combo.currentText()])

    def refresh_summaries(self):
        for row, button in enumerate(BUTTONS):
            summary = self.action_summary(button)
            self.table.item(row, 1).setText(summary)
            self.table.item(row, 1).setToolTip(summary)
            reserved = button == 0x6A or (self.edit_mode == 1 and button == 0x4A)
            overridden = bool(self.macro_mask & (1 << (self.edit_mode * len(BUTTONS) + row)))
            self.action_groups[button].setVisible(not reserved and not overridden)
            self.modifier_groups[button].setVisible(not reserved and not overridden and self.controls[button][0].currentData() >> 8 == KEY)
        self.select_button(self.table.currentRow())

    def select_button(self, row, *_):
        if row < 0:
            return
        button = tuple(BUTTONS)[row]
        self.editor_stack.setCurrentIndex(row)
        self.editor_title.setText(BUTTONS[button])
        self.edit_macro_button.setVisible(button != 0x6A and not (self.edit_mode == 1 and button == 0x4A))
        self.action_preview.setText(self.action_summary(button))
        if button == 0x6A:
            hint = "일반 모드와 마우스 모드를 전환하는 버튼입니다. 동작을 변경할 수 없습니다."
        elif self.edit_mode == 1 and button == 0x4A:
            hint = "누르는 동안 커서를 멈춰 손 위치를 조정합니다. 이 동작은 변경할 수 없습니다."
        elif self.macro_mask & (1 << (self.edit_mode * len(BUTTONS) + row)):
            hint = "장치에 저장된 문자열 매크로를 실행합니다. 기본 키 동작을 사용하려면 매크로 사용을 해제하고 저장하세요."
        else:
            mode = "일반 모드" if self.edit_mode == 0 else "마우스 모드"
            hint = f"{mode}에서 이 버튼을 눌렀을 때 실행할 동작입니다."
        self.editor_hint.setText(hint)

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
        self.refresh_summaries()

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
        self.refresh_summaries()
        self.dirty = self.profiles() != self.baseline or self.mouse_speed.value() != self.baseline_speed
        self.change_label.setText("저장하지 않은 변경 있음" if self.dirty else "변경 없음")
        self.refresh_enabled()

    def speed_changed(self, value):
        self.speed_label.setText(f"{value / SPEED_DEFAULT:g}배")
        self.edited()

    def refresh_enabled(self):
        editable = not self.busy
        self.connect_button.setEnabled(not self.busy and not self.connected and self.connection_state != ConnectionState.RETRY_WAIT and bool(self.devices.currentData()))
        self.disconnect_button.setEnabled(self.connection_state in (
            ConnectionState.CONNECTING, ConnectionState.READING, ConnectionState.READY, ConnectionState.RETRY_WAIT))
        self.devices.setEnabled(not self.busy and not self.connected and self.connection_state != ConnectionState.RETRY_WAIT)
        self.scan.setEnabled(not self.busy and not self.connected and self.connection_state != ConnectionState.RETRY_WAIT)
        self.diagnostics_button.setEnabled(self.last_discovery is not None)
        active_connection = self.connection_state in (
            ConnectionState.CONNECTING, ConnectionState.READING, ConnectionState.READY, ConnectionState.RETRY_WAIT)
        self.disconnect_button.setVisible(active_connection)
        self.connect_button.setVisible(not active_connection)
        self.connection_label.setText({
            ConnectionState.DISCONNECTED: "연결 안 됨",
            ConnectionState.CONNECTING: "연결 중…",
            ConnectionState.READING: "설정 읽는 중…",
            ConnectionState.READY: "연결됨 · USB",
            ConnectionState.RETRY_WAIT: "다시 연결하는 중…",
            ConnectionState.ERROR: "연결 확인 필요",
        }[self.connection_state])
        self.macro_panel.set_connection(self.connected, self.busy)
        self.editor.setEnabled(editable)
        self.table.setEnabled(editable)
        self.mapping_mode.setEnabled(editable)
        self.mouse_speed.setEnabled(editable)
        self.host_preset.setEnabled(editable)
        self.defaults_button.setEnabled(editable)
        self.read_button.setEnabled(self.connected and not self.busy)
        self.apply_button.setEnabled(self.connected and self.revision is not None and self.dirty and not self.busy)

    def scan_devices(self):
        self.busy = True
        self.status.setText("ESP32-S3 HID를 찾는 중…")
        self.refresh_enabled()
        self.worker.commands.put(("scan", None))

    def on_discovery(self, result):
        self.last_discovery = result
        self.diagnostics_button.setEnabled(True)

    def export_diagnostics(self):
        if self.last_discovery is None:
            return
        # Capture the latest completed search, even if a retry finishes while
        # the native file dialog's nested event loop is running.
        result = self.last_discovery
        filename, _ = QFileDialog.getSaveFileName(self, "검색 진단 저장", "remote-hid-diagnostics.json", "JSON (*.json)")
        if not filename:
            return
        try:
            save_diagnostics(result, filename)
        except (OSError, ValueError) as exc:
            QMessageBox.warning(self, "검색 진단 저장 실패", str(exc))
        else:
            QMessageBox.information(self, "검색 진단 저장", "마지막 검색 결과를 저장했습니다.")

    def on_devices(self, devices):
        self.devices.clear()
        for index, device in enumerate(devices, 1):
            title = device_label(device, index)
            self.devices.addItem(title, device["path"])
            self.devices.setItemData(index - 1, device.get("discovery_kind"), Qt.ItemDataRole.UserRole + 1)
            self.devices.setItemData(index - 1, title, Qt.ItemDataRole.UserRole + 2)
            self.devices.setItemData(index - 1, device, Qt.ItemDataRole.UserRole + 3)
        if not devices:
            self.devices.addItem("설정용 장치 후보가 없습니다", None)
        self.busy = False
        self.status.setText((self.last_discovery or discover(lambda: devices)).message)
        self.refresh_enabled()

    def connect_device(self):
        path = self.devices.currentData()
        if path:
            self.disconnect_requested = False
            self.busy = True
            self.status.setText("선택한 장치에 연결하고 ESP32-S3 설정 형식을 확인하는 중…")
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
        if self.connected and event.device_path is not None:
            index = self.devices.currentIndex()
            if index >= 0:
                self.devices.setItemData(index, event.device_path)
                metadata = dict(self.devices.itemData(index, Qt.ItemDataRole.UserRole + 3) or {})
                metadata.update(serial_number=event.device_serial, discovery_kind="named_bridge")
                self.devices.setItemText(index, device_label(metadata, index + 1))
        self.debug_panel.set_connected(self.connected)
        if not self.connected:
            self.revision = None
        if event.state == ConnectionState.DISCONNECTED:
            self.disconnect_requested = False
        self.busy = event.state in (ConnectionState.CONNECTING, ConnectionState.READING)
        self.status.setText(event.reason or {
            ConnectionState.DISCONNECTED: "연결 해제됨 · 편집 중인 매핑은 화면에 남아 있습니다.",
            ConnectionState.CONNECTING: "ESP32-S3 HID에 연결하는 중…",
            ConnectionState.READING: "저장된 매핑을 읽는 중…",
            ConnectionState.READY: "설정 읽기 완료 · HID 통신 사용 가능",
            ConnectionState.RETRY_WAIT: "ESP32-S3에 다시 연결하는 중…",
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
            "ESP32-S3 저장 완료 · 장치에서 다시 읽어 매핑이 일치함을 확인했습니다." if operation == "apply" else
            "장치 설정을 읽었습니다. 연결 전에 편집한 내용은 유지했습니다." if preserve_draft else
            "장치에 저장된 매핑을 읽었습니다. 버튼을 편집한 뒤 ‘장치에 저장’을 누르세요."
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
        self.status.setText("기본값을 편집 화면에 불러왔습니다. 장치에 반영하려면 ‘장치에 저장’을 누르세요.")

    def apply_settings(self):
        if self.revision is None:
            return
        snapshot = Snapshot(self.revision, *self.profiles(), mouse_speed=self.mouse_speed.value())
        snapshot.encode()
        self.busy = True
        self.status.setText("ESP32-S3에 매핑 저장 중… 저장 후 장치의 설정을 다시 읽어 확인합니다.")
        self.refresh_enabled()
        self.worker.commands.put(("apply", snapshot))

    def closeEvent(self, event):
        self.worker.stop()
        event.accept()


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s: %(message)s")
    parser = argparse.ArgumentParser(description="ESP32-S3 리모컨 키 매핑 설정")
    parser.add_argument("--screenshot", type=Path, help="장치를 열지 않고 설정 화면 PNG 저장")
    parser.add_argument("--list", action="store_true", help="ESP32-S3 설정용 HID 목록")
    parser.add_argument("--debug", action="store_true", help="입력 확인 탭에서 기록을 켜고 시작")
    parser.add_argument("--self-test", action="store_true", help="하드웨어 없이 패키지·Qt·HIDAPI 실행 확인 후 종료")
    parser.add_argument("--diagnostics", type=Path, help="GUI 없이 HID 검색 진단 JSON을 지정한 경로에 저장")
    args = parser.parse_args()
    if sum(bool(value) for value in (args.diagnostics, args.list, args.self_test, args.screenshot)) > 1:
        parser.error("--diagnostics, --list, --self-test, --screenshot 중 하나만 지정하세요.")
    if args.diagnostics:
        result = discover()
        try:
            save_diagnostics(result, args.diagnostics)
        except OSError as exc:
            logging.getLogger("keymapper").error("검색 진단 파일 저장 실패: %s", exc)
            return 2
        return 1 if result.error else 0
    if args.list:
        result = discover()
        for index, device in enumerate(result.devices, 1):
            print({"label": device_label(device, index), **{key: device.get(key) for key in
                   ("product_string", "usage_page", "usage", "path", "discovery_kind")}})
        print(result.message)
        return 1 if result.error else 0
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
