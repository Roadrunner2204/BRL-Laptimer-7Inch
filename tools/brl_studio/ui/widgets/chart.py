"""
Multi-channel telemetry chart built on pyqtgraph.

One stacked plot per active channel, sharing an X axis (distance in
metres). A draggable vertical cursor line emits `cursor_moved(distance_m)`
so the map widget and sector bars can sync to the same point.

Channels are added by calling `set_traces(name, [(label, color, x[], y[]),
...])`. We rebuild the plot rather than incrementally update — sessions
are tens-of-thousands of points at most, throwaway PlotItems are cheap,
and rebuild keeps the y-axis auto-ranges correct without bookkeeping.
"""

from __future__ import annotations

from dataclasses import dataclass

import pyqtgraph as pg
from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QColor
from PyQt6.QtWidgets import QVBoxLayout, QWidget

from core.theme import get_palette, theme_bus

# Lap colours — first entry is reserved for the reference lap.
LAP_COLORS = [
    "#00CFFF",  # ref:  cyan
    "#FF8A2A",  # 2:    orange
    "#FFE042",  # 3:    yellow
    "#FF4FA8",  # 4:    magenta
    "#7CFF6B",  # 5:    green
    "#B36CFF",  # 6:    purple
]


@dataclass
class Trace:
    label: str          # e.g. "Lap 3 (ref)"
    color: str          # hex
    x: list[float]      # distance in m
    y: list[float]


class TelemetryChart(QWidget):
    cursor_moved = pyqtSignal(float)   # distance_m

    def __init__(self) -> None:
        super().__init__()
        pg.setConfigOptions(antialias=True, useOpenGL=False)
        p = get_palette()
        pg.setConfigOption("background", p["bg"])
        pg.setConfigOption("foreground", p["text"])

        self._layout_w = pg.GraphicsLayoutWidget()
        self._layout_w.setBackground(p["bg"])

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.addWidget(self._layout_w)

        self._plots: list[pg.PlotItem] = []
        self._cursors: list[pg.InfiniteLine] = []
        self._suppress_cursor_signal = False
        # Remember last channels so we can re-render on theme change.
        self._last_channels: list[tuple[str, str, list[Trace]]] = []
        theme_bus().theme_changed.connect(self._on_theme)

    def clear(self) -> None:
        self._layout_w.clear()
        self._plots.clear()
        self._cursors.clear()

    def set_channels(self, channels: list[tuple[str, str, list[Trace]]]) -> None:
        """
        channels: list of (channel_name, y_unit_label, [Trace, ...])
        Builds one stacked plot per channel, all sharing the X axis.
        """
        self._last_channels = channels
        self.clear()

        first_plot: pg.PlotItem | None = None
        for i, (name, unit, traces) in enumerate(channels):
            plot: pg.PlotItem = self._layout_w.addPlot(row=i, col=0)
            plot.showGrid(x=True, y=True, alpha=0.15)
            plot.setLabel("left", name, units=unit)
            if i < len(channels) - 1:
                plot.hideAxis("bottom")
            else:
                plot.setLabel("bottom", "Distanz", units="m")
            plot.addLegend(offset=(8, 8))

            for tr in traces:
                pen = pg.mkPen(QColor(tr.color), width=2)
                plot.plot(tr.x, tr.y, pen=pen, name=tr.label)

            p_pal = get_palette()
            cursor = pg.InfiniteLine(
                angle=90,
                movable=(i == 0),  # only the top cursor is the source-of-truth
                pen=pg.mkPen(p_pal["text"], width=1,
                             style=Qt.PenStyle.DashLine),
            )
            plot.addItem(cursor)
            self._plots.append(plot)
            self._cursors.append(cursor)

            if first_plot is None:
                first_plot = plot
            else:
                plot.setXLink(first_plot)

        # Wire the top cursor as the user-facing handle; mirror to all
        # others. We re-broadcast its position so AnalyseView can route to
        # the map.
        if self._cursors:
            self._cursors[0].sigPositionChanged.connect(self._on_top_cursor_moved)

    def set_cursor(self, distance_m: float) -> None:
        if not self._cursors:
            return
        self._suppress_cursor_signal = True
        for c in self._cursors:
            c.setPos(distance_m)
        self._suppress_cursor_signal = False

    # ── internals ───────────────────────────────────────────────────────

    def _on_top_cursor_moved(self) -> None:
        if self._suppress_cursor_signal or not self._cursors:
            return
        x = float(self._cursors[0].value())
        # Mirror to the secondary plots without re-emitting.
        self._suppress_cursor_signal = True
        for c in self._cursors[1:]:
            c.setPos(x)
        self._suppress_cursor_signal = False
        self.cursor_moved.emit(x)

    def _on_theme(self, palette: dict) -> None:
        """Repaint on theme switch — pyqtgraph caches PlotItem styles, so
        we rebuild from `_last_channels` rather than poke each item."""
        pg.setConfigOption("background", palette["bg"])
        pg.setConfigOption("foreground", palette["text"])
        self._layout_w.setBackground(palette["bg"])
        # Snapshot cursor position so it survives the rebuild
        cursor_x: float | None = None
        if self._cursors:
            cursor_x = float(self._cursors[0].value())
        if self._last_channels:
            self.set_channels(self._last_channels)
            if cursor_x is not None:
                self.set_cursor(cursor_x)
