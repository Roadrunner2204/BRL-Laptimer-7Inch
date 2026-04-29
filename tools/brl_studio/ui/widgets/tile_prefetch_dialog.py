"""
Tile pre-fetch dialog — pulls OSM tiles for a bbox+zoom-range into the
on-disk cache so the map keeps working at the track without internet.

Usage:
    bounds = TileBounds(north=..., south=..., east=..., west=...)
    dlg = TilePrefetchDialog(parent, bounds, zoom_min=12, zoom_max=16)
    dlg.exec()
"""

from __future__ import annotations

from PyQt6.QtCore import QThread, pyqtSignal
from PyQt6.QtWidgets import (
    QDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QProgressBar,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
)

from core.tile_server import TileBounds, count_tiles, prefetch_tiles


class _PrefetchWorker(QThread):
    progress = pyqtSignal(int, int, str)
    done = pyqtSignal(int)        # tile count fetched/present
    error = pyqtSignal(str)

    def __init__(self, bounds: TileBounds, z_min: int, z_max: int) -> None:
        super().__init__()
        self.bounds = bounds
        self.z_min = z_min
        self.z_max = z_max
        self._cancel = False

    def cancel(self) -> None:
        self._cancel = True

    def run(self) -> None:
        try:
            n = prefetch_tiles(
                self.bounds, self.z_min, self.z_max,
                progress_cb=lambda d, t, m: self.progress.emit(d, t, m),
                cancel_cb=lambda: self._cancel,
            )
            self.done.emit(n)
        except Exception as e:  # noqa: BLE001
            self.error.emit(str(e))


class TilePrefetchDialog(QDialog):
    def __init__(self, parent, bounds: TileBounds,  # noqa: ANN001
                 zoom_min: int = 12, zoom_max: int = 16) -> None:
        super().__init__(parent)
        self.setWindowTitle("Karten-Tiles cachen")
        self.setModal(True)
        self.setMinimumWidth(460)
        self._bounds = bounds
        self._worker: _PrefetchWorker | None = None

        # Bounds info (read-only display)
        self.lbl_bounds = QLabel(self._fmt_bounds(bounds))

        self.sp_zmin = QSpinBox(); self.sp_zmin.setRange(0, 19); self.sp_zmin.setValue(zoom_min)
        self.sp_zmax = QSpinBox(); self.sp_zmax.setRange(0, 19); self.sp_zmax.setValue(zoom_max)
        for s in (self.sp_zmin, self.sp_zmax):
            s.valueChanged.connect(self._update_estimate)

        form = QFormLayout()
        form.addRow("Bereich", self.lbl_bounds)
        form.addRow("Zoom min", self.sp_zmin)
        form.addRow("Zoom max", self.sp_zmax)

        self.lbl_estimate = QLabel("…")
        self.bar = QProgressBar(); self.bar.setRange(0, 100); self.bar.setValue(0)
        self.lbl_status = QLabel("Bereit.")

        self.btn_start = QPushButton("Cachen starten")
        self.btn_start.clicked.connect(self._start)
        self.btn_cancel = QPushButton("Abbrechen")
        self.btn_cancel.clicked.connect(self._cancel)
        self.btn_close = QPushButton("Schließen")
        self.btn_close.clicked.connect(self.accept)

        btns = QHBoxLayout()
        btns.addStretch(1)
        btns.addWidget(self.btn_start)
        btns.addWidget(self.btn_cancel)
        btns.addWidget(self.btn_close)

        root = QVBoxLayout(self)
        root.addLayout(form)
        root.addWidget(self.lbl_estimate)
        root.addWidget(self.bar)
        root.addWidget(self.lbl_status)
        root.addLayout(btns)

        self._update_estimate()

    @staticmethod
    def _fmt_bounds(b: TileBounds) -> str:
        return (f"N {b.north:.4f}  S {b.south:.4f}  "
                f"W {b.west:.4f}  E {b.east:.4f}")

    def _update_estimate(self) -> None:
        zmin = self.sp_zmin.value()
        zmax = max(zmin, self.sp_zmax.value())
        if self.sp_zmax.value() < zmin:
            self.sp_zmax.setValue(zmin)
        n = count_tiles(self._bounds, zmin, zmax)
        # Rough disk estimate: average OSM PNG ~15 KB
        kb = n * 15
        if kb > 1024:
            size = f"~{kb / 1024:.1f} MB"
        else:
            size = f"~{kb} KB"
        self.lbl_estimate.setText(f"≈ {n} Tiles · {size}")

    def _start(self) -> None:
        zmin = self.sp_zmin.value()
        zmax = max(zmin, self.sp_zmax.value())
        self._worker = _PrefetchWorker(self._bounds, zmin, zmax)
        self._worker.progress.connect(self._on_progress)
        self._worker.done.connect(self._on_done)
        self._worker.error.connect(self._on_error)
        self.btn_start.setEnabled(False)
        self.lbl_status.setText("Lade…")
        self._worker.start()

    def _cancel(self) -> None:
        if self._worker is not None and self._worker.isRunning():
            self._worker.cancel()
            self._worker.wait(2000)
            self.lbl_status.setText("Abgebrochen.")
            self.btn_start.setEnabled(True)

    def _on_progress(self, done: int, total: int, msg: str) -> None:
        if total > 0:
            self.bar.setRange(0, total)
            self.bar.setValue(done)
        self.lbl_status.setText(msg)

    def _on_done(self, n: int) -> None:
        self.lbl_status.setText(f"Fertig — {n} Tiles im Cache.")
        self.btn_start.setEnabled(True)

    def _on_error(self, msg: str) -> None:
        self.lbl_status.setText(f"Fehler: {msg}")
        self.btn_start.setEnabled(True)

    def closeEvent(self, evt) -> None:  # noqa: ANN001
        if self._worker is not None and self._worker.isRunning():
            self._worker.cancel()
            self._worker.wait(2000)
        super().closeEvent(evt)
