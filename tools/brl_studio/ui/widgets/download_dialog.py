"""
Reusable progress dialog for FileDownloadWorker downloads.

Usage:
    dlg = DownloadDialog(self, "Sessions herunterladen")
    worker = FileDownloadWorker(url, dest_path)
    dlg.attach(worker)
    worker.start()
    dlg.exec()
"""

from __future__ import annotations

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import (
    QDialog,
    QHBoxLayout,
    QLabel,
    QProgressBar,
    QPushButton,
    QVBoxLayout,
)

from core.video_dl import FileDownloadWorker


def _fmt_bytes(n: int) -> str:
    if n < 0:
        return "—"
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} TB"


class DownloadDialog(QDialog):
    def __init__(self, parent, title: str = "Download") -> None:  # noqa: ANN001
        super().__init__(parent)
        self.setWindowTitle(title)
        self.setModal(True)
        self.setMinimumWidth(420)
        self._worker: FileDownloadWorker | None = None
        self._result_path: str | None = None
        self._error: str | None = None

        self.lbl_status = QLabel("Initialisiere…")
        self.bar = QProgressBar()
        self.bar.setRange(0, 100)
        self.bar.setValue(0)

        self.btn_cancel = QPushButton("Abbrechen")
        self.btn_cancel.clicked.connect(self._cancel)
        self.btn_close = QPushButton("Schließen")
        self.btn_close.setEnabled(False)
        self.btn_close.clicked.connect(self.accept)

        btnrow = QHBoxLayout()
        btnrow.addStretch(1)
        btnrow.addWidget(self.btn_cancel)
        btnrow.addWidget(self.btn_close)

        root = QVBoxLayout(self)
        root.addWidget(self.lbl_status)
        root.addWidget(self.bar)
        root.addLayout(btnrow)

    def attach(self, worker: FileDownloadWorker) -> None:
        self._worker = worker
        worker.progress.connect(self._on_progress)
        worker.finished_ok.connect(self._on_ok)
        worker.finished_err.connect(self._on_err)

    def result_path(self) -> str | None:
        return self._result_path

    def error(self) -> str | None:
        return self._error

    # ── slots ──────────────────────────────────────────────────────────

    def _on_progress(self, done: int, total: int) -> None:
        if total > 0:
            self.bar.setRange(0, total)
            self.bar.setValue(done)
            self.lbl_status.setText(
                f"{_fmt_bytes(done)} / {_fmt_bytes(total)}"
            )
        else:
            # unknown total — switch to busy indicator
            self.bar.setRange(0, 0)
            self.lbl_status.setText(_fmt_bytes(done))

    def _on_ok(self, local_path: str) -> None:
        self._result_path = local_path
        self.bar.setRange(0, 1)
        self.bar.setValue(1)
        self.lbl_status.setText(f"Fertig: {local_path}")
        self.btn_cancel.setEnabled(False)
        self.btn_close.setEnabled(True)

    def _on_err(self, message: str) -> None:
        self._error = message
        self.bar.setRange(0, 1)
        self.bar.setValue(0)
        self.lbl_status.setText(f"Fehler: {message}")
        self.btn_cancel.setEnabled(False)
        self.btn_close.setEnabled(True)

    def _cancel(self) -> None:
        if self._worker is not None and self._worker.isRunning():
            self._worker.cancel()
            self._worker.wait(2000)
        self.reject()

    def closeEvent(self, evt) -> None:  # noqa: ANN001
        if self._worker is not None and self._worker.isRunning():
            self._worker.cancel()
            self._worker.wait(2000)
        super().closeEvent(evt)
