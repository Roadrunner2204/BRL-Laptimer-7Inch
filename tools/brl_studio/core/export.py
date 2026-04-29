"""
Export — CSV (Phase 7) + MP4 with embedded HUD overlay (Phase 7).

CSV is straightforward: pull derived channels from analysis.derive_channels()
and write a row per track point.

MP4 path:
    1. Probe ffmpeg (PATH or bundled `ffmpeg/ffmpeg.exe` next to the app).
    2. Render one transparent PNG per output frame using `render_hud_png`,
       which paints the same HUD geometry as the live overlay.
    3. ffmpeg combines: input AVI + PNG sequence → H.264 MP4 with the HUD
       baked in. Default 30 fps matches the cam recording rate.

Long videos generate many PNGs; we use a temp directory and clean up.
A 5-minute clip at 30 fps is ~9000 frames @ ~80 KB each → ~700 MB on
disk transiently — acceptable for desktop. If this becomes a problem we
can switch to a piped ffmpeg input.
"""

from __future__ import annotations

import csv
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from PyQt6.QtCore import QPointF, QRectF, Qt
from PyQt6.QtGui import (
    QBrush,
    QColor,
    QFont,
    QFontMetrics,
    QImage,
    QPainter,
    QPainterPath,
    QPen,
    QPixmap,
)

from .analysis import derive_channels
from .dash_config import (
    FIELD_AN1,
    FIELD_AN2,
    FIELD_AN3,
    FIELD_AN4,
    FIELD_BATTERY,
    FIELD_BESTLAP,
    FIELD_BOOST,
    FIELD_BRAKE,
    FIELD_COOLANT,
    FIELD_DELTA_NUM,
    FIELD_INTAKE,
    FIELD_LAMBDA,
    FIELD_LAP_NR,
    FIELD_LAPTIME,
    FIELD_MAF,
    FIELD_RPM,
    FIELD_SECTOR1,
    FIELD_SECTOR2,
    FIELD_SECTOR3,
    FIELD_SPEED,
    FIELD_STEERING,
    FIELD_THROTTLE,
)
from .session import Lap
from .telemetry import Telemetry


# ---------------------------------------------------------------------------
# CSV
# ---------------------------------------------------------------------------


def export_lap_csv(lap: Lap, dest: Path) -> None:
    """One row per track point: time, distance, speed, g_lat, g_long, lat, lon."""
    ch = derive_channels(lap)
    with dest.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow([
            "lap_ms", "distance_m", "speed_kmh",
            "g_lat", "g_long", "heading_deg",
            "lat", "lon",
        ])
        for i, p in enumerate(lap.points):
            w.writerow([
                p.lap_ms,
                f"{ch.distance_m[i]:.2f}",
                f"{ch.speed_kmh[i]:.2f}",
                f"{ch.g_lat[i]:.3f}",
                f"{ch.g_long[i]:.3f}",
                f"{ch.heading_deg[i]:.1f}",
                f"{p.lat:.7f}",
                f"{p.lon:.7f}",
            ])


# ---------------------------------------------------------------------------
# ffmpeg detection
# ---------------------------------------------------------------------------


def find_ffmpeg() -> Path | None:
    """Look for ffmpeg in PATH, then alongside the running app."""
    in_path = shutil.which("ffmpeg")
    if in_path:
        return Path(in_path)
    here = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent.parent))
    cand = here / "ffmpeg" / ("ffmpeg.exe" if os.name == "nt" else "ffmpeg")
    if cand.exists():
        return cand
    return None


# ---------------------------------------------------------------------------
# HUD frame renderer — same visual contract as ui/widgets/hud_overlay.py.
# Kept here (not imported from the widget) so the export pipeline doesn't
# need a QApplication/QWidget instance.
# ---------------------------------------------------------------------------


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


def render_hud_image(t: Telemetry, ms: int, w: int, h: int) -> QImage:
    img = QImage(w, h, QImage.Format.Format_ARGB32_Premultiplied)
    img.fill(0)  # transparent
    p = QPainter(img)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    p.setRenderHint(QPainter.RenderHint.TextAntialiasing)

    gps = t.gps_at(ms)
    obd = t.obd_at(ms)
    lap_no, lap_ms = t.current_lap(ms)

    # Speed (top-left)
    if gps is not None:
        f_big = QFont("Segoe UI", 64, QFont.Weight.Bold)
        f_unit = QFont("Segoe UI", 18, QFont.Weight.Bold)
        text = f"{int(round(gps.speed_kmh))}"
        fm = QFontMetrics(f_big)
        x, y = 24, 24 + fm.ascent()
        _outlined_text(p, text, x, y, f_big, QColor("#ffffff"))
        _outlined_text(p, "km/h", x + fm.horizontalAdvance(text) + 8, y,
                       f_unit, QColor("#cccccc"))

    # Lap + time (top-right)
    m, s = divmod(lap_ms / 1000.0, 60.0)
    f_lap = QFont("Segoe UI", 18, QFont.Weight.Bold)
    f_time = QFont("Segoe UI", 36, QFont.Weight.Bold)
    fm_t = QFontMetrics(f_time)
    fm_l = QFontMetrics(f_lap)
    time_s = f"{int(m)}:{s:06.3f}"
    right_pad = 24
    top_y = 24
    x_t = w - right_pad - fm_t.horizontalAdvance(time_s)
    y_t = top_y + fm_t.ascent()
    _outlined_text(p, time_s, x_t, y_t, f_time, QColor("#ffffff"))
    lap_label = f"Lap {lap_no}"
    x_l = w - right_pad - fm_l.horizontalAdvance(lap_label)
    y_l = y_t - fm_t.ascent() - 4 + fm_l.ascent()
    _outlined_text(p, lap_label, x_l, y_l, f_lap, QColor("#cccccc"))

    # Pedals (bottom-right)
    if obd is not None:
        bar_w = 180
        bar_h = 18
        gap = 8
        bottom = h - 24
        bx = w - right_pad - bar_w

        def bar(label: str, value: float, color: QColor, by: int) -> None:
            f = QFont("Segoe UI", 11, QFont.Weight.Bold)
            _outlined_text(p, label, bx - 80, by + bar_h - 4, f,
                           QColor("#ffffff"))
            p.setPen(QPen(QColor(255, 255, 255, 180), 1))
            p.setBrush(QColor(0, 0, 0, 110))
            p.drawRect(bx, by, bar_w, bar_h)
            fill = max(0.0, min(100.0, value)) / 100.0
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(color)
            p.drawRect(bx + 1, by + 1, int((bar_w - 2) * fill), bar_h - 2)

        bar("Brake", obd.brake_pct, QColor("#E53935"), bottom - bar_h)
        bar("Throttle", obd.throttle_pct, QColor("#2ECC71"),
            bottom - 2 * bar_h - gap)

    p.end()
    return img


# ---------------------------------------------------------------------------
# MP4 export with embedded HUD
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# Free-form overlay renderer — paints a .lvt-style template onto a transparent
# QImage. The Video-Export view feeds in the user-designed template; the same
# function drives the per-frame burn-in for MP4 export.
# ---------------------------------------------------------------------------


def _resolve_value(field_id: int, t: Telemetry | None, ms: int) -> str:
    """Pull the value the user wants for a 'value' widget out of the
    telemetry stream at recording-time `ms`."""
    if t is None:
        return "—"
    gps = t.gps_at(ms)
    obd = t.obd_at(ms)
    ana = t.analog_at(ms)
    lap_no, lap_ms = t.current_lap(ms)

    if field_id == FIELD_SPEED and gps:
        return f"{int(round(gps.speed_kmh))}"
    if field_id == FIELD_LAPTIME:
        m, s = divmod(lap_ms / 1000.0, 60.0)
        return f"{int(m)}:{s:06.3f}"
    if field_id == FIELD_LAP_NR:
        return str(lap_no)
    if field_id == FIELD_RPM and obd:
        return str(int(obd.rpm))
    if field_id == FIELD_THROTTLE and obd:
        return f"{obd.throttle_pct:.0f}%"
    if field_id == FIELD_BRAKE and obd:
        return f"{obd.brake_pct:.0f}%"
    if field_id == FIELD_BOOST and obd:
        return f"{obd.boost_kpa:.1f}"
    if field_id == FIELD_COOLANT and obd:
        return f"{obd.coolant_c:.0f}°"
    if field_id == FIELD_INTAKE and obd:
        return f"{obd.intake_c:.0f}°"
    if field_id == FIELD_LAMBDA and obd:
        return f"{obd.lambda_:.3f}"
    if field_id == FIELD_STEERING and obd:
        return f"{obd.steering_deg:.0f}°"
    if field_id == FIELD_BATTERY and obd:
        # Telemetry NDJSON has no battery channel today; fall through to
        # placeholder. Once the cam-side sidecar emits it (e.g. via OBD
        # mirror), wire it here. For now we only render OBD-NDJSON
        # fields, leaving the 0x42 PID purely live (display-only).
        return "—"
    if field_id == FIELD_MAF and obd:
        return "—"
    if field_id in (FIELD_AN1, FIELD_AN2, FIELD_AN3, FIELD_AN4) and ana:
        idx = field_id - FIELD_AN1
        if idx < len(ana.value):
            return f"{ana.value[idx]:.2f}"
    # FIELD_BESTLAP / FIELD_SECTOR1..3 / FIELD_DELTA_NUM aren't directly
    # in the per-frame stream — they need lap-level context. Until the
    # video-export view threads that through, render a neutral placeholder
    # so the layout stays visible.
    if field_id in (FIELD_BESTLAP, FIELD_SECTOR1, FIELD_SECTOR2,
                    FIELD_SECTOR3, FIELD_DELTA_NUM):
        return "--:--"
    return "—"


def _qcolor(s: str | None, fallback: str = "#FFFFFF") -> QColor:
    return QColor(s) if s else QColor(fallback)


def _align_flag(name: str) -> Qt.AlignmentFlag:
    return {
        "left":   Qt.AlignmentFlag.AlignLeft,
        "right":  Qt.AlignmentFlag.AlignRight,
        "center": Qt.AlignmentFlag.AlignCenter,
    }.get(name, Qt.AlignmentFlag.AlignCenter) | Qt.AlignmentFlag.AlignVCenter


def _draw_value(p: QPainter, w: dict, telem: Telemetry | None, ms: int) -> None:
    rect = QRectF(float(w["x"]), float(w["y"]),
                  float(w["w"]), float(w["h"]))
    text = _resolve_value(int(w.get("field", 0)), telem, ms)
    font = QFont("Segoe UI", int(w.get("font_pt", 24)), QFont.Weight.Bold)
    color = _qcolor(w.get("color"))
    # Outlined for legibility on bright videos
    path = QPainterPath()
    metrics = QFontMetrics(font)
    flag = _align_flag(w.get("align", "center"))
    pos = metrics.boundingRect(rect.toRect(), int(flag), text)
    path.addText(QPointF(pos.x(), pos.y() + metrics.ascent()), font, text)
    p.setPen(QPen(QColor(0, 0, 0, 220), 2.5))
    p.setBrush(Qt.BrushStyle.NoBrush)
    p.drawPath(path)
    p.fillPath(path, color)


def _draw_label(p: QPainter, w: dict) -> None:
    rect = QRectF(float(w["x"]), float(w["y"]),
                  float(w["w"]), float(w["h"]))
    text = str(w.get("text", ""))
    font = QFont("Segoe UI", int(w.get("font_pt", 16)))
    color = _qcolor(w.get("color"))
    p.setPen(color)
    p.setFont(font)
    p.drawText(rect, int(_align_flag(w.get("align", "left"))), text)


def _draw_rect(p: QPainter, w: dict) -> None:
    rect = QRectF(float(w["x"]), float(w["y"]),
                  float(w["w"]), float(w["h"]))
    fill = w.get("fill")
    border = w.get("border")
    bw = int(w.get("border_w", 0))
    if fill:
        p.setBrush(QBrush(_qcolor(fill)))
    else:
        p.setBrush(Qt.BrushStyle.NoBrush)
    if border and bw > 0:
        p.setPen(QPen(_qcolor(border), bw))
    else:
        p.setPen(Qt.PenStyle.NoPen)
    p.drawRect(rect)


def _draw_line(p: QPainter, w: dict) -> None:
    p.setPen(QPen(_qcolor(w.get("color"), "#FFFFFF"),
                  int(w.get("width", 2))))
    p.drawLine(int(w.get("x1", 0)), int(w.get("y1", 0)),
               int(w.get("x2", 0)), int(w.get("y2", 0)))


def _draw_image(p: QPainter, w: dict) -> None:
    src = str(w.get("src", ""))
    if not src:
        return
    pix = QPixmap(src)
    if pix.isNull():
        return
    rect = QRectF(float(w["x"]), float(w["y"]),
                  float(w["w"]), float(w["h"]))
    p.drawPixmap(rect, pix, QRectF(pix.rect()))


def render_overlay_image(template: dict, telem: Telemetry | None,
                         ms: int, out_w: int, out_h: int) -> QImage:
    """Paint a .lvt-style overlay template onto a transparent QImage,
    scaled from the template's design canvas (default 1024×600) to the
    output frame size (e.g. 1920×1080). Used by the live preview in the
    Video-Export view AND by the per-frame burn-in for MP4 export."""
    img = QImage(out_w, out_h, QImage.Format.Format_ARGB32_Premultiplied)
    img.fill(0)
    p = QPainter(img)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    p.setRenderHint(QPainter.RenderHint.TextAntialiasing)

    canvas_w = int(template.get("width", 1024))
    canvas_h = int(template.get("height", 600))
    p.scale(out_w / canvas_w, out_h / canvas_h)

    for widget in template.get("widgets") or []:
        t = str(widget.get("type", ""))
        try:
            if t == "value":
                _draw_value(p, widget, telem, ms)
            elif t == "label":
                _draw_label(p, widget)
            elif t == "rect":
                _draw_rect(p, widget)
            elif t == "line":
                _draw_line(p, widget)
            elif t == "image":
                _draw_image(p, widget)
        except (KeyError, ValueError, TypeError):
            # Bad widget data shouldn't break the whole frame — skip it.
            continue

    p.end()
    return img


@dataclass
class Mp4Options:
    fps: int = 30
    crf: int = 23
    width: int = 1920
    height: int = 1080
    burn_hud: bool = True
    # When set, this template drives the per-frame overlay; otherwise the
    # built-in HUD layout in render_hud_image() is used.
    overlay_template: dict | None = None


def export_mp4(
    avi_path: Path,
    dest: Path,
    telemetry: Telemetry | None,
    options: Mp4Options | None = None,
    progress_cb: Callable[[int, int, str], None] | None = None,
) -> None:
    """
    Convert AVI → MP4. If `burn_hud` and telemetry available, paint HUD onto
    every frame via an ffmpeg overlay filter; else straight transcode.

    progress_cb(step_done, step_total, status_text) is called periodically.
    Raises RuntimeError on any failure.
    """
    opts = options or Mp4Options()
    ffmpeg = find_ffmpeg()
    if ffmpeg is None:
        raise RuntimeError(
            "ffmpeg nicht gefunden. Installiere ffmpeg und stelle sicher, "
            "dass es im PATH liegt (oder lege ffmpeg/ffmpeg.exe neben "
            "BRL-Studio.exe)."
        )

    if not (opts.burn_hud and telemetry is not None
            and telemetry.duration_ms > 0):
        # Plain transcode
        if progress_cb:
            progress_cb(0, 1, "Transcoding (ohne HUD)…")
        cmd = [
            str(ffmpeg), "-y",
            "-i", str(avi_path),
            "-c:v", "libx264", "-crf", str(opts.crf),
            "-preset", "medium",
            "-c:a", "aac",
            str(dest),
        ]
        _run_ffmpeg(cmd)
        if progress_cb:
            progress_cb(1, 1, "Fertig")
        return

    # HUD-burned: render PNG sequence then composite
    n_frames = int(opts.fps * telemetry.duration_ms / 1000)
    if n_frames <= 0:
        raise RuntimeError("Telemetrie hat 0 Dauer.")

    with tempfile.TemporaryDirectory(prefix="brl_studio_hud_") as tmp_s:
        tmp = Path(tmp_s)
        if progress_cb:
            progress_cb(0, n_frames, f"Render HUD ({n_frames} Frames)…")
        use_template = opts.overlay_template is not None
        for i in range(n_frames):
            t_ms = int(i * 1000 / opts.fps)
            if use_template:
                img = render_overlay_image(
                    opts.overlay_template, telemetry, t_ms,
                    opts.width, opts.height,
                )
            else:
                img = render_hud_image(telemetry, t_ms,
                                       opts.width, opts.height)
            img.save(str(tmp / f"o_{i:06d}.png"), "PNG")
            if progress_cb and (i % 30 == 0):
                progress_cb(i, n_frames,
                            f"HUD-Rendering {i}/{n_frames}…")

        if progress_cb:
            progress_cb(n_frames, n_frames + 1, "Composite via ffmpeg…")
        cmd = [
            str(ffmpeg), "-y",
            "-i", str(avi_path),
            "-framerate", str(opts.fps),
            "-i", str(tmp / "o_%06d.png"),
            "-filter_complex",
            f"[0:v]scale={opts.width}:{opts.height}[bg];[bg][1:v]overlay=0:0",
            "-c:v", "libx264", "-crf", str(opts.crf),
            "-preset", "medium",
            "-pix_fmt", "yuv420p",
            "-c:a", "aac",
            str(dest),
        ]
        _run_ffmpeg(cmd)
        if progress_cb:
            progress_cb(n_frames + 1, n_frames + 1, "Fertig")


def _run_ffmpeg(cmd: list[str]) -> None:
    try:
        proc = subprocess.run(cmd, check=False, capture_output=True, text=True)
    except FileNotFoundError:
        raise RuntimeError("ffmpeg nicht aufrufbar")
    if proc.returncode != 0:
        # Last 1500 chars of stderr is usually enough to see the real cause.
        tail = (proc.stderr or "")[-1500:]
        raise RuntimeError(f"ffmpeg-Fehler:\n{tail}")
