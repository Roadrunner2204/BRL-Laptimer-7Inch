"""
Session file parser.

Sessions are JSON written by the laptimer's session_store.cpp:

    {
        "id":    "sess_A1B2C3D4",
        "name":  "BRL_Timing_01.04_10:15",
        "track": "Salzburgring",
        "laps": [
            {
                "lap": 1,
                "total_ms": 96450,
                "sectors": [40481, 30875, 25094],
                "track_points": [[lat, lon, lap_ms], ...]
            },
            ...
        ]
    }

OBD/CAN/analog channels are not in the session JSON yet (firmware-side
TODO from the pro telemetry roadmap). When they arrive, the loader will
expose them as additional per-point arrays without changing the public
Session/Lap dataclasses' identity — extra fields go in a `channels` dict.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class TrackPoint:
    lat: float
    lon: float
    lap_ms: int


def format_lap_time_ms(ms: int) -> str:
    """Format a lap time (ms) as 'M:SS.mmm' or '—' for zero."""
    if ms <= 0:
        return "—"
    m, s = divmod(ms / 1000.0, 60.0)
    return f"{int(m)}:{s:06.3f}"


@dataclass
class Lap:
    number: int
    total_ms: int
    sectors: list[int]
    points: list[TrackPoint]
    # Future: per-point channels (rpm, throttle, brake, custom CAN, analog).
    # Keys are channel names, values are arrays len == len(points).
    channels: dict[str, list[float]] = field(default_factory=dict)

    @property
    def total_s(self) -> float:
        return self.total_ms / 1000.0

    def lap_time_str(self) -> str:
        return format_lap_time_ms(self.total_ms)


@dataclass
class Session:
    """
    Holds either a full session (laps populated) or a summary-only stub
    (laps empty, lap_count + best_ms set from the /sessions index).
    `is_summary` distinguishes the two; the UI lazy-loads the full body
    on demand via DisplayClient.get_session(id).
    """

    id: str
    name: str
    track: str
    laps: list[Lap]
    lap_count: int = 0
    best_ms: int = 0
    source_path: Path | None = None  # local file, or None when fetched via HTTP

    @property
    def is_summary(self) -> bool:
        return len(self.laps) == 0 and self.lap_count > 0

    @property
    def best_lap(self) -> Lap | None:
        valid = [l for l in self.laps if l.total_ms > 0]
        return min(valid, key=lambda l: l.total_ms) if valid else None

    def best_lap_str(self) -> str:
        if self.laps:
            bl = self.best_lap
            return bl.lap_time_str() if bl else "—"
        return format_lap_time_ms(self.best_ms)


def parse_session(doc: dict[str, Any], source: Path | None = None) -> Session:
    """Build a full Session from a parsed JSON document with `laps`."""
    laps_raw = doc.get("laps") or []
    laps: list[Lap] = []
    for i, l in enumerate(laps_raw):
        pts_raw = l.get("track_points") or []
        points = [
            TrackPoint(lat=float(p[0]), lon=float(p[1]), lap_ms=int(p[2]))
            for p in pts_raw
            if isinstance(p, (list, tuple)) and len(p) >= 3
        ]
        laps.append(
            Lap(
                number=int(l.get("lap", i + 1)),
                total_ms=int(l.get("total_ms", 0)),
                sectors=[int(s) for s in (l.get("sectors") or [])],
                points=points,
            )
        )
    valid_ms = [l.total_ms for l in laps if l.total_ms > 0]
    return Session(
        id=str(doc.get("id", "")),
        name=str(doc.get("name", "")),
        track=str(doc.get("track", "")),
        laps=laps,
        lap_count=len(laps),
        best_ms=min(valid_ms) if valid_ms else 0,
        source_path=source,
    )


def parse_session_summary(doc: dict[str, Any]) -> Session:
    """Build a summary stub from the `/sessions` index entry.

    Schema (data_server.cpp:117): {id, name, track, lap_count, best_ms}.
    """
    return Session(
        id=str(doc.get("id", "")),
        name=str(doc.get("name", "")),
        track=str(doc.get("track", "")),
        laps=[],
        lap_count=int(doc.get("lap_count", 0)),
        best_ms=int(doc.get("best_ms", 0)),
    )


def load_session_file(path: Path) -> Session:
    with path.open("r", encoding="utf-8") as f:
        doc = json.load(f)
    return parse_session(doc, source=path)


def list_local_sessions(folder: Path) -> list[Session]:
    """Scan a folder for *.json sessions (e.g. an extracted SD card)."""
    if not folder.exists():
        return []
    out: list[Session] = []
    for p in sorted(folder.glob("*.json")):
        try:
            out.append(load_session_file(p))
        except (OSError, json.JSONDecodeError):
            continue
    return out
