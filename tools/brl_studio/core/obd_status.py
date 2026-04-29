"""
Per-FieldId live status — Studio-side mirror of the firmware's tracker.

Polls `/obd_status` on the laptimer (every 2 s when active) and exposes
`is_live(field_id)` to the slot pickers in LayoutView and the
Custom-Designer's properties panel. A field is considered "live" if its
last-update timestamp is within OBD_STATUS_LIVE_WINDOW_MS (5 s) of the
"now" reported by the firmware.

Singleton + Qt signal so views can refresh on every tick without owning
their own poll loops.
"""

from __future__ import annotations

import threading
import time
from typing import Iterable

from PyQt6.QtCore import QObject, pyqtSignal

from .http_client import DisplayClient, Endpoint

OBD_STATUS_LIVE_WINDOW_MS = 5000
POLL_INTERVAL_S = 2.0


class _StatusBus(QObject):
    """Emits whenever a poll completes — payload is the raw status dict
    so views that want to render details get a fresh snapshot."""

    updated = pyqtSignal(dict)


class ObdStatusPoller:
    def __init__(self) -> None:
        self.bus = _StatusBus()
        self._lock = threading.Lock()
        self._snapshot: dict = {}     # last server response
        self._endpoint: Endpoint | None = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()

    # ── lifecycle ──────────────────────────────────────────────────────

    def configure(self, endpoint: Endpoint | None) -> None:
        """Switch endpoints (or pass None to stop polling). Idempotent."""
        with self._lock:
            self._endpoint = endpoint
        # If polling, the loop will pick up the change on next tick.
        if endpoint is not None and (self._thread is None
                                     or not self._thread.is_alive()):
            self._stop.clear()
            self._thread = threading.Thread(
                target=self._loop, daemon=True, name="brl-obd-status",
            )
            self._thread.start()
        elif endpoint is None:
            self._stop.set()

    def stop(self) -> None:
        self._stop.set()

    # ── public API ─────────────────────────────────────────────────────

    def snapshot(self) -> dict:
        with self._lock:
            return dict(self._snapshot)

    def is_live(self, field_id: int) -> bool:
        """True if the firmware reported a recent update for this field."""
        with self._lock:
            snap = self._snapshot
        if not snap:
            return False
        fields = snap.get("fields") or {}
        last = fields.get(str(int(field_id)))
        if last is None:
            return False
        now = int(snap.get("now", 0))
        try:
            last_i = int(last)
        except (TypeError, ValueError):
            return False
        return (now - last_i) < OBD_STATUS_LIVE_WINDOW_MS

    def live_field_ids(self) -> set[int]:
        with self._lock:
            snap = self._snapshot
        if not snap:
            return set()
        now = int(snap.get("now", 0))
        out: set[int] = set()
        for k, v in (snap.get("fields") or {}).items():
            try:
                fid = int(k); last = int(v)
            except (TypeError, ValueError):
                continue
            if (now - last) < OBD_STATUS_LIVE_WINDOW_MS:
                out.add(fid)
        return out

    # ── poll loop ──────────────────────────────────────────────────────

    def _loop(self) -> None:
        while not self._stop.is_set():
            with self._lock:
                ep = self._endpoint
            if ep is None:
                # No endpoint — sleep and re-check; configure() restarts us.
                time.sleep(POLL_INTERVAL_S)
                continue
            try:
                doc = DisplayClient(ep).get_obd_status()
            except Exception:  # noqa: BLE001
                doc = {}
            with self._lock:
                self._snapshot = doc
            # Always emit — the UI may choose to ignore empty docs, but a
            # transition empty→populated is exactly what the slot picker
            # wants to react to.
            try:
                self.bus.updated.emit(doc)
            except Exception:  # noqa: BLE001
                pass
            self._stop.wait(POLL_INTERVAL_S)


_INSTANCE: ObdStatusPoller | None = None


def get_obd_status_poller() -> ObdStatusPoller:
    global _INSTANCE
    if _INSTANCE is None:
        _INSTANCE = ObdStatusPoller()
    return _INSTANCE


def is_obd_field(field_id: int) -> bool:
    """True for fields that come from OBD/Analog (and thus *can* be 'not
    live' depending on adapter/car). GPS/timing fields are always
    considered available — filtering them would be confusing."""
    # Match the ranges defined in core/dash_config.py
    return (32 <= field_id <= 47) or (64 <= field_id <= 67)


def filter_live(field_ids: Iterable[int],
                poller: ObdStatusPoller | None = None) -> list[int]:
    """Helper for slot pickers: keep GPS/timing fields, drop OBD/AN
    fields that aren't currently live. Used to either grey-out or
    narrow the dropdown contents."""
    if poller is None:
        poller = get_obd_status_poller()
    out: list[int] = []
    for fid in field_ids:
        if not is_obd_field(fid):
            out.append(fid); continue
        if poller.is_live(fid):
            out.append(fid)
    return out
