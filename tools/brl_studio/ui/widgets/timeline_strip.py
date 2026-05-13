"""
Timeline strip — compact overview of the full lap distance with the
chart's current zoom window highlighted as a draggable rectangle.

Click outside the rect → centre rect on the click. Drag the rect → pan
zoom window. Drag a rect edge → resize. Emits `range_changed(lo, hi)`
in data coordinates (metres) whenever the user moves things; AnalyseView
wires that to `chart.set_x_range(lo, hi)`.
"""

from __future__ import annotations

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QColor, QMouseEvent, QPainter, QPen
from PyQt6.QtWidgets import QWidget

from core.theme import get_palette, theme_bus


class TimelineStrip(QWidget):
    range_changed = pyqtSignal(float, float)   # lo, hi (data units, metres)

    EDGE_GRAB    = 6     # px from rect edge counts as a resize-handle
    MIN_SPAN_PX  = 14    # don't shrink the rect below this on-screen
    BAR_PAD_Y    = 4

    def __init__(self) -> None:
        super().__init__()
        self.setFixedHeight(34)
        self.setMouseTracking(True)
        self._full: tuple[float, float] = (0.0, 1.0)
        self._view: tuple[float, float] = (0.0, 1.0)
        self._cursor: float | None = None

        # Track-preview polylines (optional). Kept simple: small list of
        # (color, [(x_data, y_normalised), ...]) -- we just plot them
        # behind the rect so the user has visual orientation.
        self._previews: list[tuple[str, list[tuple[float, float]]]] = []

        self._drag_mode: str | None = None   # 'pan' | 'lo' | 'hi' | None
        self._drag_anchor_x: int = 0
        self._drag_view0: tuple[float, float] = (0.0, 1.0)

        theme_bus().theme_changed.connect(lambda _p: self.update())

    # ── public API ─────────────────────────────────────────────────────
    def set_full_range(self, lo: float, hi: float) -> None:
        if hi <= lo:
            return
        self._full = (lo, hi)
        # If view was empty / wider than new range, snap to full
        if self._view[1] <= self._view[0] or (
            self._view[0] <= lo and self._view[1] >= hi
        ):
            self._view = (lo, hi)
        self.update()

    def set_view_range(self, lo: float, hi: float) -> None:
        if hi <= lo:
            return
        self._view = (lo, hi)
        self.update()

    def set_cursor(self, x: float | None) -> None:
        self._cursor = x
        self.update()

    def set_previews(self,
                     previews: list[tuple[str, list[tuple[float, float]]]]
                     ) -> None:
        """Optional decoration: small overview traces drawn behind the
        view-window rect. y values must already be normalised to [0,1]."""
        self._previews = previews
        self.update()

    # ── coordinate helpers ────────────────────────────────────────────
    def _x_to_data(self, px: int) -> float:
        lo, hi = self._full
        if self.width() <= 0 or hi <= lo:
            return lo
        return lo + (px / self.width()) * (hi - lo)

    def _data_to_x(self, d: float) -> int:
        lo, hi = self._full
        if hi <= lo:
            return 0
        return int((d - lo) / (hi - lo) * self.width())

    def _view_rect_px(self) -> tuple[int, int]:
        return self._data_to_x(self._view[0]), self._data_to_x(self._view[1])

    # ── paint ─────────────────────────────────────────────────────────
    def paintEvent(self, _evt) -> None:                    # noqa: ANN001
        p = QPainter(self)
        pal = get_palette()
        bg          = QColor(pal.get("surface2", "#202329"))
        rect_bg     = QColor(pal.get("highlight", "#0A1A2E"))
        rect_border = QColor(pal.get("accent", "#0096FF"))
        cursor_clr  = QColor(pal.get("text", "#fff"))
        dim         = QColor(pal.get("textDark", "#3A4A5C"))

        # Background
        p.fillRect(self.rect(), bg)

        # Optional previews
        if self._previews:
            inner_top = self.BAR_PAD_Y
            inner_h = self.height() - 2 * self.BAR_PAD_Y
            for color, pts in self._previews:
                if len(pts) < 2:
                    continue
                pen = QPen(QColor(color)); pen.setWidthF(1.0); pen.setCosmetic(True)
                p.setPen(pen)
                last_x = self._data_to_x(pts[0][0])
                last_y = inner_top + int((1.0 - pts[0][1]) * inner_h)
                for px_d, py_n in pts[1:]:
                    nx = self._data_to_x(px_d)
                    ny = inner_top + int((1.0 - py_n) * inner_h)
                    p.drawLine(last_x, last_y, nx, ny)
                    last_x, last_y = nx, ny

        # Frame around the strip
        p.setPen(QPen(dim, 1))
        p.drawRect(self.rect().adjusted(0, 0, -1, -1))

        # View-window rect
        x_lo, x_hi = self._view_rect_px()
        if x_hi - x_lo < 2:
            x_hi = x_lo + 2
        rect = self.rect().adjusted(0, self.BAR_PAD_Y, 0, -self.BAR_PAD_Y)
        rect.setLeft(x_lo)
        rect.setRight(x_hi)
        fill = QColor(rect_bg); fill.setAlpha(160)
        p.fillRect(rect, fill)
        p.setPen(QPen(rect_border, 1))
        p.drawRect(rect)

        # Cursor line
        if self._cursor is not None:
            cx = self._data_to_x(self._cursor)
            p.setPen(QPen(cursor_clr, 1, Qt.PenStyle.DashLine))
            p.drawLine(cx, 0, cx, self.height())

    # ── mouse ─────────────────────────────────────────────────────────
    def mousePressEvent(self, e: QMouseEvent) -> None:
        if e.button() != Qt.MouseButton.LeftButton:
            return
        x = e.pos().x()
        x_lo, x_hi = self._view_rect_px()

        if abs(x - x_lo) <= self.EDGE_GRAB:
            self._drag_mode = "lo"
        elif abs(x - x_hi) <= self.EDGE_GRAB:
            self._drag_mode = "hi"
        elif x_lo < x < x_hi:
            self._drag_mode = "pan"
        else:
            # Click outside rect → centre rect on click position
            span = self._view[1] - self._view[0]
            c = self._x_to_data(x)
            self._clamp_and_emit(c - span / 2, c + span / 2)
            return

        self._drag_anchor_x = x
        self._drag_view0 = self._view

    def mouseMoveEvent(self, e: QMouseEvent) -> None:
        if self._drag_mode is None:
            x = e.pos().x()
            x_lo, x_hi = self._view_rect_px()
            if (abs(x - x_lo) <= self.EDGE_GRAB
                    or abs(x - x_hi) <= self.EDGE_GRAB):
                self.setCursor(Qt.CursorShape.SizeHorCursor)
            elif x_lo < x < x_hi:
                self.setCursor(Qt.CursorShape.OpenHandCursor)
            else:
                self.setCursor(Qt.CursorShape.PointingHandCursor)
            return

        full_lo, full_hi = self._full
        scale = (full_hi - full_lo) / max(1, self.width())
        dx = (e.pos().x() - self._drag_anchor_x) * scale
        v0_lo, v0_hi = self._drag_view0

        if self._drag_mode == "pan":
            self._clamp_and_emit(v0_lo + dx, v0_hi + dx)
        elif self._drag_mode == "lo":
            new_lo = min(v0_lo + dx, v0_hi - self.MIN_SPAN_PX * scale)
            self._clamp_and_emit(new_lo, v0_hi)
        elif self._drag_mode == "hi":
            new_hi = max(v0_hi + dx, v0_lo + self.MIN_SPAN_PX * scale)
            self._clamp_and_emit(v0_lo, new_hi)

    def mouseReleaseEvent(self, _e: QMouseEvent) -> None:
        self._drag_mode = None

    def _clamp_and_emit(self, lo: float, hi: float) -> None:
        full_lo, full_hi = self._full
        span = hi - lo
        if span >= (full_hi - full_lo):
            lo, hi = full_lo, full_hi
        else:
            if lo < full_lo:
                lo, hi = full_lo, full_lo + span
            if hi > full_hi:
                hi, lo = full_hi, full_hi - span
        self._view = (lo, hi)
        self.update()
        self.range_changed.emit(lo, hi)
