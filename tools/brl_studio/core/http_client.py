"""
HTTP transport — talk to the laptimer (Display) and the camera module.

Endpoints (Display, port 80, see main/wifi/data_server.cpp):
    GET    /sessions             -> [{id, name, track, laps, ...}, ...]
    GET    /session/<id>         -> full session JSON
    DELETE /session/<id>         -> 200 on success
    GET    /tracks               -> [{name, country, sf, sectors, ...}, ...]
    POST   /track                -> upload/replace a track def (JSON body)
    GET    /track/<id>           -> single track JSON
    GET    /videos               -> [{id, size, ts, ...}, ...] (302 to cam)
    GET    /video/<id>           -> 302 redirect to camera module

Endpoints (Camera, see cam-firmware/main/http_server/http_server.c):
    GET    /videos/list          -> [{id, size, ts, ...}, ...]
    GET    /video/<id>           -> chunked AVI stream
    GET    /telemetry/<id>       -> NDJSON sidecar (gps/obd/ana/lap rows)
    GET    /preview.jpg          -> live preview JPEG

Both servers send CORS headers; long send_wait_timeout (30 s) for large
AVIs. We rely on `requests` for streaming; chunked downloads use
`stream=True` and a generator so the GUI can show progress.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Iterator

import requests

from .session import Session, parse_session, parse_session_summary

DEFAULT_TIMEOUT = (5.0, 30.0)  # (connect, read) — read big for streamed AVIs


@dataclass
class Endpoint:
    """Where to reach a Display or Cam unit."""

    host: str
    port: int = 80
    scheme: str = "http"

    def url(self, path: str) -> str:
        if not path.startswith("/"):
            path = "/" + path
        return f"{self.scheme}://{self.host}:{self.port}{path}"


class DisplayClient:
    def __init__(self, endpoint: Endpoint) -> None:
        self.ep = endpoint

    def list_sessions(self) -> list[Session]:
        """List session summaries — `Session.is_summary` is True for these.

        The Display's `/sessions` endpoint returns only `{id, name, track,
        lap_count, best_ms}` per entry; lap details require a follow-up
        `get_session(id)` call.
        """
        r = requests.get(self.ep.url("/sessions"), timeout=DEFAULT_TIMEOUT)
        r.raise_for_status()
        docs = r.json()
        if not isinstance(docs, list):
            return []
        return [parse_session_summary(d) for d in docs if isinstance(d, dict)]

    def get_session(self, session_id: str) -> Session:
        r = requests.get(self.ep.url(f"/session/{session_id}"),
                         timeout=DEFAULT_TIMEOUT)
        r.raise_for_status()
        return parse_session(r.json())

    def delete_session(self, session_id: str) -> None:
        r = requests.delete(self.ep.url(f"/session/{session_id}"),
                            timeout=DEFAULT_TIMEOUT)
        r.raise_for_status()

    def list_tracks(self) -> list[dict[str, Any]]:
        """Lightweight track index — each entry only contains name,
        country, length_km, sector_count etc. NO `sf` line and NO
        `sectors[]`. Use `get_track(idx)` to fetch the full def for
        a single track when the user selects it."""
        r = requests.get(self.ep.url("/tracks"), timeout=DEFAULT_TIMEOUT)
        r.raise_for_status()
        out = r.json()
        return out if isinstance(out, list) else []

    def get_track(self, idx: int) -> dict[str, Any]:
        """Full TrackDef incl. sf + sectors[] for one track."""
        r = requests.get(self.ep.url(f"/track/{int(idx)}"),
                         timeout=DEFAULT_TIMEOUT)
        r.raise_for_status()
        doc = r.json()
        return doc if isinstance(doc, dict) else {}

    def upload_track(self, track: dict[str, Any]) -> None:
        r = requests.post(self.ep.url("/track"), json=track,
                          timeout=DEFAULT_TIMEOUT)
        r.raise_for_status()

    def list_videos(self) -> list[dict[str, Any]]:
        """Display proxies /videos to the cam (or returns its index)."""
        r = requests.get(self.ep.url("/videos"), timeout=DEFAULT_TIMEOUT,
                         allow_redirects=True)
        r.raise_for_status()
        out = r.json()
        return out if isinstance(out, list) else []

    def get_obd_status(self) -> dict[str, Any]:
        """Per-FieldId freshness map. Schema:
            {"now": <ms>, "fields": {"<field_id>": <last_ms>, ...}}
        Returns an empty {} on failure so callers can treat absence as
        'no data yet' rather than dying."""
        try:
            r = requests.get(self.ep.url("/obd_status"),
                             timeout=DEFAULT_TIMEOUT)
            if not r.ok:
                return {}
            doc = r.json()
            return doc if isinstance(doc, dict) else {}
        except requests.RequestException:
            return {}


class CamClient:
    def __init__(self, endpoint: Endpoint) -> None:
        self.ep = endpoint

    def list_videos(self) -> list[dict[str, Any]]:
        r = requests.get(self.ep.url("/videos/list"), timeout=DEFAULT_TIMEOUT)
        r.raise_for_status()
        out = r.json()
        return out if isinstance(out, list) else []

    def stream_video(self, video_id: str,
                     chunk_size: int = 32 * 1024) -> Iterator[bytes]:
        r = requests.get(self.ep.url(f"/video/{video_id}"),
                         stream=True, timeout=DEFAULT_TIMEOUT)
        r.raise_for_status()
        for chunk in r.iter_content(chunk_size=chunk_size):
            if chunk:
                yield chunk

    def get_telemetry_ndjson(self, video_id: str) -> str:
        r = requests.get(self.ep.url(f"/telemetry/{video_id}"),
                         timeout=DEFAULT_TIMEOUT)
        r.raise_for_status()
        r.encoding = "utf-8"
        return r.text

    def preview_jpeg(self) -> bytes:
        r = requests.get(self.ep.url("/preview.jpg"),
                         timeout=DEFAULT_TIMEOUT)
        r.raise_for_status()
        return r.content


def probe(endpoint: Endpoint) -> bool:
    """Quick reachability check — `GET /` must return 2xx."""
    try:
        r = requests.get(endpoint.url("/"), timeout=(2.0, 4.0))
        return r.ok
    except requests.RequestException:
        return False
