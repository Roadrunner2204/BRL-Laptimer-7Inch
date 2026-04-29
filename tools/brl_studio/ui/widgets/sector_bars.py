"""
Sector-vs-reference bar chart.

For each sector, draws a horizontal bar centred on 0:
    - left of centre (cyan)  = faster than reference
    - right of centre (red)  = slower than reference
The label shows ±M.mmm s.

Identical visual language to the Android app's SectorBars.tsx so the
driver sees the same colour story on both screens.
"""

from __future__ import annotations

from PyQt6.QtCore import Qt, QSize
from PyQt6.QtGui import QColor, QFont, QPainter, QPen
from PyQt6.QtWidgets import QSizePolicy, QWidget

from core.theme import get_palette, theme_bus


class SectorBars(QWidget):
    def __init__(self) -> None:
        super().__init__()
        self._sectors: list[tuple[str, float]] = []  # (label, delta_s)
        self._scale_s = 1.0  # full-bar width = ±1.0 s by default
        self._apply_palette(get_palette())
        theme_bus().theme_changed.connect(self._apply_palette)
        self.setMinimumHeight(120)
        self.setSizePolicy(QSizePolicy.Policy.Expanding,
                           QSizePolicy.Policy.Fixed)

    def _apply_palette(self, p: dict) -> None:
        self._clr_faster = QColor(p["ok"])
        self._clr_slower = QColor(p["danger"])
        self._clr_bg     = QColor(p["surface"])
        self._clr_axis   = QColor(p["border"])
        self._clr_text   = QColor(p["text"])
        self.update()

    def set_sectors(self, sectors: list[tuple[str, float]],
                    scale_s: float | None = None) -> None:
        self._sectors = sectors
        if scale_s is not None and scale_s > 0:
            self._scale_s = scale_s
        else:
            # auto-scale to the largest abs-delta, with a 0.5 s floor
            largest = max((abs(d) for _, d in sectors), default=0.0)
            self._scale_s = max(0.5, largest * 1.2)
        self.update()

    def sizeHint(self) -> QSize:
        return QSize(600, 30 + 30 * max(1, len(self._sectors)))

    def paintEvent(self, _evt) -> None:  # noqa: ANN001 — Qt sig
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.fillRect(self.rect(), self._clr_bg)

        if not self._sectors:
            p.setPen(self._clr_text)
            p.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter,
                       "Keine Sektor-Daten")
            return

        w = self.width()
        h = self.height()
        margin_x = 80
        bar_h = 18
        row_h = (h - 12) // len(self._sectors)
        center_x = w // 2

        # Y axis (centre line)
        p.setPen(QPen(self._clr_axis, 1, Qt.PenStyle.SolidLine))
        p.drawLine(center_x, 4, center_x, h - 4)

        font = QFont()
        font.setPointSize(10)
        p.setFont(font)

        for i, (label, delta) in enumerate(self._sectors):
            y = 6 + i * row_h
            cy = y + (row_h - bar_h) // 2

            # Bar
            half_avail = (w - 2 * margin_x) // 2
            bar_w = int(min(1.0, abs(delta) / self._scale_s) * half_avail)
            color = self._clr_faster if delta < 0 else self._clr_slower
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(color)
            if delta < 0:
                p.drawRect(center_x - bar_w, cy, bar_w, bar_h)
            else:
                p.drawRect(center_x, cy, bar_w, bar_h)

            # Sector label (left)
            p.setPen(self._clr_text)
            p.drawText(8, cy, margin_x - 16, bar_h,
                       Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                       label)

            # Delta text (right)
            txt = f"{delta:+.3f} s" if delta != 0 else "±0.000 s"
            p.drawText(w - margin_x + 8, cy, margin_x - 16, bar_h,
                       Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                       txt)

        p.end()
