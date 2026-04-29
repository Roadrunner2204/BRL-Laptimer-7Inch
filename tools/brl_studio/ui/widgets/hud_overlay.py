"""
Transparent HUD overlay drawn on top of the video player.

Reads telemetry samples (gps, obd, lap markers) at the player's current
recording-time position and renders:

    ┌──────────────────────────────────────┐
    │ 142 km/h            Lap 3 / 1:34.821 │
    │                     Δ -0.42 s        │
    │                                      │
    │                                      │
    │ ◯ G-meter          Throttle ▰▰▰▰▱   │
    │   (lat/long g)     Brake     ▰▱▱▱▱   │
    └──────────────────────────────────────┘

The overlay is a sibling widget of the VideoPlayer with WA_TranslucentBackground
+ WA_TransparentForMouseEvents so it composites on top without intercepting
input. paintEvent is cheap — we only repaint when the player position moves
or the telemetry source changes.
"""

from __future__ import annotations

import math

from PyQt6.QtCore import QPointF, QRectF, Qt
from PyQt6.QtGui import (
    QColor,
    QFont,
    QFontMetrics,
    QPainter,
    QPainterPath,
    QPen,
)
from PyQt6.QtWidgets import QWidget

from core.telemetry import Telemetry


class HudOverlay(QWidget):
    def __init__(self) -> None:
        super().__init__()
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self._telemetry: Telemetry | None = None
        self._ms: int = 0
        self._ref_total_ms: int = 0   # for delta vs reference (set externally)
        self._visible_speed = True
        self._visible_lap = True
        self._visible_g = True
        self._visible_pedals = True

    # ── public API ─────────────────────────────────────────────────────

    def set_telemetry(self, t: Telemetry | None) -> None:
        self._telemetry = t
        self.update()

    def set_position_ms(self, ms: int) -> None:
        self._ms = max(0, ms)
        self.update()

    def set_reference_lap_ms(self, ms: int) -> None:
        """Total time of the reference lap, used for live delta display."""
        self._ref_total_ms = max(0, ms)
        self.update()

    def set_visibility(self, *, speed: bool | None = None,
                       lap: bool | None = None,
                       g: bool | None = None,
                       pedals: bool | None = None) -> None:
        if speed is not None:
            self._visible_speed = speed
        if lap is not None:
            self._visible_lap = lap
        if g is not None:
            self._visible_g = g
        if pedals is not None:
            self._visible_pedals = pedals
        self.update()

    # ── paint ──────────────────────────────────────────────────────────

    def paintEvent(self, _evt) -> None:  # noqa: ANN001
        if self._telemetry is None:
            return
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setRenderHint(QPainter.RenderHint.TextAntialiasing)

        gps = self._telemetry.gps_at(self._ms)
        obd = self._telemetry.obd_at(self._ms)
        lap_no, lap_ms = self._telemetry.current_lap(self._ms)

        if self._visible_speed and gps is not None:
            self._draw_speed(p, gps.speed_kmh)

        if self._visible_lap:
            self._draw_lap(p, lap_no, lap_ms)

        if self._visible_g and gps is not None:
            self._draw_gmeter(p, obd, gps)

        if self._visible_pedals and obd is not None:
            self._draw_pedals(p, obd.throttle_pct, obd.brake_pct)

        p.end()

    # ── pieces ─────────────────────────────────────────────────────────

    @staticmethod
    def _outlined_text(p: QPainter, text: str, x: float, y: float,
                       font: QFont, fill: QColor,
                       outline: QColor = QColor(0, 0, 0, 220),
                       outline_w: float = 2.5) -> None:
        path = QPainterPath()
        path.addText(QPointF(x, y), font, text)
        p.setPen(QPen(outline, outline_w))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawPath(path)
        p.fillPath(path, fill)

    def _draw_speed(self, p: QPainter, speed_kmh: float) -> None:
        f_big = QFont("Segoe UI", 64, QFont.Weight.Bold)
        f_unit = QFont("Segoe UI", 18, QFont.Weight.Bold)
        text_speed = f"{int(round(speed_kmh))}"
        fm_big = QFontMetrics(f_big)
        x = 24
        y = 24 + fm_big.ascent()
        self._outlined_text(p, text_speed, x, y, f_big, QColor("#ffffff"))
        self._outlined_text(p, "km/h", x + fm_big.horizontalAdvance(text_speed) + 8,
                            y, f_unit, QColor("#cccccc"))

    def _draw_lap(self, p: QPainter, lap_no: int, lap_ms: int) -> None:
        m, s = divmod(lap_ms / 1000.0, 60.0)
        time_s = f"{int(m)}:{s:06.3f}"
        f_lap = QFont("Segoe UI", 18, QFont.Weight.Bold)
        f_time = QFont("Segoe UI", 36, QFont.Weight.Bold)
        fm_t = QFontMetrics(f_time)
        fm_l = QFontMetrics(f_lap)
        right_pad = 24
        top_y = 24

        x_t = self.width() - right_pad - fm_t.horizontalAdvance(time_s)
        y_t = top_y + fm_t.ascent()
        self._outlined_text(p, time_s, x_t, y_t, f_time, QColor("#ffffff"))

        lap_label = f"Lap {lap_no}"
        x_l = self.width() - right_pad - fm_l.horizontalAdvance(lap_label)
        y_l = y_t - fm_t.ascent() - 4 + fm_l.ascent()
        self._outlined_text(p, lap_label, x_l, y_l, f_lap, QColor("#cccccc"))

        # Live delta vs reference (if known)
        if self._ref_total_ms > 0:
            # crude live delta: assume linear ramp — replace with per-position
            # delta when the chart-driven cursor is hooked up
            est_full = max(1, lap_ms)
            scale = self._ref_total_ms / est_full if est_full > 0 else 1.0
            delta_s = (lap_ms - self._ref_total_ms * (lap_ms / est_full)) / 1000.0
            # Above is intentionally a placeholder; real per-point delta
            # comes from analysis.delta_vs_reference() and is fed via
            # set_external_delta_s(). Don't trust the number until hooked.
            # We still render *something* so the layout is visible.
            txt = f"Δ {delta_s:+.2f} s"
            f_d = QFont("Segoe UI", 22, QFont.Weight.Bold)
            fm_d = QFontMetrics(f_d)
            x_d = self.width() - right_pad - fm_d.horizontalAdvance(txt)
            y_d = y_t + fm_d.ascent() + 8
            color = QColor("#00CC66") if delta_s < 0 else QColor("#E53935")
            self._outlined_text(p, txt, x_d, y_d, f_d, color)

    def _draw_gmeter(self, p: QPainter, obd, gps) -> None:  # noqa: ANN001
        """G-meter circle bottom-left. lateral g (~steering) on x, longitudinal g on y."""
        size = 140
        margin = 24
        cx = margin + size / 2
        cy = self.height() - margin - size / 2
        # Background ring
        ring = QPen(QColor(255, 255, 255, 180), 2)
        p.setPen(ring)
        p.setBrush(QColor(0, 0, 0, 110))
        p.drawEllipse(QRectF(cx - size / 2, cy - size / 2, size, size))
        # Cross hairs
        p.setPen(QPen(QColor(255, 255, 255, 110), 1, Qt.PenStyle.DashLine))
        p.drawLine(int(cx - size / 2), int(cy), int(cx + size / 2), int(cy))
        p.drawLine(int(cx), int(cy - size / 2), int(cx), int(cy + size / 2))

        # 1.5g full-scale
        scale = (size / 2) / 1.5

        # Heuristic g-values: longitudinal from throttle/brake (very rough),
        # lateral from steering. Only used until firmware ships real IMU data.
        g_long = 0.0
        g_lat = 0.0
        if obd is not None:
            g_long = (obd.throttle_pct - obd.brake_pct) / 100.0
            g_lat = (obd.steering_deg / 540.0)  # ±540° lock, normalised
        gx = max(-1.5, min(1.5, g_lat)) * scale
        gy = -max(-1.5, min(1.5, g_long)) * scale  # inverted — accel up
        dot_r = 8
        p.setPen(QPen(QColor("#ffffff"), 2))
        p.setBrush(QColor("#2d6cdf"))
        p.drawEllipse(QPointF(cx + gx, cy + gy), dot_r, dot_r)

        # Magnitude label
        g_mag = math.hypot(g_lat, g_long)
        f = QFont("Segoe UI", 11, QFont.Weight.Bold)
        self._outlined_text(p, f"{g_mag:.2f} g",
                            cx - size / 2 + 4, cy + size / 2 + 16, f,
                            QColor("#ffffff"))

    def _draw_pedals(self, p: QPainter, throttle_pct: float,
                     brake_pct: float) -> None:
        right_pad = 24
        bar_w = 180
        bar_h = 18
        gap = 8
        bottom = self.height() - 24
        x = self.width() - right_pad - bar_w

        def bar(label: str, value: float, color: QColor, y: int) -> None:
            f = QFont("Segoe UI", 11, QFont.Weight.Bold)
            self._outlined_text(p, label, x - 80, y + bar_h - 4, f,
                                QColor("#ffffff"))
            p.setPen(QPen(QColor(255, 255, 255, 180), 1))
            p.setBrush(QColor(0, 0, 0, 110))
            p.drawRect(x, y, bar_w, bar_h)
            fill = max(0.0, min(100.0, value)) / 100.0
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(color)
            p.drawRect(x + 1, y + 1, int((bar_w - 2) * fill), bar_h - 2)

        bar("Brake", brake_pct, QColor("#E53935"), bottom - bar_h)
        bar("Throttle", throttle_pct, QColor("#2ECC71"),
            bottom - 2 * bar_h - gap)
