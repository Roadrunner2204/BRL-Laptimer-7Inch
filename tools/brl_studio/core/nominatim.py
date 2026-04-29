"""
Nominatim address search — same OpenStreetMap geocoder the Android app
uses, so search results are consistent across the two front-ends per the
unified-architecture rule.

Usage policy: ≤ 1 request/second + a meaningful User-Agent. The QThread
worker enforces the rate limit by virtue of being one-shot per query.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

import requests
from PyQt6.QtCore import QThread, pyqtSignal


_NOMINATIM_URL = "https://nominatim.openstreetmap.org/search"
_USER_AGENT = "BRL-Studio/0.1 (laptimer companion)"


@dataclass
class SearchHit:
    display: str        # human-readable label
    lat: float
    lon: float
    bbox: tuple[float, float, float, float] | None  # (south, north, west, east)

    @classmethod
    def from_json(cls, doc: dict[str, Any]) -> "SearchHit | None":
        try:
            lat = float(doc["lat"])
            lon = float(doc["lon"])
        except (KeyError, ValueError, TypeError):
            return None
        bbox = None
        bb = doc.get("boundingbox")
        if isinstance(bb, list) and len(bb) == 4:
            try:
                bbox = (float(bb[0]), float(bb[1]),
                        float(bb[2]), float(bb[3]))
            except (ValueError, TypeError):
                bbox = None
        return cls(
            display=str(doc.get("display_name", f"{lat}, {lon}")),
            lat=lat, lon=lon, bbox=bbox,
        )


class NominatimWorker(QThread):
    finished_ok = pyqtSignal(list)        # list[SearchHit]
    finished_err = pyqtSignal(str)

    def __init__(self, query: str, limit: int = 8) -> None:
        super().__init__()
        self.query = query
        self.limit = limit

    def run(self) -> None:
        if not self.query.strip():
            self.finished_ok.emit([])
            return
        try:
            r = requests.get(
                _NOMINATIM_URL,
                params={
                    "q": self.query,
                    "format": "json",
                    "limit": str(self.limit),
                    "addressdetails": "0",
                },
                headers={"User-Agent": _USER_AGENT,
                         "Accept-Language": "de,en"},
                timeout=(5.0, 10.0),
            )
            r.raise_for_status()
            docs = r.json()
        except requests.RequestException as e:
            self.finished_err.emit(f"Netzwerk: {e}")
            return
        except ValueError as e:
            self.finished_err.emit(f"JSON: {e}")
            return
        hits: list[SearchHit] = []
        for d in docs:
            if isinstance(d, dict):
                h = SearchHit.from_json(d)
                if h is not None:
                    hits.append(h)
        self.finished_ok.emit(hits)
