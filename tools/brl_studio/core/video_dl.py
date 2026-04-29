"""
Video + session download manager.

Streams files from the Display or the Cam to a local cache directory
(`%APPDATA%/BRL Studio/cache/videos|sessions`). Emits Qt signals through
a thin QThread wrapper so the UI can show per-file progress.

Used by both the videos browser (Phase 3) and the analyse view's
"download for offline" path.
"""

from __future__ import annotations

import os
import time
from pathlib import Path

import requests
from PyQt6.QtCore import QStandardPaths, QThread, pyqtSignal


def cache_dir(sub: str = "") -> Path:
    base = QStandardPaths.writableLocation(
        QStandardPaths.StandardLocation.AppDataLocation
    )
    p = Path(base) / "cache"
    if sub:
        p = p / sub
    p.mkdir(parents=True, exist_ok=True)
    return p


class FileDownloadWorker(QThread):
    """
    Streams a URL to disk with chunked progress reporting.

    Emits:
        progress(bytes_done, bytes_total_or_minus_1)
        finished_ok(local_path: str)
        finished_err(message: str)
    """

    progress = pyqtSignal(int, int)
    finished_ok = pyqtSignal(str)
    finished_err = pyqtSignal(str)

    def __init__(self, url: str, dest: Path, chunk_size: int = 64 * 1024) -> None:
        super().__init__()
        self.url = url
        self.dest = dest
        self.chunk_size = chunk_size
        self._cancel = False

    def cancel(self) -> None:
        self._cancel = True

    def run(self) -> None:
        tmp = self.dest.with_suffix(self.dest.suffix + ".part")
        try:
            with requests.get(self.url, stream=True, timeout=(5.0, 60.0)) as r:
                r.raise_for_status()
                total = int(r.headers.get("Content-Length", "-1"))
                self.dest.parent.mkdir(parents=True, exist_ok=True)
                done = 0
                last_emit = 0.0
                with open(tmp, "wb") as f:
                    for chunk in r.iter_content(chunk_size=self.chunk_size):
                        if self._cancel:
                            f.close()
                            tmp.unlink(missing_ok=True)
                            self.finished_err.emit("Abgebrochen")
                            return
                        if not chunk:
                            continue
                        f.write(chunk)
                        done += len(chunk)
                        now = time.monotonic()
                        # Throttle UI updates to ~10 Hz to avoid signal spam
                        if now - last_emit > 0.1:
                            self.progress.emit(done, total)
                            last_emit = now
            os.replace(tmp, self.dest)
            self.progress.emit(done, total)
            self.finished_ok.emit(str(self.dest))
        except requests.RequestException as e:
            tmp.unlink(missing_ok=True)
            self.finished_err.emit(f"Netzwerkfehler: {e}")
        except OSError as e:
            tmp.unlink(missing_ok=True)
            self.finished_err.emit(f"Schreibfehler: {e}")
