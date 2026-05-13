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
    cursor_moved = pyqtSignal(float)            # distance_m
    x_range_changed = pyqtSignal(float, float)  # lo, hi (metres)

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
            # Cursor: bright accent colour + thicker so it's obvious the
            # line is interactive. EVERY cursor is movable -- grabbing
            # any one of them drags all of them in lock-step (mirrored
            # via sigPositionChanged below).
            cursor_color = p_pal.get("accent", "#0096FF")
            cursor = pg.InfiniteLine(
                angle=90,
                movable=True,
                pen=pg.mkPen(cursor_color, width=2,
                             style=Qt.PenStyle.SolidLine),
                hoverPen=pg.mkPen(cursor_color, width=3),
            )
            plot.addItem(cursor)
            self._plots.append(plot)
            self._cursors.append(cursor)

            # Click anywhere on the plot → cursor jumps there. Pyqtgraph's
            # default behaviour is mouse-pan, so we hook the SCENE click
            # signal and only act if it wasn't a drag.
            plot.scene().sigMouseClicked.connect(self._on_scene_clicked)

            if first_plot is None:
                first_plot = plot
            else:
                plot.setXLink(first_plot)

        # Every cursor is its own drag handle. Each one's position-change
        # mirrors to the others and re-broadcasts as `cursor_moved` so
        # AnalyseView can route to the map / readout / video. The
        # `_suppress_cursor_signal` guard prevents the mirror writes from
        # cascading into a feedback loop.
        for c in self._cursors:
            c.sigPositionChanged.connect(
                lambda src=c: self._on_any_cursor_moved(src)
            )

        # Mouse-driven pan/zoom on the chart needs to flow back to the
        # mini-strip + readout, otherwise dragging in pyqtgraph leaves the
        # overview stale. Hook the first plot's ViewBox X-range signal.
        if first_plot is not None:
            first_plot.vb.sigXRangeChanged.connect(self._on_view_x_changed)

    def set_cursor(self, distance_m: float) -> None:
        if not self._cursors:
            return
        self._suppress_cursor_signal = True
        for c in self._cursors:
            c.setPos(distance_m)
        self._suppress_cursor_signal = False

    # ── X-axis range control (timeline-strip + zoom buttons) ────────────

    def set_x_range(self, lo: float, hi: float) -> None:
        if not self._plots or hi <= lo:
            return
        # All plots share an X-link, so setting the first propagates.
        # padding=0 keeps the view exactly at the requested bounds.
        self._suppress_view_signal = True
        try:
            self._plots[0].setXRange(lo, hi, padding=0)
        finally:
            self._suppress_view_signal = False

    def get_x_range(self) -> tuple[float, float] | None:
        if not self._plots:
            return None
        xmin, xmax = self._plots[0].vb.viewRange()[0]
        return float(xmin), float(xmax)

    def get_x_full_range(self) -> tuple[float, float]:
        """Smallest / largest x across all currently-shown traces."""
        lo, hi = float("inf"), float("-inf")
        for _name, _unit, traces in self._last_channels:
            for tr in traces:
                if tr.x:
                    lo = min(lo, tr.x[0])
                    hi = max(hi, tr.x[-1])
        if not (lo < hi):
            return (0.0, 1.0)
        return (lo, hi)

    # ── internals ───────────────────────────────────────────────────────

    def _on_scene_clicked(self, evt) -> None:               # noqa: ANN001
        """Single-click on any chart → move cursor to that x."""
        if not self._plots or not self._cursors:
            return
        # Ignore drags / right-button / multi-click
        try:
            if evt.button() != Qt.MouseButton.LeftButton:
                return
            if not evt.isAccepted() and evt.double():
                return
        except Exception:        # noqa: BLE001
            pass
        pos = evt.scenePos()
        # Map scene → first plot's data coords
        vb = self._plots[0].vb
        if vb is None or not self._plots[0].sceneBoundingRect().contains(pos):
            # Click outside the first plot's bounds — try the others
            hit = False
            for pl in self._plots[1:]:
                if pl.sceneBoundingRect().contains(pos):
                    vb = pl.vb
                    hit = True
                    break
            if not hit and not self._plots[0].sceneBoundingRect().contains(pos):
                return
        try:
            mapped = vb.mapSceneToView(pos)
        except Exception:        # noqa: BLE001
            return
        self.set_cursor(float(mapped.x()))
        # set_cursor doesn't broadcast — emit so AnalyseView routes to map
        self.cursor_moved.emit(float(mapped.x()))

    def _on_view_x_changed(self, _vb, range_xy) -> None:    # noqa: ANN001
        # Fired by pyqtgraph on every wheel-scroll / mouse-drag. Suppress
        # while we set the range from Python so we don't ping-pong with
        # the timeline-strip.
        if getattr(self, "_suppress_view_signal", False):
            return
        try:
            lo = float(range_xy[0]); hi = float(range_xy[1])
        except (TypeError, ValueError):
            return
        self.x_range_changed.emit(lo, hi)

    def _on_any_cursor_moved(self, source) -> None:    # noqa: ANN001
        if self._suppress_cursor_signal or not self._cursors:
            return
        x = float(source.value())
        # Mirror to the OTHER cursors without retriggering ourselves.
        self._suppress_cursor_signal = True
        try:
            for c in self._cursors:
                if c is not source:
                    c.setPos(x)
        finally:
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
