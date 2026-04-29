"""
Local OSM tile + Leaflet asset proxy with on-disk cache.

Why this exists:
    When Studio is connected to the laptimer's WiFi AP (192.168.4.1) the
    PC has no upstream internet, so direct access to unpkg.com (Leaflet)
    and tile.openstreetmap.org (map tiles) fails — the map stays blank.

How it solves it:
    A tiny ThreadingHTTPServer runs on 127.0.0.1:<auto-port>. The Leaflet
    HTML in `ui/widgets/map.py` is rewritten to point at this server.
    On every request:
      - If we have the asset/tile cached on disk → serve immediately,
        works fully offline.
      - Otherwise → fetch from upstream once, write to cache, serve.

Result: as long as the user has loaded a region online once (e.g. at
home before going to the track), the same region works at the track
without internet.

Cache layout:
    %APPDATA%/BRL Studio/cache/tiles/<z>/<x>/<y>.png
    %APPDATA%/BRL Studio/cache/leaflet/<asset path>

Pre-caching:
    `prefetch_tiles(bounds, zoom_min, zoom_max, progress_cb)` walks the
    tile pyramid for a bounding box and downloads everything not yet on
    disk. The Tracks/Analyse views expose a button that calls this.
"""

from __future__ import annotations

import io
import math
import socket
import threading
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Callable, Optional

import requests

from .video_dl import cache_dir

# Tiny 1x1 transparent PNG used as a placeholder for missing tiles when
# offline. Better than a 404, which Leaflet logs noisily.
_EMPTY_PNG = bytes.fromhex(
    "89504E470D0A1A0A0000000D49484452000000010000000108060000001F15C489"
    "0000000D49444154789C636400010000050001"
    "0D0A2DB40000000049454E44AE426082"
)

_OSM_BASE = "https://tile.openstreetmap.org"
_LEAFLET_BASE = "https://unpkg.com/leaflet@1.9.4/dist"
_USER_AGENT = "BRL-Studio/0.1 (laptimer companion)"


def _tiles_dir() -> Path:
    p = cache_dir("tiles")
    return p


def _leaflet_dir() -> Path:
    p = cache_dir("leaflet")
    return p


# ---------------------------------------------------------------------------
# Cache helpers — work the same for HTTP requests and pre-fetch loops.
# ---------------------------------------------------------------------------


def _fetch_to_cache(url: str, dest: Path) -> bool:
    """Fetch a URL into `dest` (atomically). Returns True on success."""
    if dest.exists() and dest.stat().st_size > 0:
        return True
    try:
        r = requests.get(url, timeout=(3.0, 10.0),
                         headers={"User-Agent": _USER_AGENT})
        if not r.ok or not r.content:
            return False
        dest.parent.mkdir(parents=True, exist_ok=True)
        tmp = dest.with_suffix(dest.suffix + ".part")
        tmp.write_bytes(r.content)
        tmp.replace(dest)
        return True
    except requests.RequestException:
        return False


def tile_path(z: int, x: int, y: int) -> Path:
    return _tiles_dir() / str(z) / str(x) / f"{y}.png"


def fetch_tile(z: int, x: int, y: int) -> bool:
    return _fetch_to_cache(f"{_OSM_BASE}/{z}/{x}/{y}.png",
                           tile_path(z, x, y))


# ---------------------------------------------------------------------------
# Pre-fetch a bounding box at a range of zooms (ahead of going to track).
# ---------------------------------------------------------------------------


@dataclass
class TileBounds:
    north: float
    south: float
    east: float
    west: float

    def is_valid(self) -> bool:
        return (self.north > self.south
                and self.east != self.west
                and -90 <= self.south <= 90
                and -90 <= self.north <= 90
                and -180 <= self.west <= 180
                and -180 <= self.east <= 180)


def _lonlat_to_tile(lat: float, lon: float, z: int) -> tuple[int, int]:
    n = 1 << z
    x = int((lon + 180.0) / 360.0 * n)
    lat_r = math.radians(max(min(lat, 85.0511), -85.0511))
    y = int((1.0 - math.asinh(math.tan(lat_r)) / math.pi) / 2.0 * n)
    return max(0, min(n - 1, x)), max(0, min(n - 1, y))


def count_tiles(bounds: TileBounds, zoom_min: int, zoom_max: int) -> int:
    if not bounds.is_valid():
        return 0
    total = 0
    for z in range(zoom_min, zoom_max + 1):
        x_min, y_max = _lonlat_to_tile(bounds.south, bounds.west, z)
        x_max, y_min = _lonlat_to_tile(bounds.north, bounds.east, z)
        if x_min > x_max:
            x_min, x_max = x_max, x_min
        if y_min > y_max:
            y_min, y_max = y_max, y_min
        total += (x_max - x_min + 1) * (y_max - y_min + 1)
    return total


def prefetch_tiles(bounds: TileBounds, zoom_min: int = 11, zoom_max: int = 16,
                   progress_cb: Optional[Callable[[int, int, str], None]] = None,
                   cancel_cb: Optional[Callable[[], bool]] = None) -> int:
    """Walk the bbox at every zoom in [zoom_min, zoom_max] and cache.

    Returns the number of tiles that ended up on disk (existing + newly
    fetched). Bound checks: the OSM tile-usage policy asks for max ~250
    tiles per second and a User-Agent — we send the UA and stay
    sequential, which is well below the throttle.
    """
    if not bounds.is_valid():
        return 0
    total = count_tiles(bounds, zoom_min, zoom_max)
    done = 0
    if progress_cb:
        progress_cb(0, total, "Starte Tile-Prefetch…")
    for z in range(zoom_min, zoom_max + 1):
        x_min, y_max = _lonlat_to_tile(bounds.south, bounds.west, z)
        x_max, y_min = _lonlat_to_tile(bounds.north, bounds.east, z)
        if x_min > x_max:
            x_min, x_max = x_max, x_min
        if y_min > y_max:
            y_min, y_max = y_max, y_min
        for y in range(y_min, y_max + 1):
            for x in range(x_min, x_max + 1):
                if cancel_cb and cancel_cb():
                    return done
                if not tile_path(z, x, y).exists():
                    fetch_tile(z, x, y)
                done += 1
                if progress_cb and (done % 8 == 0 or done == total):
                    progress_cb(done, total,
                                f"Zoom {z} — {done}/{total} Tiles")
    if progress_cb:
        progress_cb(total, total, "Fertig")
    return done


# ---------------------------------------------------------------------------
# HTTP server — serves cached tiles + Leaflet assets to the embedded
# QWebEngineView. All paths under /tiles/<z>/<x>/<y>.png go to OSM, all
# paths under /leaflet/* mirror unpkg's Leaflet 1.9.4 dist tree.
# ---------------------------------------------------------------------------


_LEAFLET_MIMES = {
    ".js":  "application/javascript",
    ".css": "text/css",
    ".png": "image/png",
    ".svg": "image/svg+xml",
    ".ico": "image/x-icon",
    ".map": "application/json",
}


class _Handler(BaseHTTPRequestHandler):
    # silence the default stderr access log
    def log_message(self, fmt, *args):  # noqa: ANN001
        return

    def do_GET(self) -> None:  # noqa: N802
        path = self.path.split("?", 1)[0]
        if path.startswith("/tiles/"):
            self._serve_tile(path)
        elif path.startswith("/leaflet/"):
            self._serve_leaflet(path)
        elif path == "/" or path == "/health":
            self._reply(200, b"ok", "text/plain")
        else:
            self._reply(404, b"not found", "text/plain")

    # ── tiles ──────────────────────────────────────────────────────────

    def _serve_tile(self, path: str) -> None:
        parts = path.lstrip("/").split("/")
        # ['tiles', '<z>', '<x>', '<y>.png']
        if len(parts) != 4 or not parts[3].endswith(".png"):
            self._reply(400, b"bad tile path", "text/plain"); return
        try:
            z = int(parts[1]); x = int(parts[2])
            y = int(parts[3].rsplit(".", 1)[0])
        except ValueError:
            self._reply(400, b"bad tile coords", "text/plain"); return
        cp = tile_path(z, x, y)
        if not cp.exists():
            fetch_tile(z, x, y)
        if cp.exists():
            self._reply(200, cp.read_bytes(), "image/png",
                        cache_seconds=86400)
        else:
            # Offline + uncached → transparent placeholder
            self._reply(200, _EMPTY_PNG, "image/png")

    # ── leaflet ────────────────────────────────────────────────────────

    def _serve_leaflet(self, path: str) -> None:
        # /leaflet/leaflet.js → cache file leaflet/leaflet.js
        rel = path[len("/leaflet/"):]
        if not rel or ".." in rel:
            self._reply(404, b"not found", "text/plain"); return
        cp = _leaflet_dir() / rel
        if not cp.exists():
            _fetch_to_cache(f"{_LEAFLET_BASE}/{rel}", cp)
        if not cp.exists():
            self._reply(404, b"not found", "text/plain"); return
        ext = "." + rel.rsplit(".", 1)[-1] if "." in rel else ""
        self._reply(200, cp.read_bytes(),
                    _LEAFLET_MIMES.get(ext, "application/octet-stream"),
                    cache_seconds=86400)

    # ── helpers ────────────────────────────────────────────────────────

    def _reply(self, code: int, body: bytes, content_type: str,
               cache_seconds: int = 0) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        if cache_seconds > 0:
            self.send_header("Cache-Control", f"max-age={cache_seconds}")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


# ---------------------------------------------------------------------------
# Singleton server lifecycle.
# ---------------------------------------------------------------------------


class _Server:
    def __init__(self) -> None:
        self.httpd: Optional[ThreadingHTTPServer] = None
        self.thread: Optional[threading.Thread] = None
        self.port: int = 0
        self.lock = threading.Lock()


_S = _Server()


def _pick_port() -> int:
    """Bind to 0 to get a random free loopback port."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start_tile_server() -> int:
    """Idempotent — returns the chosen port. Safe to call from anywhere."""
    with _S.lock:
        if _S.httpd is not None:
            return _S.port
        port = _pick_port()
        _S.httpd = ThreadingHTTPServer(("127.0.0.1", port), _Handler)
        _S.port = port
        _S.thread = threading.Thread(
            target=_S.httpd.serve_forever, name="brl-tile-server",
            daemon=True,
        )
        _S.thread.start()
        return port


def stop_tile_server() -> None:
    with _S.lock:
        if _S.httpd is not None:
            _S.httpd.shutdown()
            _S.httpd.server_close()
            _S.httpd = None
            _S.port = 0


def server_url() -> str:
    return f"http://127.0.0.1:{_S.port}" if _S.port else ""
