"""
Scan an SD card (mounted as a drive letter) for sessions and videos.

Layout written by the firmware:
    <SD>/sessions/sess_*.json   — laptimer session files
    <SD>/tracks/*.json          — track defs (from Display)
    <SD>/cars/*.brl             — encrypted car profiles

The cam module's SD has its own layout (recorder.c):
    <SD>/<session_id>/<lap_##>.avi
    <SD>/<session_id>/<lap_##>.ndjson

`detect` returns a tag describing what kind of card the path points to so
the UI can show the right browser. `scan_sessions` and `scan_videos`
return the raw file lists; the views build their own models on top.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from pathlib import Path


class CardKind(Enum):
    UNKNOWN = "unknown"
    LAPTIMER = "laptimer"   # has /sessions and /tracks
    CAMERA = "camera"       # has session-id directories with *.avi


@dataclass
class VideoFile:
    session_id: str
    lap: int
    avi_path: Path
    ndjson_path: Path | None
    size_bytes: int


def detect(root: Path) -> CardKind:
    if not root.exists() or not root.is_dir():
        return CardKind.UNKNOWN
    if (root / "sessions").is_dir() or (root / "tracks").is_dir():
        return CardKind.LAPTIMER
    # Camera card heuristic: at least one immediate subdirectory containing
    # a *.avi file matches recorder.c's layout.
    for sub in root.iterdir():
        if sub.is_dir() and any(sub.glob("*.avi")):
            return CardKind.CAMERA
    return CardKind.UNKNOWN


def scan_sessions(root: Path) -> list[Path]:
    folder = root / "sessions"
    if not folder.is_dir():
        return []
    return sorted(folder.glob("sess_*.json"))


def scan_videos(root: Path) -> list[VideoFile]:
    out: list[VideoFile] = []
    if not root.is_dir():
        return out
    for sub in sorted(root.iterdir()):
        if not sub.is_dir():
            continue
        for avi in sorted(sub.glob("*.avi")):
            # Filename convention: lap_<NN>.avi (recorder.c writes this)
            stem = avi.stem
            lap = 0
            if "_" in stem:
                tail = stem.rsplit("_", 1)[-1]
                if tail.isdigit():
                    lap = int(tail)
            ndjson = avi.with_suffix(".ndjson")
            out.append(
                VideoFile(
                    session_id=sub.name,
                    lap=lap,
                    avi_path=avi,
                    ndjson_path=ndjson if ndjson.exists() else None,
                    size_bytes=avi.stat().st_size,
                )
            )
    return out
