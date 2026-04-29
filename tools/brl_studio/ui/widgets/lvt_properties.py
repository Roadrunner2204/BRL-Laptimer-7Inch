"""
Properties panel for the LvtDesigner — Phase 6.

Shows widget-specific fields when a graphics item is selected. Edits
emit `changed(patch_dict)` which the host applies via
`LvtDesigner.update_selected(**patch)`.
"""

from __future__ import annotations

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QBrush, QColor, QFont
from PyQt6.QtWidgets import (
    QColorDialog,
    QComboBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from core.dash_config import FIELD_LABELS
from core.obd_status import get_obd_status_poller, is_obd_field


class _ColorButton(QPushButton):
    color_changed = pyqtSignal(str)   # hex string

    def __init__(self) -> None:
        super().__init__()
        self.setMinimumWidth(80)
        self._hex = "#FFFFFF"
        self.clicked.connect(self._pick)
        self._update_label()

    def set_hex(self, hex_str: str | None) -> None:
        self._hex = hex_str or "#FFFFFF"
        self._update_label()

    def hex(self) -> str:
        return self._hex

    def _pick(self) -> None:
        c = QColorDialog.getColor(QColor(self._hex), self, "Farbe wählen")
        if c.isValid():
            self._hex = c.name()
            self._update_label()
            self.color_changed.emit(self._hex)

    def _update_label(self) -> None:
        c = QColor(self._hex)
        fg = "#000" if (c.red() + c.green() + c.blue()) > 380 else "#fff"
        self.setStyleSheet(
            f"background-color: {self._hex}; color: {fg}; border: 1px solid #555;"
        )
        self.setText(self._hex)


class LvtProperties(QWidget):
    changed = pyqtSignal(dict)

    def __init__(self) -> None:
        super().__init__()
        self._silent = False
        self._current: dict | None = None

        self.lbl_type = QLabel("—")
        self.lbl_type.setStyleSheet("font-weight: bold;")

        # Common geometry
        self.sp_x = QSpinBox(); self.sp_x.setRange(-2000, 2000)
        self.sp_y = QSpinBox(); self.sp_y.setRange(-2000, 2000)
        self.sp_w = QSpinBox(); self.sp_w.setRange(1, 2000)
        self.sp_h = QSpinBox(); self.sp_h.setRange(1, 2000)
        for s in (self.sp_x, self.sp_y, self.sp_w, self.sp_h):
            s.valueChanged.connect(self._emit_geom)

        # Line endpoints
        self.sp_x1 = QSpinBox(); self.sp_x1.setRange(-2000, 2000)
        self.sp_y1 = QSpinBox(); self.sp_y1.setRange(-2000, 2000)
        self.sp_x2 = QSpinBox(); self.sp_x2.setRange(-2000, 2000)
        self.sp_y2 = QSpinBox(); self.sp_y2.setRange(-2000, 2000)
        for s in (self.sp_x1, self.sp_y1, self.sp_x2, self.sp_y2):
            s.valueChanged.connect(self._emit_line)

        # Style
        self.btn_color = _ColorButton()
        self.btn_color.color_changed.connect(
            lambda c: self._patch({"color": c})
        )
        self.btn_fill = _ColorButton()
        self.btn_fill.color_changed.connect(
            lambda c: self._patch({"fill": c})
        )
        self.btn_border = _ColorButton()
        self.btn_border.color_changed.connect(
            lambda c: self._patch({"border": c})
        )
        self.sp_border_w = QSpinBox(); self.sp_border_w.setRange(0, 30)
        self.sp_border_w.valueChanged.connect(
            lambda v: self._patch({"border_w": v})
        )
        self.sp_line_w = QSpinBox(); self.sp_line_w.setRange(1, 30)
        self.sp_line_w.valueChanged.connect(
            lambda v: self._patch({"width": v})
        )
        self.sp_font = QSpinBox(); self.sp_font.setRange(6, 200)
        self.sp_font.valueChanged.connect(
            lambda v: self._patch({"font_pt": v})
        )
        self.cb_align = QComboBox()
        for v in ("left", "center", "right"):
            self.cb_align.addItem(v, v)
        self.cb_align.currentIndexChanged.connect(
            lambda _: self._patch({"align": self.cb_align.currentData()})
        )

        # Type-specific
        self.cb_field = QComboBox()
        for fid, label in FIELD_LABELS.items():
            self.cb_field.addItem(label, fid)
        self.cb_field.currentIndexChanged.connect(
            lambda _: self._patch({"field": int(self.cb_field.currentData())})
        )
        # Live-status colouring on the field combo: OBD/Analog fields
        # that the connected display is currently NOT receiving get
        # rendered italic + dim. GPS/timing fields are always normal.
        get_obd_status_poller().bus.updated.connect(self._refresh_field_combo)
        self._refresh_field_combo({})
        self.le_text = QLineEdit()
        self.le_text.editingFinished.connect(
            lambda: self._patch({"text": self.le_text.text()})
        )
        self.le_src = QLineEdit()
        self.le_src.editingFinished.connect(
            lambda: self._patch({"src": self.le_src.text()})
        )

        # ── layout ─────────────────────────────────────────────────────
        self.geom_form = QFormLayout()
        self.geom_form.addRow("X", self.sp_x)
        self.geom_form.addRow("Y", self.sp_y)
        self.geom_form.addRow("W", self.sp_w)
        self.geom_form.addRow("H", self.sp_h)

        self.line_form = QFormLayout()
        self.line_form.addRow("X1", self.sp_x1)
        self.line_form.addRow("Y1", self.sp_y1)
        self.line_form.addRow("X2", self.sp_x2)
        self.line_form.addRow("Y2", self.sp_y2)

        self.style_form = QFormLayout()
        self.style_form.addRow("Farbe", self.btn_color)
        self.style_form.addRow("Fill", self.btn_fill)
        self.style_form.addRow("Border", self.btn_border)
        self.style_form.addRow("Border-Stärke", self.sp_border_w)
        self.style_form.addRow("Linien-Stärke", self.sp_line_w)
        self.style_form.addRow("Schriftgröße", self.sp_font)
        self.style_form.addRow("Ausrichtung", self.cb_align)

        self.spec_form = QFormLayout()
        self.spec_form.addRow("Field", self.cb_field)
        self.spec_form.addRow("Text", self.le_text)
        self.spec_form.addRow("Bild-Pfad", self.le_src)

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.addWidget(self.lbl_type)
        root.addLayout(self.geom_form)
        root.addLayout(self.line_form)
        root.addLayout(self.style_form)
        root.addLayout(self.spec_form)
        root.addStretch(1)

        self.set_widget(None)

    # ── API ────────────────────────────────────────────────────────────

    def set_widget(self, d: dict | None) -> None:
        self._current = d
        self._silent = True
        # Reset visibility — show only the rows relevant to the type
        if d is None:
            self.lbl_type.setText("(nichts ausgewählt)")
            self._set_section_visible(self.geom_form, False)
            self._set_section_visible(self.line_form, False)
            self._set_section_visible(self.style_form, False)
            self._set_section_visible(self.spec_form, False)
            self._silent = False
            return

        t = d.get("type", "")
        self.lbl_type.setText(f"{t.capitalize()}")

        # geometry vs line
        if t == "line":
            self._set_section_visible(self.geom_form, False)
            self._set_section_visible(self.line_form, True)
            self.sp_x1.setValue(int(d.get("x1", 0)))
            self.sp_y1.setValue(int(d.get("y1", 0)))
            self.sp_x2.setValue(int(d.get("x2", 0)))
            self.sp_y2.setValue(int(d.get("y2", 0)))
        else:
            self._set_section_visible(self.geom_form, True)
            self._set_section_visible(self.line_form, False)
            self.sp_x.setValue(int(d.get("x", 0)))
            self.sp_y.setValue(int(d.get("y", 0)))
            self.sp_w.setValue(int(d.get("w", 100)))
            self.sp_h.setValue(int(d.get("h", 50)))

        # style fields
        self._set_section_visible(self.style_form, True)
        self._row_visible(self.style_form, self.btn_color,
                          t in ("value", "label", "line"))
        self._row_visible(self.style_form, self.btn_fill, t == "rect")
        self._row_visible(self.style_form, self.btn_border, t == "rect")
        self._row_visible(self.style_form, self.sp_border_w, t == "rect")
        self._row_visible(self.style_form, self.sp_line_w, t == "line")
        self._row_visible(self.style_form, self.sp_font,
                          t in ("value", "label"))
        self._row_visible(self.style_form, self.cb_align,
                          t in ("value", "label"))

        # type-specific
        self._set_section_visible(self.spec_form, True)
        self._row_visible(self.spec_form, self.cb_field, t == "value")
        self._row_visible(self.spec_form, self.le_text, t == "label")
        self._row_visible(self.spec_form, self.le_src, t == "image")

        # populate values
        self.btn_color.set_hex(d.get("color"))
        self.btn_fill.set_hex(d.get("fill"))
        self.btn_border.set_hex(d.get("border"))
        self.sp_border_w.setValue(int(d.get("border_w", 0)))
        self.sp_line_w.setValue(int(d.get("width", 2)))
        self.sp_font.setValue(int(d.get("font_pt", 16)))
        for i in range(self.cb_align.count()):
            if self.cb_align.itemData(i) == d.get("align", "center"):
                self.cb_align.setCurrentIndex(i); break
        for i in range(self.cb_field.count()):
            if self.cb_field.itemData(i) == d.get("field", 0):
                self.cb_field.setCurrentIndex(i); break
        self.le_text.setText(str(d.get("text", "")))
        self.le_src.setText(str(d.get("src", "")))
        self._silent = False

    # ── helpers ────────────────────────────────────────────────────────

    def _refresh_field_combo(self, _doc: dict) -> None:
        """Per-item enable/disable + colour based on live OBD status."""
        poller = get_obd_status_poller()
        live = poller.live_field_ids()
        model = self.cb_field.model()
        italic = QFont(); italic.setItalic(True)
        normal = QFont()
        for i in range(self.cb_field.count()):
            fid = int(self.cb_field.itemData(i))
            label = FIELD_LABELS.get(fid, str(fid))
            if is_obd_field(fid) and fid not in live:
                # Not live → grey + italic + tag, but selectable so the
                # user can pre-stage a layout before the car is connected.
                self.cb_field.setItemText(i, f"{label}  · nicht live")
                model.setData(model.index(i, 0),
                              QBrush(QColor("#888")), Qt.ItemDataRole.ForegroundRole)
                model.setData(model.index(i, 0), italic,
                              Qt.ItemDataRole.FontRole)
            else:
                self.cb_field.setItemText(i, label)
                model.setData(model.index(i, 0),
                              QBrush(QColor("#e6e6e6")),
                              Qt.ItemDataRole.ForegroundRole)
                model.setData(model.index(i, 0), normal,
                              Qt.ItemDataRole.FontRole)

    @staticmethod
    def _set_section_visible(form: QFormLayout, on: bool) -> None:
        for i in range(form.count()):
            it = form.itemAt(i)
            if it is None or it.widget() is None:
                continue
            it.widget().setVisible(on)

    @staticmethod
    def _row_visible(form: QFormLayout, widget, on: bool) -> None:
        for i in range(form.rowCount()):
            label = form.itemAt(i, QFormLayout.ItemRole.LabelRole)
            field = form.itemAt(i, QFormLayout.ItemRole.FieldRole)
            if field and field.widget() is widget:
                if label and label.widget():
                    label.widget().setVisible(on)
                widget.setVisible(on)
                return

    def _emit_geom(self) -> None:
        if self._silent:
            return
        self._patch({
            "x": self.sp_x.value(),
            "y": self.sp_y.value(),
            "w": self.sp_w.value(),
            "h": self.sp_h.value(),
        })

    def _emit_line(self) -> None:
        if self._silent:
            return
        self._patch({
            "x1": self.sp_x1.value(), "y1": self.sp_y1.value(),
            "x2": self.sp_x2.value(), "y2": self.sp_y2.value(),
        })

    def _patch(self, d: dict) -> None:
        if self._silent or self._current is None:
            return
        self.changed.emit(d)
