"""An opt-in, bounded live view of ESP32 button recognition in two test modes."""
from datetime import datetime
from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QCheckBox, QComboBox, QLabel,
    QPushButton, QTableWidget, QTableWidgetItem, QHeaderView, QSplitter,
)
from debugging import MODES
from platform_support import debug_note


class DebugPanel(QWidget):
    changed = Signal(object)
    MAX_ROWS = 300

    def __init__(self):
        super().__init__()
        self.connected = False
        self.counts = {}
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 6, 0, 0)
        controls = QHBoxLayout()
        self.enabled = QCheckBox("디버깅 모드 · 입력 기록")
        controls.addWidget(self.enabled)
        controls.addStretch()
        controls.addWidget(QLabel("테스트할 모드"))
        self.mode = QComboBox()
        self.mode.addItems(MODES)
        controls.addWidget(self.mode)
        self.clear_button = QPushButton("기록 지우기")
        controls.addWidget(self.clear_button)
        layout.addLayout(controls)
        note = QLabel("리모컨의 실제 모드를 맞춘 뒤 위에서 같은 모드를 선택하세요. 모드 자동 감지는 지원하지 않습니다.\n"
                      + debug_note())
        note.setWordWrap(True)
        note.setObjectName("muted")
        layout.addWidget(note)
        self.latest = QLabel("디버깅 모드를 켜고 기기를 연결하세요.")
        self.latest.setWordWrap(True)
        self.latest.setObjectName("status")
        layout.addWidget(self.latest)
        self.stats = QLabel("수신 0 · 원본 순번 누락 0 · 해석 제외 0 · 표시 누락 0")
        self.stats.setObjectName("muted")
        layout.addWidget(self.stats)
        split = QSplitter(Qt.Orientation.Vertical)
        comparison = QWidget()
        comparison_layout = QVBoxLayout(comparison)
        comparison_layout.setContentsMargins(0, 0, 0, 0)
        comparison_layout.addWidget(QLabel("모드별 인식 비교 · 누름/초기 상태 인식 횟수"))
        self.comparison = QTableWidget(0, 3)
        self.comparison.setHorizontalHeaderLabels(["인식한 입력", "일반 모드", "마우스 커서 모드"])
        self.comparison.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        comparison_layout.addWidget(self.comparison)
        split.addWidget(comparison)
        history = QWidget()
        history_layout = QVBoxLayout(history)
        history_layout.setContentsMargins(0, 0, 0, 0)
        history_layout.addWidget(QLabel("최근 입력 기록 · 최대 300줄 · 마우스 이동은 초당 최대 10회 표시"))
        self.history = QTableWidget(0, 6)
        self.history.setHorizontalHeaderLabels(["시각", "테스트 모드", "입력 종류", "인식 결과", "상태", "원시 HID"])
        self.history.horizontalHeader().setSectionResizeMode(3, QHeaderView.ResizeMode.Stretch)
        for column, width in ((0, 100), (1, 115), (2, 95), (4, 80), (5, 240)):
            self.history.setColumnWidth(column, width)
        history_layout.addWidget(self.history)
        split.addWidget(history)
        split.setSizes([220, 330])
        layout.addWidget(split, 1)
        for table in (self.comparison, self.history):
            table.verticalHeader().hide()
            table.setAlternatingRowColors(True)
            table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
            table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.enabled.toggled.connect(self.configure)
        self.mode.currentIndexChanged.connect(self.configure)
        self.clear_button.clicked.connect(self.clear)

    def configure(self, *_):
        self.changed.emit(self.mode.currentText() if self.enabled.isChecked() else None)
        self.update_status()

    def update_status(self):
        if not self.enabled.isChecked():
            self.latest.setText("기록 중지 · 디버깅 모드를 켜면 새 입력을 기록합니다.")
        elif not self.connected:
            self.latest.setText("연결 대기 · 위에서 ESP32를 연결하세요. 이전 기록은 남아 있습니다.")
        else:
            self.latest.setText(f"{self.mode.currentText()} · 새 입력을 기다리는 중…")

    def set_connected(self, connected):
        self.connected = connected
        self.update_status()

    def clear(self):
        self.counts.clear()
        self.comparison.setRowCount(0)
        self.history.setRowCount(0)
        self.stats.setText("수신 0 · 원본 순번 누락 0 · 해석 제외 0 · 표시 누락 0")
        self.configure()

    def on_batch(self, batch):
        if not self.enabled.isChecked() or not self.connected or batch.mode != self.mode.currentText():
            return
        ready = "리모컨 연결됨" if batch.remote_ready else "리모컨 연결 안 됨" if batch.remote_ready is False else "리모컨 상태 대기"
        self.stats.setText(f"{ready} · 수신 {batch.packets} · 원본 순번 누락 {batch.gaps} · "
                           f"해석 제외 {batch.invalid} · 표시 누락 {batch.dropped}")
        for event in batch.events:
            if self.history.rowCount() >= self.MAX_ROWS:
                self.history.removeRow(0)
            row = self.history.rowCount()
            self.history.insertRow(row)
            values = (datetime.now().strftime("%H:%M:%S.%f")[:-3], batch.mode,
                      event.source, event.name, event.action, event.raw)
            for column, value in enumerate(values):
                item = QTableWidgetItem(value)
                item.setToolTip(value)
                self.history.setItem(row, column, item)
            # Movement/repeats/releases must not inflate recognized press counts.
            if event.action in ("누름", "상태 복구"):
                name = f"{event.source} · {event.name}"
                if name not in self.counts:
                    self.counts[name] = [0, 0]
                    index = self.comparison.rowCount()
                    self.comparison.insertRow(index)
                    self.comparison.setItem(index, 0, QTableWidgetItem(name))
                self.counts[name][MODES.index(batch.mode)] += 1
                index = list(self.counts).index(name)
                for column, count in enumerate(self.counts[name], 1):
                    self.comparison.setItem(index, column, QTableWidgetItem(str(count)))
            if event.action != "이동":
                self.latest.setText(f"{batch.mode} · {event.source}: {event.name} · {event.action}")
        if batch.events:
            self.history.scrollToBottom()
