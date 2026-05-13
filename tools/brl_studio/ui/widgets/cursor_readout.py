"""
Cursor readout — per-lap raw channel values at the current chart cursor.

Mirrors the column-per-channel readout the Android app shows below its
chart, but expanded with one row per selected lap so a 4-lap compare is
all visible at a glance. Linear-interpolates each channel by distance so
the values are smooth between sample points.

Usage from AnalyseView:
    self.readout = CursorReadout()
    ...
    self.readout.set_layout(selected_pairs, ref_cache_idx, self._delta_for)
    ...
    self.readout.set_cursor(distance_m)

`selected_pairs` is the list of (cache_idx, _LapView) tuples that the
chart is currently drawing — same shape AnalyseView already uses for
`_selected()`. `delta_provider` is a callable taking a cache index and
returning the per-point delta list in seconds (None / [] if none).
"""

from __future__ import annotations

import bisect
from typing import Callable

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import QGridLayout, QLabel, QWidget

from core.theme import get_palette


def _interp(xs: list[float], ys: list[float], x: float) -> float | None:
    """Linear interpolation. Returns None if arrays don't line up."""
    if not xs or len(xs) != len(ys):
        return None
    if x <= xs[0]:
        return ys[0]
    if x >= xs[-1]:
        return ys[-1]
    i = bisect.bisect_left(xs, x)
    x0, x1 = xs[i - 1], xs[i]
    y0, y1 = ys[i - 1], ys[i]
    if x1 == x0:
        return y0
    t = (x - x0) / (x1 - x0)
    return y0 + t * (y1 - y0)


def _fmt_time(ms: float | None) -> str:
    if ms is None:
        return "—"
    s = max(0.0, ms) / 1000.0
    m, sec = divmod(s, 60.0)
    return f"{int(m)}:{sec:06.3f}"


def _fmt_delta(s: float | None) -> str:
    if s is None:
        return "—"
    sign = "+" if s >= 0 else "−"
    return f"{sign}{abs(s):.3f}"


class CursorReadout(QWidget):
    HEADERS = ("Lap", "v [km/h]", "G-lat", "G-long", "Δ [s]", "Zeit")
    HEADER_STRETCH = (1, 2, 1, 1, 2, 2)
    MAX_ROWS = 6   # matches LAP_COLORS palette length

    def __init__(self) -> None:
        super().__init__()
        self._pairs: list[tuple[int, object]] = []     # (cache_idx, _LapView)
        self._ref_idx: int = -1
        self._delta_provider: Callable[[int], list[float] | None] = lambda _i: None
        self._dist: float = 0.0

        pal = get_palette()
        self.setStyleSheet(
            f"CursorReadout {{ background: {pal.get('surface', '#16181d')};"
            f"                  border-radius: 6px; }}"
        )

        grid = QGridLayout(self)
        grid.setContentsMargins(10, 6, 10, 6)
        grid.setHorizontalSpacing(14)
        grid.setVerticalSpacing(2)

        # Header row
        dim = pal.get("dim", "#7A8FA6")
        hdr_style = (f"color: {dim}; font-weight: bold; font-size: 10px;"
                     f" letter-spacing: 0.5px;")
        for col, (txt, stretch) in enumerate(zip(self.HEADERS, self.HEADER_STRETCH)):
            l = QLabel(txt.upper())
            l.setStyleSheet(hdr_style)
            l.setAlignment(Qt.AlignmentFlag.AlignLeft if col == 0
                           else Qt.AlignmentFlag.AlignRight)
            grid.addWidget(l, 0, col)
            grid.setColumnStretch(col, stretch)

        # Pre-built rows -- updated in place on every cursor move so a
        # 30 Hz scrub doesn't churn the layout.
        self._cells: list[list[QLabel]] = []
        for r in range(1, self.MAX_ROWS + 1):
            row_cells: list[QLabel] = []
            for col in range(len(self.HEADERS)):
                lbl = QLabel("")
                lbl.setAlignment(Qt.AlignmentFlag.AlignLeft if col == 0
                                 else Qt.AlignmentFlag.AlignRight)
                grid.addWidget(lbl, r, col)
                row_cells.append(lbl)
            self._cells.append(row_cells)

        # Footer
        self._dist_lbl = QLabel("Distanz: —")
        self._dist_lbl.setStyleSheet(
            f"color: {dim}; font-size: 10px; margin-top: 6px;")
        grid.addWidget(self._dist_lbl, self.MAX_ROWS + 1, 0,
                       1, len(self.HEADERS))

        self.set_layout([], -1, lambda _i: None)

    def set_layout(self, selected_pairs: list[tuple[int, object]],
                   ref_cache_idx: int,
                   delta_provider: Callable[[int], list[float] | None]) -> None:
        self._pairs = list(selected_pairs)[: self.MAX_ROWS]
        self._ref_idx = ref_cache_idx
        self._delta_provider = delta_provider
        for r, cells in enumerate(self._cells):
            visible = r < len(self._pairs)
            for cell in cells:
                cell.setVisible(visible)
        self._refresh()

    def set_cursor(self, distance_m: float) -> None:
        self._dist = distance_m
        self._dist_lbl.setText(f"Distanz: {distance_m:.0f} m")
        self._refresh()

    def _refresh(self) -> None:
        for r, (cache_idx, lv) in enumerate(self._pairs):
            cells = self._cells[r]
            ch = lv.channels                # type: ignore[attr-defined]
            xs = ch.distance_m

            speed = _interp(xs, ch.speed_kmh, self._dist)
            glat  = _interp(xs, ch.g_lat,    self._dist)
            glong = _interp(xs, ch.g_long,   self._dist)

            # Lap time at this distance: the per-point lap_ms is exactly
            # what we want -- it already encodes "time since lap start"
            # so interpolating it gives the in-lap clock at the cursor.
            t_ms_arr = [float(p.lap_ms) for p in lv.lap.points]   # type: ignore[attr-defined]
            t_ms = _interp(xs, t_ms_arr, self._dist)

            is_ref = (cache_idx == self._ref_idx)
            delta_s: float | None = None
            if not is_ref:
                d = self._delta_provider(cache_idx)
                if d:
                    delta_s = _interp(xs, d, self._dist)

            color = lv.color                                       # type: ignore[attr-defined]
            weight = "700" if is_ref else "500"
            style = (f"color: {color}; font-family: 'Consolas','Menlo',monospace;"
                     f" font-size: 12px; font-weight: {weight};")
            for cell in cells:
                cell.setStyleSheet(style)

            num = lv.lap.number                                    # type: ignore[attr-defined]
            cells[0].setText(f"R{num}{'  ★' if is_ref else ''}")
            cells[1].setText("—" if speed is None else f"{speed:.0f}")
            cells[2].setText("—" if glat  is None else f"{glat:+.2f}")
            cells[3].setText("—" if glong is None else f"{glong:+.2f}")
            cells[4].setText("—" if (delta_s is None or is_ref)
                              else _fmt_delta(delta_s))
            cells[5].setText(_fmt_time(t_ms))
