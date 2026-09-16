"""A string-first editor; device writes are explicit and independently verified."""
from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QLabel, QComboBox,
    QPushButton, QCheckBox, QSpinBox, QPlainTextEdit, QTableWidget, QTableWidgetItem,
    QHeaderView, QGridLayout, QMessageBox)
from macros import Macro, Text, Wait, slot_for
from protocol import BUTTONS


class MacroPanel(QWidget):
    requested = Signal(str, object)
    stop_requested = Signal()

    def __init__(self):
        super().__init__()
        self.connected = self.busy = self.loading = self.dirty = False
        self.revision = None
        self.preserve_read = False
        self.steps = []
        self.slot = 8
        self.baseline = Macro()
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 16, 0, 0)
        layout.setSpacing(12)
        targets = QHBoxLayout()
        targets.addWidget(QLabel('편집할 버튼'))
        self.target = QComboBox()
        for mode in (0, 1):
            for button, name in BUTTONS.items():
                if button == 0x6A or (mode == 1 and button == 0x4A):
                    continue
                self.target.addItem(f'{"일반" if mode == 0 else "마우스"} 모드 · {name}', slot_for(button, mode))
        self.target.setCurrentIndex(self.target.findData(self.slot))
        targets.addWidget(self.target, 1)
        self.read_button = QPushButton('장치에서 읽기')
        targets.addWidget(self.read_button)
        self.stop_button = QPushButton('실행 중지')
        self.stop_button.setToolTip('장치에서 실행 중인 매크로를 중지하고 눌린 키를 해제합니다.')
        targets.addWidget(self.stop_button)
        layout.addLayout(targets)
        self.enabled = QCheckBox('이 버튼에 문자열 매크로 사용')
        layout.addWidget(self.enabled)
        note = QLabel('저장하면 기존 버튼 동작 대신 실행됩니다. 같은 버튼을 다시 누르면 중지됩니다.\n미국식 QWERTY · 영문 입력 · Caps Lock 꺼짐 기준입니다.')
        note.setObjectName('muted')
        note.setWordWrap(True)
        layout.addWidget(note)

        body = QHBoxLayout()
        left = QVBoxLayout()
        self.table = QTableWidget(0, 2)
        self.table.setHorizontalHeaderLabels(['단계', '내용'])
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        self.table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        self.table.verticalHeader().hide()
        self.table.setShowGrid(False)
        self.table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QTableWidget.SelectionMode.SingleSelection)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        left.addWidget(self.table, 1)
        add = QHBoxLayout()
        self.add_text = QPushButton('문자열 추가')
        self.add_wait = QPushButton('대기 추가')
        add.addWidget(self.add_text)
        add.addWidget(self.add_wait)
        left.addLayout(add)
        order = QHBoxLayout()
        self.up = QPushButton('위로')
        self.down = QPushButton('아래로')
        self.remove = QPushButton('삭제')
        for button in (self.up, self.down, self.remove):
            order.addWidget(button)
        left.addLayout(order)
        body.addLayout(left, 1)
        right = QVBoxLayout()
        self.step_title = QLabel('단계를 추가하세요')
        right.addWidget(self.step_title)
        self.text = QPlainTextEdit()
        self.text.setPlaceholderText('Hello World!\n\n영문·숫자·기본 기호를 입력하세요.\n줄바꿈은 Enter로 입력됩니다.')
        self.text.setTabChangesFocus(True)
        self.text.setAccessibleName('입력할 문자열')
        right.addWidget(self.text, 1)
        self.wait_widget = QWidget()
        wait_layout = QHBoxLayout(self.wait_widget)
        wait_layout.setContentsMargins(0, 0, 0, 0)
        self.wait_min, self.wait_max = self.range_controls(wait_layout, 60000, '대기')
        right.addWidget(self.wait_widget)
        self.add_tab = QPushButton('Tab 삽입')
        self.add_tab.clicked.connect(lambda: self.text.insertPlainText('\t'))
        right.addWidget(self.add_tab, alignment=Qt.AlignmentFlag.AlignLeft)
        body.addLayout(right, 1)
        layout.addLayout(body, 1)

        timing = QGridLayout()
        timing.setHorizontalSpacing(18)
        self.gap_min, self.gap_max = self.timing_row(timing, 0, '문자 사이 간격', 5000)
        self.repeat_min, self.repeat_max = self.timing_row(timing, 1, '반복 사이 간격', 60000)
        settings = QHBoxLayout()
        settings.addWidget(QLabel('반복 횟수'))
        self.repeats = QSpinBox()
        self.repeats.setRange(1, 100)
        self.repeats.setSuffix('회')
        settings.addWidget(self.repeats)
        settings.addStretch()
        settings.addWidget(QLabel('키 누름 시간'))
        self.hold = QSpinBox()
        self.hold.setRange(10, 500)
        self.hold.setSuffix(' ms')
        settings.addWidget(self.hold)
        timing.addLayout(settings, 2, 0, 1, 2)
        layout.addLayout(timing)
        hint = QLabel('최소·최대가 같으면 고정 시간, 다르면 매번 범위 안에서 무작위로 기다립니다.')
        hint.setObjectName('muted')
        hint.setWordWrap(True)
        layout.addWidget(hint)
        footer = QHBoxLayout()
        self.state = QLabel('먼저 장치에서 읽으면 저장할 수 있습니다.')
        self.state.setWordWrap(True)
        footer.addWidget(self.state, 1)
        self.save_button = QPushButton('매크로 저장', objectName='apply')
        footer.addWidget(self.save_button)
        layout.addLayout(footer)
        self.target.currentIndexChanged.connect(self.change_target)
        self.read_button.clicked.connect(self.read)
        self.save_button.clicked.connect(self.save)
        self.stop_button.clicked.connect(self.stop_requested)
        self.add_text.clicked.connect(lambda: self.add_step(Text('')))
        self.add_wait.clicked.connect(lambda: self.add_step(Wait()))
        self.up.clicked.connect(lambda: self.move_step(-1))
        self.down.clicked.connect(lambda: self.move_step(1))
        self.remove.clicked.connect(self.remove_step)
        self.table.currentCellChanged.connect(self.select_step)
        self.text.textChanged.connect(self.edit_step)
        self.wait_min.valueChanged.connect(self.edit_step)
        self.wait_max.valueChanged.connect(self.edit_step)
        for spin in (self.gap_min, self.gap_max, self.repeat_min, self.repeat_max, self.repeats, self.hold):
            spin.valueChanged.connect(self.changed)
        self.enabled.toggled.connect(self.changed)
        self.populate(Macro())

    def range_controls(self, layout, maximum, name):
        low, high = QSpinBox(), QSpinBox()
        for spin, suffix in ((low, '최소'), (high, '최대')):
            spin.setRange(0, maximum)
            spin.setSuffix(' ms')
            spin.setAccessibleName(f'{name} {suffix}')
        layout.addWidget(low)
        layout.addWidget(QLabel('~'))
        layout.addWidget(high)
        return low, high

    def timing_row(self, grid, row, title, maximum):
        grid.addWidget(QLabel(title), row, 0)
        controls = QHBoxLayout()
        result = self.range_controls(controls, maximum, title)
        grid.addLayout(controls, row, 1)
        return result

    def value(self):
        return Macro(self.enabled.isChecked(), self.repeats.value(), self.hold.value(),
                     self.gap_min.value(), self.gap_max.value(), self.repeat_min.value(),
                     self.repeat_max.value(), tuple(self.steps))

    def populate(self, macro):
        self.loading = True
        self.steps = list(macro.steps)
        self.enabled.setChecked(macro.enabled)
        for widget, value in ((self.repeats, macro.repeats), (self.hold, macro.hold_ms),
                              (self.gap_min, macro.gap_min), (self.gap_max, macro.gap_max),
                              (self.repeat_min, macro.repeat_min), (self.repeat_max, macro.repeat_max)):
            widget.setValue(value)
        self.baseline = macro
        self.loading = False
        self.render_steps(0 if self.steps else -1)
        self.changed()

    def render_steps(self, selected):
        self.table.blockSignals(True)
        self.table.setRowCount(len(self.steps))
        for row, step in enumerate(self.steps):
            title = '문자열' if isinstance(step, Text) else '대기'
            summary = step.text.replace('\n', ' ↵ ').replace('\t', ' ⇥ ') if isinstance(step, Text) else f'{step.minimum}~{step.maximum} ms'
            self.table.setItem(row, 0, QTableWidgetItem(f'{row+1}. {title}'))
            self.table.setItem(row, 1, QTableWidgetItem(summary))
        if selected >= 0:
            self.table.setCurrentCell(selected, 0)
        self.table.blockSignals(False)
        self.select_step(selected)

    def select_step(self, row, *_):
        self.loading = True
        step = self.steps[row] if 0 <= row < len(self.steps) else None
        is_text = isinstance(step, Text)
        self.text.setVisible(is_text or step is None)
        self.text.setEnabled(is_text and not self.busy)
        self.add_tab.setVisible(is_text)
        self.wait_widget.setVisible(isinstance(step, Wait))
        self.step_title.setText('입력할 문자열' if is_text else '대기 시간' if step else '단계를 추가하세요')
        self.text.setPlainText(step.text if is_text else '')
        if isinstance(step, Wait):
            self.wait_min.setValue(step.minimum)
            self.wait_max.setValue(step.maximum)
        self.loading = False
        self.refresh()

    def edit_step(self, *_):
        if self.loading:
            return
        row = self.table.currentRow()
        if not 0 <= row < len(self.steps):
            return
        old = self.steps[row]
        new = Text(self.text.toPlainText()) if isinstance(old, Text) else Wait(self.wait_min.value(), self.wait_max.value())
        self.steps[row] = new
        summary = new.text.replace('\n', ' ↵ ').replace('\t', ' ⇥ ') if isinstance(new, Text) else f'{new.minimum}~{new.maximum} ms'
        self.table.item(row, 1).setText(summary)
        self.changed()

    def add_step(self, step):
        self.steps.append(step)
        self.render_steps(len(self.steps)-1)
        self.changed()

    def move_step(self, delta):
        row = self.table.currentRow()
        if not 0 <= row+delta < len(self.steps):
            return
        self.steps[row], self.steps[row+delta] = self.steps[row+delta], self.steps[row]
        self.render_steps(row+delta)
        self.changed()

    def remove_step(self):
        row = self.table.currentRow()
        if row >= 0:
            self.steps.pop(row)
            self.render_steps(min(row, len(self.steps)-1))
            self.changed()

    def changed(self, *_):
        if self.loading:
            return
        self.dirty = self.value() != self.baseline
        try:
            size = len(self.value().encode())
            self.state.setText(f'{size}/512 bytes · ' + ('저장하지 않은 변경 있음' if self.dirty else '변경 없음'))
        except ValueError as exc:
            self.state.setText(str(exc))
        self.refresh()

    def confirm_replace(self):
        return not self.dirty or QMessageBox.question(self, '편집 내용 바꾸기',
            '저장하지 않은 매크로 편집 내용을 버릴까요?',
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No) == QMessageBox.StandardButton.Yes

    def change_target(self):
        slot = self.target.currentData()
        if not self.confirm_replace():
            self.target.blockSignals(True)
            self.target.setCurrentIndex(self.target.findData(self.slot))
            self.target.blockSignals(False)
            return
        self.slot, self.revision = slot, None
        self.populate(Macro())
        self.state.setText('장치에서 이 버튼의 매크로를 먼저 읽어주세요.')

    def read(self):
        self.preserve_read = self.revision is None and self.dirty
        if self.preserve_read or self.confirm_replace():
            self.requested.emit('macro_read', self.slot)

    def save(self):
        if self.revision is not None:
            self.value().encode()
            self.requested.emit('macro_save', (self.slot, self.revision, self.value()))

    def result(self, command, slot, payload):
        if command == 'macro_stop':
            self.state.setText('매크로 중지를 요청했습니다. 눌린 키를 해제합니다.')
        elif slot == self.slot:
            self.revision, macro = payload
            if command == 'macro_read' and self.preserve_read:
                self.baseline = macro
                self.preserve_read = False
                self.changed()
                self.state.setText('장치의 매크로를 확인했습니다. 화면의 편집 내용은 유지했습니다.')
                return
            self.populate(macro)
            self.state.setText('매크로 저장 완료 · 장치에서 다시 읽어 확인했습니다.' if command == 'macro_save'
                               else '장치의 매크로를 읽었습니다. 편집 후 매크로 저장을 누르세요.')
        self.refresh()

    def problem(self, message):
        self.revision = None
        self.state.setText(message + ' 다시 읽어 저장 상태를 확인하세요.')
        self.refresh()

    def set_connection(self, connected, busy=False):
        if self.connected and not connected:
            self.revision = None
        self.connected, self.busy = connected, busy
        self.refresh()

    def refresh(self):
        editable = not self.busy
        for widget in (self.target, self.enabled, self.table, self.add_text, self.add_wait,
                       self.gap_min, self.gap_max, self.repeat_min, self.repeat_max, self.repeats,
                       self.hold, self.wait_widget, self.add_tab):
            widget.setEnabled(editable)
        row = self.table.currentRow()
        self.text.setEnabled(editable and row >= 0 and isinstance(self.steps[row], Text))
        self.up.setEnabled(editable and row > 0)
        self.down.setEnabled(editable and 0 <= row < len(self.steps)-1)
        self.remove.setEnabled(editable and row >= 0)
        self.read_button.setEnabled(self.connected and editable)
        self.stop_button.setEnabled(self.connected)
        try:
            self.value().encode()
            valid = True
        except ValueError:
            valid = False
        self.save_button.setEnabled(self.connected and editable and self.revision is not None and self.dirty and valid)
