"""
.lvt drag-and-drop designer — Phase 6.

QGraphicsView/Scene at fixed 1024×600 display dimensions. Widgets are
QGraphicsItem subclasses that carry their .lvt dict in `data(0)`. Move
with the mouse; properties (incl. resize w/h) live in the right-hand
panel because resize-handles on a 1024×600 canvas at small zoom levels
get fiddly fast.

Public:
    LvtDesigner.set_doc(doc)            replace whole .lvt
    LvtDesigner.doc()                   serialise back to dict
    LvtDesigner.add_widget(type)        add at canvas centre
    Signals:
        selection_changed(widget_dict | None)
        doc_changed()                   any move / property edit
"""

from __future__ import annotations

import copy
from typing import Any

from PyQt6.QtCore import QPointF, QRectF, Qt, pyqtSignal
from PyQt6.QtGui import (
    QBrush,
    QColor,
    QFont,
    QPainter,
    QPen,
    QPixmap,
)
from PyQt6.QtWidgets import (
    QGraphicsItem,
    QGraphicsLineItem,
    QGraphicsPixmapItem,
    QGraphicsRectItem,
    QGraphicsScene,
    QGraphicsSimpleTextItem,
    QGraphicsView,
)

from core.dash_config import FIELD_LABELS
from core.lvt_format import LVT_CANVAS_H, LVT_CANVAS_W, empty_lvt

GRID_PX = 20


def _qcolor(s: str | None, fallback: str = "#FFFFFF") -> QColor:
    return QColor(s) if s else QColor(fallback)


# ---------------------------------------------------------------------------
# QGraphicsItem subclasses, one per .lvt widget type.
# Each carries its source dict in setData(0, ...) and rebuilds its visual
# representation in update_from_dict(); `to_dict` writes the current
# (possibly moved) geometry back.
# ---------------------------------------------------------------------------


class _BaseItem:
    """Mixin to standardise the dict round-trip + selection visuals."""

    def to_dict(self) -> dict:  # noqa: D401
        raise NotImplementedError

    def update_from_dict(self, d: dict) -> None:
        raise NotImplementedError


class ValueItem(QGraphicsRectItem, _BaseItem):
    def __init__(self, d: dict) -> None:
        super().__init__()
        self.setFlags(QGraphicsItem.GraphicsItemFlag.ItemIsMovable
                      | QGraphicsItem.GraphicsItemFlag.ItemIsSelectable
                      | QGraphicsItem.GraphicsItemFlag.ItemSendsGeometryChanges)
        self.update_from_dict(d)

    def update_from_dict(self, d: dict) -> None:
        self.setData(0, d)
        self.setRect(0, 0, float(d.get("w", 100)), float(d.get("h", 50)))
        self.setPos(float(d.get("x", 0)), float(d.get("y", 0)))
        self.setPen(QPen(QColor("#3a3f48"), 1, Qt.PenStyle.DashLine))
        self.setBrush(QBrush(QColor(0, 0, 0, 100)))
        self._color = _qcolor(d.get("color"))
        self._font = QFont("Segoe UI", int(d.get("font_pt", 24)),
                           QFont.Weight.Bold)
        self._field = int(d.get("field", 0))
        self._align = str(d.get("align", "center"))
        self.update()

    def to_dict(self) -> dict:
        d = dict(self.data(0))
        d["x"] = round(self.pos().x())
        d["y"] = round(self.pos().y())
        return d

    def paint(self, p: QPainter, opt, w) -> None:  # noqa: ANN001
        super().paint(p, opt, w)
        p.setPen(self._color)
        p.setFont(self._font)
        rect = self.rect()
        flag = {
            "left":   Qt.AlignmentFlag.AlignLeft,
            "right":  Qt.AlignmentFlag.AlignRight,
            "center": Qt.AlignmentFlag.AlignCenter,
        }.get(self._align, Qt.AlignmentFlag.AlignCenter) | Qt.AlignmentFlag.AlignVCenter
        text = "[" + FIELD_LABELS.get(self._field, "?") + "]"
        p.drawText(rect, flag, text)
        if self.isSelected():
            p.setPen(QPen(QColor("#2d6cdf"), 2))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawRect(rect)


class LabelItem(QGraphicsRectItem, _BaseItem):
    def __init__(self, d: dict) -> None:
        super().__init__()
        self.setFlags(QGraphicsItem.GraphicsItemFlag.ItemIsMovable
                      | QGraphicsItem.GraphicsItemFlag.ItemIsSelectable
                      | QGraphicsItem.GraphicsItemFlag.ItemSendsGeometryChanges)
        self.update_from_dict(d)

    def update_from_dict(self, d: dict) -> None:
        self.setData(0, d)
        self.setRect(0, 0, float(d.get("w", 100)), float(d.get("h", 30)))
        self.setPos(float(d.get("x", 0)), float(d.get("y", 0)))
        self.setPen(Qt.PenStyle.NoPen)
        self.setBrush(Qt.BrushStyle.NoBrush)
        self._color = _qcolor(d.get("color"))
        self._font = QFont("Segoe UI", int(d.get("font_pt", 16)))
        self._text = str(d.get("text", "Label"))
        self._align = str(d.get("align", "left"))
        self.update()

    def to_dict(self) -> dict:
        d = dict(self.data(0))
        d["x"] = round(self.pos().x())
        d["y"] = round(self.pos().y())
        return d

    def paint(self, p: QPainter, opt, w) -> None:  # noqa: ANN001
        rect = self.rect()
        p.setPen(self._color)
        p.setFont(self._font)
        flag = {
            "left":   Qt.AlignmentFlag.AlignLeft,
            "right":  Qt.AlignmentFlag.AlignRight,
            "center": Qt.AlignmentFlag.AlignCenter,
        }.get(self._align, Qt.AlignmentFlag.AlignLeft) | Qt.AlignmentFlag.AlignVCenter
        p.drawText(rect, flag, self._text)
        if self.isSelected():
            p.setPen(QPen(QColor("#2d6cdf"), 1, Qt.PenStyle.DashLine))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawRect(rect)


class RectItem(QGraphicsRectItem, _BaseItem):
    def __init__(self, d: dict) -> None:
        super().__init__()
        self.setFlags(QGraphicsItem.GraphicsItemFlag.ItemIsMovable
                      | QGraphicsItem.GraphicsItemFlag.ItemIsSelectable
                      | QGraphicsItem.GraphicsItemFlag.ItemSendsGeometryChanges)
        self.update_from_dict(d)

    def update_from_dict(self, d: dict) -> None:
        self.setData(0, d)
        self.setRect(0, 0, float(d.get("w", 100)), float(d.get("h", 50)))
        self.setPos(float(d.get("x", 0)), float(d.get("y", 0)))
        fill = d.get("fill")
        border = d.get("border")
        bw = int(d.get("border_w", 0))
        self.setBrush(QBrush(_qcolor(fill, "#1a1c20")) if fill
                      else Qt.BrushStyle.NoBrush)
        if border and bw > 0:
            self.setPen(QPen(_qcolor(border), bw))
        else:
            self.setPen(Qt.PenStyle.NoPen)

    def to_dict(self) -> dict:
        d = dict(self.data(0))
        d["x"] = round(self.pos().x())
        d["y"] = round(self.pos().y())
        return d


class LineItem(QGraphicsLineItem, _BaseItem):
    def __init__(self, d: dict) -> None:
        super().__init__()
        self.setFlags(QGraphicsItem.GraphicsItemFlag.ItemIsMovable
                      | QGraphicsItem.GraphicsItemFlag.ItemIsSelectable
                      | QGraphicsItem.GraphicsItemFlag.ItemSendsGeometryChanges)
        self.update_from_dict(d)

    def update_from_dict(self, d: dict) -> None:
        self.setData(0, d)
        self.setLine(float(d.get("x1", 0)), float(d.get("y1", 0)),
                     float(d.get("x2", 100)), float(d.get("y2", 0)))
        self.setPen(QPen(_qcolor(d.get("color"), "#3a3f48"),
                         int(d.get("width", 2))))

    def to_dict(self) -> dict:
        d = dict(self.data(0))
        # Lines are moved as a whole — apply pos offset to both endpoints.
        line = self.line()
        ox, oy = self.pos().x(), self.pos().y()
        d["x1"] = round(line.x1() + ox)
        d["y1"] = round(line.y1() + oy)
        d["x2"] = round(line.x2() + ox)
        d["y2"] = round(line.y2() + oy)
        return d


class ImageItem(QGraphicsPixmapItem, _BaseItem):
    def __init__(self, d: dict) -> None:
        super().__init__()
        self.setFlags(QGraphicsItem.GraphicsItemFlag.ItemIsMovable
                      | QGraphicsItem.GraphicsItemFlag.ItemIsSelectable
                      | QGraphicsItem.GraphicsItemFlag.ItemSendsGeometryChanges)
        self.update_from_dict(d)

    def update_from_dict(self, d: dict) -> None:
        self.setData(0, d)
        self.setPos(float(d.get("x", 0)), float(d.get("y", 0)))
        src = str(d.get("src", ""))
        if src:
            pix = QPixmap(src)
            if not pix.isNull():
                # Scale to widget box
                w = int(d.get("w", pix.width()))
                h = int(d.get("h", pix.height()))
                self.setPixmap(pix.scaled(w, h,
                    Qt.AspectRatioMode.IgnoreAspectRatio,
                    Qt.TransformationMode.SmoothTransformation))
                return
        # Placeholder
        ph = QPixmap(int(d.get("w", 100)), int(d.get("h", 50)))
        ph.fill(QColor(60, 60, 60, 180))
        self.setPixmap(ph)

    def to_dict(self) -> dict:
        d = dict(self.data(0))
        d["x"] = round(self.pos().x())
        d["y"] = round(self.pos().y())
        return d


_TYPE_TO_ITEM = {
    "value": ValueItem,
    "label": LabelItem,
    "rect": RectItem,
    "line": LineItem,
    "image": ImageItem,
}


# ---------------------------------------------------------------------------
# Designer view
# ---------------------------------------------------------------------------


class LvtDesigner(QGraphicsView):
    selection_changed = pyqtSignal(object)   # widget dict or None
    doc_changed = pyqtSignal()

    def __init__(self, canvas_w: int = LVT_CANVAS_W,
                 canvas_h: int = LVT_CANVAS_H) -> None:
        super().__init__()
        self.setRenderHint(QPainter.RenderHint.Antialiasing)
        self.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        self.setBackgroundBrush(QColor("#0d0f12"))
        self._canvas_w = canvas_w
        self._canvas_h = canvas_h
        self._scene = QGraphicsScene()
        self._scene.setSceneRect(0, 0, canvas_w, canvas_h)
        self._scene.selectionChanged.connect(self._on_selection_changed)
        self.setScene(self._scene)

        self._bg: QGraphicsRectItem | None = None
        self._grid_lines: list = []
        self._build_canvas_background()

        self._doc: dict = empty_lvt()
        self._doc["width"] = canvas_w
        self._doc["height"] = canvas_h

    def set_canvas_size(self, w: int, h: int) -> None:
        """Resize the design canvas (1024×600 for display layouts,
        1920×1080 for video overlays). Existing widgets keep their
        coordinates; the user may need to nudge them after a resize."""
        self._canvas_w = w
        self._canvas_h = h
        self._scene.setSceneRect(0, 0, w, h)
        self._build_canvas_background()
        self._doc["width"] = w
        self._doc["height"] = h

    def _build_canvas_background(self) -> None:
        # Remove old backdrop + grid
        if self._bg is not None:
            self._scene.removeItem(self._bg)
            self._bg = None
        for ln in self._grid_lines:
            self._scene.removeItem(ln)
        self._grid_lines = []

        self._bg = QGraphicsRectItem(0, 0, self._canvas_w, self._canvas_h)
        self._bg.setBrush(QColor("#000000"))
        self._bg.setPen(QPen(QColor("#3a3f48"), 1))
        self._bg.setZValue(-100)
        self._scene.addItem(self._bg)

        for x in range(0, self._canvas_w + 1, GRID_PX):
            ln = self._scene.addLine(x, 0, x, self._canvas_h,
                                     QPen(QColor(50, 55, 65), 1))
            ln.setZValue(-50)
            self._grid_lines.append(ln)
        for y in range(0, self._canvas_h + 1, GRID_PX):
            ln = self._scene.addLine(0, y, self._canvas_w, y,
                                     QPen(QColor(50, 55, 65), 1))
            ln.setZValue(-50)
            self._grid_lines.append(ln)

    # ── public API ─────────────────────────────────────────────────────

    def set_doc(self, doc: dict) -> None:
        self._doc = copy.deepcopy(doc)
        self._rebuild_scene()

    def doc(self) -> dict:
        # Refresh widgets from current item geometry
        widgets: list[dict] = []
        for it in self._scene.items():
            if isinstance(it, _BaseItem):
                widgets.append(it.to_dict())
        # Items come in z-order from front to back; reverse so save matches
        # add-order.
        widgets.reverse()
        out = dict(self._doc)
        out["widgets"] = widgets
        return out

    def add_widget(self, default: dict) -> None:
        d = copy.deepcopy(default)
        # Centre new widgets on the canvas by default
        if "x" in d and "y" in d:
            d["x"] = (LVT_CANVAS_W - int(d.get("w", 100))) // 2
            d["y"] = (LVT_CANVAS_H - int(d.get("h", 50))) // 2
        self._add_item_for(d)
        self.doc_changed.emit()

    def remove_selected(self) -> None:
        for it in list(self._scene.selectedItems()):
            self._scene.removeItem(it)
        self.doc_changed.emit()

    def update_selected(self, **patch: Any) -> None:
        sel = self._scene.selectedItems()
        if not sel or not isinstance(sel[0], _BaseItem):
            return
        item = sel[0]
        d = dict(item.data(0))
        d.update(patch)
        item.update_from_dict(d)
        self.doc_changed.emit()
        self.selection_changed.emit(d)

    # ── internals ──────────────────────────────────────────────────────

    def _rebuild_scene(self) -> None:
        # Adopt the doc's canvas size if it differs from the current one.
        doc_w = int(self._doc.get("width", self._canvas_w))
        doc_h = int(self._doc.get("height", self._canvas_h))
        if doc_w != self._canvas_w or doc_h != self._canvas_h:
            self.set_canvas_size(doc_w, doc_h)
        for it in list(self._scene.items()):
            if isinstance(it, _BaseItem):
                self._scene.removeItem(it)
        bg_color = self._doc.get("background")
        if bg_color and self._bg is not None:
            self._bg.setBrush(QColor(bg_color))
        for w in self._doc.get("widgets") or []:
            self._add_item_for(w)
        self.doc_changed.emit()

    def _add_item_for(self, d: dict) -> None:
        cls = _TYPE_TO_ITEM.get(d.get("type", ""))
        if cls is None:
            return
        item = cls(d)
        self._scene.addItem(item)

    def _on_selection_changed(self) -> None:
        sel = self._scene.selectedItems()
        if sel and isinstance(sel[0], _BaseItem):
            d = sel[0].to_dict()
            self.selection_changed.emit(d)
        else:
            self.selection_changed.emit(None)

    def keyPressEvent(self, evt) -> None:  # noqa: ANN001
        if evt.key() == Qt.Key.Key_Delete:
            self.remove_selected()
            evt.accept()
            return
        super().keyPressEvent(evt)

    def mouseReleaseEvent(self, evt) -> None:  # noqa: ANN001
        super().mouseReleaseEvent(evt)
        # After a drag, the moved item's pos has changed — update its dict.
        for it in self._scene.selectedItems():
            if isinstance(it, _BaseItem):
                it.setData(0, it.to_dict())
        self.doc_changed.emit()
        self._on_selection_changed()
