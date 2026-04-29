"""
Display layout configurator — Phase 5 (Slot-Editor) + Phase 6 (Custom-Designer).

Two modes side by side, switched by the toolbar at the top:

    [ Slot-Editor ] [ Custom-Designer ]

Slot-Editor edits the laptimer's DashConfig (Z1/Z2/Z3 slot fields +
global settings). The Custom-Designer is a 1024×600 drag-and-drop canvas
that produces .lvt files; the Display-side renderer for these is a
follow-up firmware task.

Both modes save locally as JSON (dash_config.json / *.lvt) and have an
"upload to Display" button. The DashConfig endpoint (POST /dash_config)
and the .lvt endpoint (POST /lvt) are both pending firmware support; the
upload buttons handle a 404 gracefully so Studio remains useful with
local files until then.
"""

from __future__ import annotations

import copy
import json

import requests
from PyQt6.QtCore import QSettings, Qt
from PyQt6.QtGui import QBrush, QColor, QFont, QPainter, QPen
from PyQt6.QtWidgets import (
    QButtonGroup,
    QCheckBox,
    QComboBox,
    QFileDialog,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QPushButton,
    QSizePolicy,
    QSpinBox,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from core.dash_config import (
    DELTA_SCALE_OPTIONS_MS,
    FIELD_LABELS,
    LANGUAGE_LABELS,
    UNITS_LABELS,
    VEH_CONN_LABELS,
    Z1_FIELDS,
    Z1_SLOTS,
    Z2_FIELDS,
    Z2_SLOTS,
    Z3_FIELDS,
    Z3_SLOTS,
    default_dash_config,
)
from core.http_client import Endpoint
from core.lvt_format import WIDGET_DEFAULTS, empty_lvt
from core.obd_status import get_obd_status_poller, is_obd_field
from ui.widgets.lvt_designer import LvtDesigner
from ui.widgets.lvt_properties import LvtProperties


# ---------------------------------------------------------------------------
# Slot-mode preview (1024×600 mock-up of DashConfig zones)
# ---------------------------------------------------------------------------


class _DashPreview(QWidget):
    SCREEN_W = 1024
    SCREEN_H = 600

    def __init__(self) -> None:
        super().__init__()
        self.cfg = default_dash_config()
        self.setMinimumSize(512, 300)
        self.setSizePolicy(QSizePolicy.Policy.Expanding,
                           QSizePolicy.Policy.Expanding)

    def set_config(self, cfg: dict) -> None:
        self.cfg = cfg
        self.update()

    def paintEvent(self, _evt) -> None:  # noqa: ANN001
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        avail_w = self.width() - 8
        avail_h = self.height() - 8
        scale = min(avail_w / self.SCREEN_W, avail_h / self.SCREEN_H)
        sw = self.SCREEN_W * scale
        sh = self.SCREEN_H * scale
        ox = (self.width() - sw) / 2
        oy = (self.height() - sh) / 2
        p.fillRect(int(ox), int(oy), int(sw), int(sh), QColor("#0d0f12"))
        p.setPen(QPen(QColor("#3a3f48"), 1))
        p.drawRect(int(ox), int(oy), int(sw), int(sh))
        font_label = QFont("Segoe UI", max(7, int(scale * 11)))

        def map_y(y: int) -> int:
            return int(oy + y * scale)

        p.fillRect(int(ox), map_y(0), int(sw), int(40 * scale),
                   QColor("#1a1c20"))
        p.fillRect(int(ox), map_y(40), int(sw), int(50 * scale),
                   QColor("#222428"))
        p.fillRect(int(ox), map_y(90), int(sw), int(86 * scale),
                   QColor("#26282d"))

        def slot(x0: int, y0: int, w: int, h: int, field: int) -> None:
            r = (int(ox + x0 * scale), int(oy + y0 * scale),
                 int(w * scale), int(h * scale))
            p.fillRect(*r, QColor("#1a1c20"))
            p.setPen(QPen(QColor("#3a3f48"), 1))
            p.drawRect(*r)
            label = FIELD_LABELS.get(field, "?")
            p.setPen(QColor("#888"))
            p.setFont(font_label)
            p.drawText(r[0] + 4, r[1] + int(font_label.pointSize() * 1.4), label)

        z1 = self.cfg.get("z1", [])
        z1_y, z1_h = 176, 206
        wide_w = 256
        narrow_w = (self.SCREEN_W - 2 * wide_w) // 3
        if len(z1) >= 5:
            slot(0, z1_y, wide_w, z1_h, z1[0])
            slot(wide_w, z1_y, wide_w, z1_h, z1[1])
            for i in range(3):
                slot(2 * wide_w + i * narrow_w, z1_y, narrow_w, z1_h, z1[2 + i])

        z2 = self.cfg.get("z2", [])
        if len(z2) >= 3:
            sw_each = self.SCREEN_W // 3
            for i in range(3):
                slot(i * sw_each, 382, sw_each, 85, z2[i])

        z3 = self.cfg.get("z3", [])
        if len(z3) >= 5:
            sw_each = self.SCREEN_W // 5
            for i in range(5):
                slot(i * sw_each, 467, sw_each, 133, z3[i])

        p.end()


def _build_combo(allowed: list[int]) -> QComboBox:
    cb = QComboBox()
    for fid in allowed:
        cb.addItem(FIELD_LABELS.get(fid, str(fid)), fid)
    return cb


def _apply_live_status_to_combo(cb: QComboBox, live: set[int]) -> None:
    """Grey-italic OBD/Analog entries that aren't currently live; leave
    GPS/timing entries alone. Stays selectable so the user can pre-stage
    a layout before the car/adapter is connected."""
    model = cb.model()
    italic = QFont(); italic.setItalic(True)
    normal = QFont()
    for i in range(cb.count()):
        fid = int(cb.itemData(i))
        label = FIELD_LABELS.get(fid, str(fid))
        idx = model.index(i, 0)
        if is_obd_field(fid) and fid not in live:
            cb.setItemText(i, f"{label}  · nicht live")
            model.setData(idx, QBrush(QColor("#888")),
                          Qt.ItemDataRole.ForegroundRole)
            model.setData(idx, italic, Qt.ItemDataRole.FontRole)
        else:
            cb.setItemText(i, label)
            model.setData(idx, QBrush(QColor("#e6e6e6")),
                          Qt.ItemDataRole.ForegroundRole)
            model.setData(idx, normal, Qt.ItemDataRole.FontRole)


# ---------------------------------------------------------------------------
# Slot-Editor page (Phase 5)
# ---------------------------------------------------------------------------


class _SlotEditor(QWidget):
    def __init__(self, host_view) -> None:  # noqa: ANN001
        super().__init__()
        self._host = host_view
        self._cfg: dict = default_dash_config()
        self._silent = False

        self.preview = _DashPreview()

        self.z1_combos = [_build_combo(Z1_FIELDS) for _ in range(Z1_SLOTS)]
        self.z2_combos = [_build_combo(Z2_FIELDS) for _ in range(Z2_SLOTS)]
        self.z3_combos = [_build_combo(Z3_FIELDS) for _ in range(Z3_SLOTS)]

        z1_box = QGroupBox("Zone 1 (oben)")
        z1_form = QFormLayout(z1_box)
        for i, c in enumerate(self.z1_combos):
            z1_form.addRow(f"Slot {i + 1}", c)
            c.currentIndexChanged.connect(self._on_field_changed)
        z2_box = QGroupBox("Zone 2 (Sektoren)")
        z2_form = QFormLayout(z2_box)
        for i, c in enumerate(self.z2_combos):
            z2_form.addRow(f"Slot {i + 1}", c)
            c.currentIndexChanged.connect(self._on_field_changed)
        z3_box = QGroupBox("Zone 3 (Engine / Analog)")
        z3_form = QFormLayout(z3_box)
        for i, c in enumerate(self.z3_combos):
            z3_form.addRow(f"Slot {i + 1}", c)
            c.currentIndexChanged.connect(self._on_field_changed)

        self.cb_lang = QComboBox()
        for k, v in LANGUAGE_LABELS.items():
            self.cb_lang.addItem(v, k)
        self.cb_units = QComboBox()
        for k, v in UNITS_LABELS.items():
            self.cb_units.addItem(v, k)
        self.cb_veh = QComboBox()
        for k, v in VEH_CONN_LABELS.items():
            self.cb_veh.addItem(v, k)
        self.sp_delta = QSpinBox()
        self.sp_delta.setRange(500, 60000)
        self.sp_delta.setSingleStep(500)
        self.sp_delta.setSuffix(" ms")
        self.cb_show_obd = QCheckBox("Engine-Daten anzeigen")
        for w in (self.cb_lang, self.cb_units, self.cb_veh):
            w.currentIndexChanged.connect(self._on_field_changed)
        self.sp_delta.valueChanged.connect(self._on_field_changed)
        self.cb_show_obd.toggled.connect(self._on_field_changed)

        s_box = QGroupBox("Allgemein")
        s_form = QFormLayout(s_box)
        s_form.addRow("Sprache", self.cb_lang)
        s_form.addRow("Einheiten", self.cb_units)
        s_form.addRow("Fahrzeug-Anbindung", self.cb_veh)
        s_form.addRow("Delta-Bar Skala", self.sp_delta)
        s_form.addRow("", self.cb_show_obd)

        right_col = QVBoxLayout()
        right_col.addWidget(z1_box)
        right_col.addWidget(z2_box)
        right_col.addWidget(z3_box)
        right_col.addWidget(s_box)
        right_col.addStretch(1)
        right_w = QWidget(); right_w.setLayout(right_col)
        right_w.setMaximumWidth(380)

        body = QHBoxLayout(self)
        body.setContentsMargins(0, 0, 0, 0)
        body.addWidget(self.preview, stretch=1)
        body.addWidget(right_w)
        self._populate_form()

        # Refresh combo styling whenever the OBD-status poller reports
        # a new snapshot. Initial render with empty snapshot greys all
        # OBD entries; first successful poll populates them.
        get_obd_status_poller().bus.updated.connect(self._refresh_live)
        self._refresh_live({})

    def _refresh_live(self, _doc: dict) -> None:
        live = get_obd_status_poller().live_field_ids()
        for c in (*self.z1_combos, *self.z2_combos, *self.z3_combos):
            _apply_live_status_to_combo(c, live)

    def cfg(self) -> dict:
        return self._cfg

    def set_cfg(self, cfg: dict) -> None:
        merged = default_dash_config(); merged.update(cfg)
        self._cfg = merged
        self._populate_form()

    def _populate_form(self) -> None:
        self._silent = True
        for combos, key in ((self.z1_combos, "z1"),
                            (self.z2_combos, "z2"),
                            (self.z3_combos, "z3")):
            arr = self._cfg.get(key, [])
            for i, c in enumerate(combos):
                if i < len(arr):
                    j = c.findData(arr[i])
                    c.setCurrentIndex(j if j >= 0 else 0)
        for w, key in ((self.cb_lang, "language"),
                       (self.cb_units, "units"),
                       (self.cb_veh, "veh_conn_mode")):
            j = w.findData(int(self._cfg.get(key, 0)))
            w.setCurrentIndex(j if j >= 0 else 0)
        self.sp_delta.setValue(int(self._cfg.get("delta_scale_ms", 1000)))
        self.cb_show_obd.setChecked(bool(self._cfg.get("show_obd", 1)))
        self._silent = False
        self.preview.set_config(self._cfg)

    def _on_field_changed(self) -> None:
        if self._silent:
            return
        self._cfg["z1"] = [c.currentData() for c in self.z1_combos]
        self._cfg["z2"] = [c.currentData() for c in self.z2_combos]
        self._cfg["z3"] = [c.currentData() for c in self.z3_combos]
        self._cfg["language"] = int(self.cb_lang.currentData())
        self._cfg["units"] = int(self.cb_units.currentData())
        self._cfg["veh_conn_mode"] = int(self.cb_veh.currentData())
        self._cfg["delta_scale_ms"] = int(self.sp_delta.value())
        self._cfg["show_obd"] = 1 if self.cb_show_obd.isChecked() else 0
        self.preview.set_config(self._cfg)


# ---------------------------------------------------------------------------
# Custom-Designer page (Phase 6)
# ---------------------------------------------------------------------------


class _CustomDesigner(QWidget):
    def __init__(self, host_view) -> None:  # noqa: ANN001
        super().__init__()
        self._host = host_view

        self.designer = LvtDesigner()
        self.props = LvtProperties()
        self.designer.selection_changed.connect(self.props.set_widget)
        self.props.changed.connect(
            lambda patch: self.designer.update_selected(**patch)
        )

        # Toolbar (left side) — widget add buttons
        tb = QVBoxLayout()
        tb.addWidget(QLabel("Hinzufügen"))
        for kind, label in (("value", "Wertfeld"), ("label", "Label"),
                            ("rect", "Box"), ("line", "Linie"),
                            ("image", "Bild")):
            btn = QPushButton(f"+ {label}")
            btn.clicked.connect(
                lambda _=False, k=kind: self.designer.add_widget(WIDGET_DEFAULTS[k])
            )
            tb.addWidget(btn)
        tb.addSpacing(12)
        btn_del = QPushButton("🗑 Auswahl löschen")
        btn_del.clicked.connect(self.designer.remove_selected)
        tb.addWidget(btn_del)
        tb.addStretch(1)

        tb_w = QWidget(); tb_w.setLayout(tb); tb_w.setMaximumWidth(180)

        right = QVBoxLayout()
        right.addWidget(QLabel("Eigenschaften"))
        right.addWidget(self.props)
        right_w = QWidget(); right_w.setLayout(right)
        right_w.setMaximumWidth(280)

        body = QHBoxLayout(self)
        body.setContentsMargins(0, 0, 0, 0)
        body.addWidget(tb_w)
        body.addWidget(self.designer, stretch=1)
        body.addWidget(right_w)

    def doc(self) -> dict:
        return self.designer.doc()

    def set_doc(self, doc: dict) -> None:
        self.designer.set_doc(doc)


# ---------------------------------------------------------------------------
# Layout view — toggles between the two modes
# ---------------------------------------------------------------------------


class LayoutView(QWidget):
    def __init__(self, main_window) -> None:  # noqa: ANN001
        super().__init__()
        self._mw = main_window
        self._settings = QSettings()

        # ── Mode toggle + actions ──────────────────────────────────────
        self.btn_mode_slot = QPushButton("Slot-Editor")
        self.btn_mode_slot.setCheckable(True)
        self.btn_mode_slot.setChecked(True)
        self.btn_mode_custom = QPushButton("Custom-Designer")
        self.btn_mode_custom.setCheckable(True)
        grp = QButtonGroup(self)
        grp.addButton(self.btn_mode_slot)
        grp.addButton(self.btn_mode_custom)
        self.btn_mode_slot.clicked.connect(lambda: self._set_mode(0))
        self.btn_mode_custom.clicked.connect(lambda: self._set_mode(1))

        self.btn_load = QPushButton("Aus Datei laden…")
        self.btn_save = QPushButton("💾 Lokal speichern…")
        self.btn_send = QPushButton("📡 Zum Display senden")
        self.btn_default = QPushButton("Default")
        self.btn_load.clicked.connect(self._load_file)
        self.btn_save.clicked.connect(self._save_file)
        self.btn_send.clicked.connect(self._send_to_display)
        self.btn_default.clicked.connect(self._reset_default)

        actions = QHBoxLayout()
        actions.addWidget(self.btn_mode_slot)
        actions.addWidget(self.btn_mode_custom)
        actions.addSpacing(20)
        actions.addWidget(self.btn_load)
        actions.addWidget(self.btn_default)
        actions.addStretch(1)
        actions.addWidget(self.btn_save)
        actions.addWidget(self.btn_send)

        # ── Stack ──────────────────────────────────────────────────────
        self.slot_page = _SlotEditor(self)
        self.custom_page = _CustomDesigner(self)
        self.custom_page.set_doc(empty_lvt())
        self.stack = QStackedWidget()
        self.stack.addWidget(self.slot_page)
        self.stack.addWidget(self.custom_page)

        root = QVBoxLayout(self)
        root.setContentsMargins(8, 8, 8, 8)
        root.addLayout(actions)
        root.addWidget(self.stack, stretch=1)

    # ── Mode handling ──────────────────────────────────────────────────

    def _set_mode(self, idx: int) -> None:
        self.stack.setCurrentIndex(idx)

    def _is_slot_mode(self) -> bool:
        return self.stack.currentIndex() == 0

    # ── Load / Save / Send ─────────────────────────────────────────────

    def _reset_default(self) -> None:
        if self._is_slot_mode():
            self.slot_page.set_cfg(default_dash_config())
        else:
            self.custom_page.set_doc(empty_lvt())

    def _load_file(self) -> None:
        if self._is_slot_mode():
            path, _ = QFileDialog.getOpenFileName(
                self, "Layout-JSON öffnen", "", "JSON (*.json)"
            )
            if not path:
                return
            try:
                with open(path, "r", encoding="utf-8") as f:
                    doc = json.load(f)
            except (OSError, json.JSONDecodeError) as e:
                QMessageBox.warning(self, "Fehler", str(e))
                return
            if not isinstance(doc, dict):
                QMessageBox.warning(self, "Fehler", "Kein Objekt.")
                return
            self.slot_page.set_cfg(doc)
        else:
            path, _ = QFileDialog.getOpenFileName(
                self, ".lvt öffnen", "", "Layouts (*.lvt *.json)"
            )
            if not path:
                return
            try:
                with open(path, "r", encoding="utf-8") as f:
                    doc = json.load(f)
            except (OSError, json.JSONDecodeError) as e:
                QMessageBox.warning(self, "Fehler", str(e))
                return
            if not isinstance(doc, dict):
                QMessageBox.warning(self, "Fehler", "Kein .lvt-Objekt.")
                return
            self.custom_page.set_doc(doc)

    def _save_file(self) -> None:
        if self._is_slot_mode():
            path, _ = QFileDialog.getSaveFileName(
                self, "Slot-Layout speichern", "dash_config.json",
                "JSON (*.json)"
            )
            data = self.slot_page.cfg()
        else:
            path, _ = QFileDialog.getSaveFileName(
                self, ".lvt speichern", "custom_layout.lvt",
                "Layouts (*.lvt)"
            )
            data = self.custom_page.doc()
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
        except OSError as e:
            QMessageBox.warning(self, "Fehler", str(e))
            return
        self._mw.set_status(f"Gespeichert: {path}")

    def _send_to_display(self) -> None:
        host = str(self._settings.value("display/host", "192.168.4.1", str))
        port = int(self._settings.value("display/port", 80, int))
        ep = Endpoint(host=host, port=port)
        if self._is_slot_mode():
            url = ep.url("/dash_config")
            payload = copy.deepcopy(self.slot_page.cfg())
            label = "DashConfig"
        else:
            url = ep.url("/lvt")
            payload = copy.deepcopy(self.custom_page.doc())
            label = ".lvt"
        try:
            r = requests.post(url, json=payload, timeout=(5.0, 15.0))
            if r.status_code == 404:
                QMessageBox.information(
                    self, "Endpoint fehlt",
                    f"Das Display hat noch keinen passenden Endpoint für "
                    f"{label} ({url}).\n\n"
                    "Lokal speichern und manuell auf SD kopieren funktioniert "
                    "weiterhin. Endpoint kann firmware-seitig in "
                    "main/wifi/data_server.cpp ergänzt werden.",
                )
                return
            r.raise_for_status()
        except requests.RequestException as e:
            QMessageBox.warning(self, "Upload-Fehler", str(e))
            return
        self._mw.set_status(f"{label} ans Display gesendet ✓")
        QMessageBox.information(self, "Erfolg",
                                f"{label} wurde übertragen.")
