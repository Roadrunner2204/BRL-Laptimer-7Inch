"""
Leaflet map embedded in QWebEngineView.

Two operating modes:

    Read-only (Analyse):
        set_traces([{color, points: [[lat, lon], ...]}, ...])
        set_cursor(lat, lon)
        fit_bounds()

    Editable (Tracks):
        set_track_def({name, sf:[a,b,c,d], sectors:[{lat,lon,name}, ...]})
        set_pick_mode("sf1" | "sf2" | "sector_add" | "sector_<n>" | "off")
        Signals:
            map_clicked(lat, lon)        — emitted in any pick mode
            sector_dragged(idx, lat, lon)
            sf_dragged(end_idx /*0|1*/, lat, lon)

The Python↔JS bridge runs over `QWebChannel`. We register a single
`MapBridge` object exposed under the JS-side name `pyBridge`; methods
decorated with `@pyqtSlot` are callable from JS and signals can be
subscribed to.
"""

from __future__ import annotations

import json

from PyQt6.QtCore import QObject, QUrl, pyqtSignal, pyqtSlot
from PyQt6.QtWebChannel import QWebChannel
from PyQt6.QtWebEngineCore import QWebEnginePage
from PyQt6.QtWebEngineWidgets import QWebEngineView

from core.theme import get_palette, theme_bus
from core.tile_server import server_url, start_tile_server


# All external resources are routed through the local tile-proxy at
# %SERVER%/ so the map keeps working when the PC is on the laptimer's
# WiFi (no internet). Tiles + Leaflet assets get cached to disk on first
# access, then served offline. See core/tile_server.py.
_HTML_TEMPLATE = r"""
<!doctype html>
<html><head>
  <meta charset="utf-8">
  <link rel="stylesheet" href="%SERVER%/leaflet/leaflet.css">
  <style>
    html, body, #map { height: 100%; margin: 0; background: %BG%; }
    .leaflet-container { background: %BG%; }
    .pick-mode { cursor: crosshair !important; }
    .sector-label {
      background: rgba(0,0,0,.65); color:#fff; padding:1px 6px;
      border-radius:4px; font: 11px/1.2 sans-serif; white-space: nowrap;
    }
  </style>
</head><body>
  <div id="map"></div>
  <script src="%SERVER%/leaflet/leaflet.js"></script>
  <script src="qrc:///qtwebchannel/qwebchannel.js"></script>
  <script>
    // Leaflet's default marker icon URLs hard-code the unpkg path; rewire
    // them to the local proxy so they also work offline.
    L.Icon.Default.prototype.options.imagePath = '%SERVER%/leaflet/images/';
    var map = L.map('map', { zoomControl: true, attributionControl: false })
                .setView([47.7833, 13.1846], 14);
    L.tileLayer('%SERVER%/tiles/{z}/{x}/{y}.png',
                { maxZoom: 19, crossOrigin: true }).addTo(map);

    // Read-only state
    var polylines = [];
    var bounds = null;
    var cursor = null;

    // Editable track state
    var sfMarkers = [null, null];       // L.Marker[2]
    var sfLine = null;                  // L.Polyline between them
    var sectorMarkers = [];             // L.Marker[]
    var pickMode = "off";

    var bridge = null;
    new QWebChannel(qt.webChannelTransport, function(ch) {
      bridge = ch.objects.pyBridge;
    });

    map.on('click', function(e) {
      if (pickMode === "off") return;
      if (bridge) bridge.on_map_click(e.latlng.lat, e.latlng.lng);
    });

    // ── Geolocation ──────────────────────────────────────────────────
    var youAreHere = null;
    window.brl_request_position = function() {
      if (!navigator.geolocation) {
        if (bridge) bridge.on_geolocation_error("Browser unterstützt keine Geolocation");
        return;
      }
      navigator.geolocation.getCurrentPosition(function(pos) {
        var lat = pos.coords.latitude, lon = pos.coords.longitude;
        if (youAreHere) map.removeLayer(youAreHere);
        youAreHere = L.circleMarker([lat, lon], {
          radius: 9, color: '#fff', weight: 2,
          fillColor: '#2ECC71', fillOpacity: 1.0,
        }).addTo(map);
        map.setView([lat, lon], 15);
        if (bridge) bridge.on_position_received(lat, lon);
      }, function(err) {
        if (bridge) bridge.on_geolocation_error(err.message || "geolocation failed");
      }, { enableHighAccuracy: true, timeout: 10000, maximumAge: 60000 });
    };

    window.brl_goto = function(lat, lon, zoom) {
      map.setView([lat, lon], zoom || 14);
    };

    function clearTraces() {
      for (var i = 0; i < polylines.length; i++) map.removeLayer(polylines[i]);
      polylines = [];
      bounds = null;
    }
    function clampZoomToBounds(b) {
      // Lowest zoom that still shows the whole track is the natural
      // out-limit; allow one step further out so the user can pan with
      // a bit of context. Max stays at the tile-pyramid max (19).
      var fit = map.getBoundsZoom(b, false, [20, 20]);
      map.setMinZoom(Math.max(0, fit - 1));
      map.setMaxZoom(19);
    }
    function clearTrackDef() {
      for (var i = 0; i < 2; i++) {
        if (sfMarkers[i]) { map.removeLayer(sfMarkers[i]); sfMarkers[i] = null; }
      }
      if (sfLine) { map.removeLayer(sfLine); sfLine = null; }
      for (var j = 0; j < sectorMarkers.length; j++) map.removeLayer(sectorMarkers[j]);
      sectorMarkers = [];
    }

    function makeIcon(color, label) {
      return L.divIcon({
        className: '',
        html: '<div class="sector-label" style="border:2px solid '+color+
              ';background:#000a">'+label+'</div>',
        iconSize: [null, null],
        iconAnchor: [10, 10]
      });
    }

    window.brl_set_traces = function(traces) {
      clearTraces();
      var allPts = [];
      for (var i = 0; i < traces.length; i++) {
        var t = traces[i];
        var pl = L.polyline(t.points, { color: t.color, weight: 3, opacity: 0.85 });
        pl.addTo(map);
        polylines.push(pl);
        for (var j = 0; j < t.points.length; j++) allPts.push(t.points[j]);
      }
      if (allPts.length > 0) {
        bounds = L.latLngBounds(allPts);
        clampZoomToBounds(bounds);
        map.fitBounds(bounds, { padding: [20, 20] });
      }
    };
    window.brl_set_cursor = function(lat, lon) {
      if (lat === null || lon === null) {
        if (cursor) { map.removeLayer(cursor); cursor = null; }
        return;
      }
      if (!cursor) {
        cursor = L.circleMarker([lat, lon], {
          radius: 7, color: '#ffffff', weight: 2,
          fillColor: '#2d6cdf', fillOpacity: 1.0,
        }).addTo(map);
      } else {
        cursor.setLatLng([lat, lon]);
      }
    };
    window.brl_fit_bounds = function() {
      if (bounds) map.fitBounds(bounds, { padding: [20, 20] });
    };

    window.brl_set_track_def = function(td) {
      clearTrackDef();
      if (!td) return;

      // S/F line — two endpoints + connecting line
      if (td.sf && td.sf.length >= 4) {
        var p1 = [td.sf[0], td.sf[1]];
        var p2 = [td.sf[2], td.sf[3]];
        sfMarkers[0] = L.marker(p1, {
          draggable: true, icon: makeIcon('#00CFFF', 'S/F-A')
        }).addTo(map);
        sfMarkers[1] = L.marker(p2, {
          draggable: true, icon: makeIcon('#00CFFF', 'S/F-B')
        }).addTo(map);
        sfMarkers[0].on('dragend', function(e) {
          if (bridge) bridge.on_sf_dragged(0,
            e.target.getLatLng().lat, e.target.getLatLng().lng);
          updateSfLine();
        });
        sfMarkers[1].on('dragend', function(e) {
          if (bridge) bridge.on_sf_dragged(1,
            e.target.getLatLng().lat, e.target.getLatLng().lng);
          updateSfLine();
        });
        updateSfLine();
      }
      // Sector points
      var sectors = td.sectors || [];
      for (var i = 0; i < sectors.length; i++) {
        addSectorMarker(i, sectors[i]);
      }
      // Auto-fit if bounds available
      var allPts = [];
      for (var k = 0; k < 2; k++) if (sfMarkers[k]) allPts.push(sfMarkers[k].getLatLng());
      for (var m = 0; m < sectorMarkers.length; m++) allPts.push(sectorMarkers[m].getLatLng());
      if (allPts.length > 1) {
        var b = L.latLngBounds(allPts);
        clampZoomToBounds(b);
        map.fitBounds(b, { padding: [40, 40] });
      }
      else if (allPts.length === 1) {
        map.setMinZoom(0); map.setMaxZoom(19);
        map.setView(allPts[0], 14);
      }
    };

    function updateSfLine() {
      if (sfLine) { map.removeLayer(sfLine); sfLine = null; }
      if (sfMarkers[0] && sfMarkers[1]) {
        sfLine = L.polyline(
          [sfMarkers[0].getLatLng(), sfMarkers[1].getLatLng()],
          { color: '#00CFFF', weight: 4, dashArray: '6 6' }
        ).addTo(map);
      }
    }
    function addSectorMarker(idx, s) {
      var label = s.name || ('S' + (idx + 1));
      var m = L.marker([s.lat, s.lon], {
        draggable: true, icon: makeIcon('#FFE042', label)
      }).addTo(map);
      m.on('dragend', function(e) {
        if (bridge) bridge.on_sector_dragged(idx,
          e.target.getLatLng().lat, e.target.getLatLng().lng);
      });
      sectorMarkers[idx] = m;
    }

    window.brl_set_pick_mode = function(mode) {
      pickMode = mode || "off";
      var div = map.getContainer();
      if (pickMode === "off") div.classList.remove('pick-mode');
      else div.classList.add('pick-mode');
    };

    window.brl_set_bg = function(bg) {
      // Recolour the page so it follows the active app theme. Only the
      // map gutter and around-tile area changes; OSM tiles themselves
      // keep their natural appearance.
      document.body.style.background = bg;
      document.documentElement.style.background = bg;
      var c = document.querySelector('.leaflet-container');
      if (c) c.style.background = bg;
    };
  </script>
</body></html>
"""


class MapBridge(QObject):
    """JS→Python channel. Methods exposed via @pyqtSlot are callable from JS."""

    map_clicked = pyqtSignal(float, float)
    sector_dragged = pyqtSignal(int, float, float)
    sf_dragged = pyqtSignal(int, float, float)
    position_received = pyqtSignal(float, float)
    geolocation_error = pyqtSignal(str)

    @pyqtSlot(float, float)
    def on_map_click(self, lat: float, lon: float) -> None:
        self.map_clicked.emit(lat, lon)

    @pyqtSlot(int, float, float)
    def on_sector_dragged(self, idx: int, lat: float, lon: float) -> None:
        self.sector_dragged.emit(idx, lat, lon)

    @pyqtSlot(int, float, float)
    def on_sf_dragged(self, end_idx: int, lat: float, lon: float) -> None:
        self.sf_dragged.emit(end_idx, lat, lon)

    @pyqtSlot(float, float)
    def on_position_received(self, lat: float, lon: float) -> None:
        self.position_received.emit(lat, lon)

    @pyqtSlot(str)
    def on_geolocation_error(self, msg: str) -> None:
        self.geolocation_error.emit(msg)


class LeafletMap(QWebEngineView):
    def __init__(self) -> None:
        super().__init__()
        self.bridge = MapBridge()
        self._channel = QWebChannel(self.page())
        self._channel.registerObject("pyBridge", self.bridge)
        self.page().setWebChannel(self._channel)
        # Auto-grant geolocation when the JS asks for it. Without this
        # handler the request silently times out (no native browser-style
        # confirm dialog appears in QtWebEngine — the page just gets no
        # response and our position callback never fires).
        try:
            self.page().featurePermissionRequested.connect(
                self._on_feature_permission_requested
            )
        except AttributeError:
            # Older PyQt6 builds may have moved the signal — still works
            # with manual user permission, just less seamless.
            pass

    def _on_feature_permission_requested(self, origin,  # noqa: ANN001
                                         feature) -> None:
        if feature == QWebEnginePage.Feature.Geolocation:
            self.page().setFeaturePermission(
                origin, feature,
                QWebEnginePage.PermissionPolicy.PermissionGrantedByUser,
            )

        # Make sure the proxy is live before we point Leaflet at it.
        # start_tile_server is idempotent.
        start_tile_server()
        base = server_url() or "http://127.0.0.1:0"
        bg = get_palette().get("bg", "#16181d")
        html = _HTML_TEMPLATE.replace("%SERVER%", base).replace("%BG%", bg)
        self.setHtml(html, baseUrl=QUrl(base + "/"))
        self._ready = False
        self.loadFinished.connect(self._on_load_finished)
        self._pending: list[str] = []
        theme_bus().theme_changed.connect(self._on_theme_changed)

    def _on_theme_changed(self, palette: dict) -> None:
        bg = palette.get("bg", "#16181d")
        self._exec(f"window.brl_set_bg({json.dumps(bg)});")

    def _on_load_finished(self, ok: bool) -> None:
        self._ready = ok
        if not ok:
            return
        for js in self._pending:
            self.page().runJavaScript(js)
        self._pending.clear()

    def _exec(self, js: str) -> None:
        if self._ready:
            self.page().runJavaScript(js)
        else:
            self._pending.append(js)

    # ── Read-only mode ──────────────────────────────────────────────────

    def set_traces(self, traces: list[dict]) -> None:
        self._exec(f"window.brl_set_traces({json.dumps(traces)});")

    def set_cursor(self, lat: float | None, lon: float | None) -> None:
        if lat is None or lon is None:
            self._exec("window.brl_set_cursor(null, null);")
        else:
            self._exec(f"window.brl_set_cursor({lat}, {lon});")

    def fit_bounds(self) -> None:
        self._exec("window.brl_fit_bounds();")

    # ── Editable track mode ─────────────────────────────────────────────

    def set_track_def(self, track: dict | None) -> None:
        if track is None:
            self._exec("window.brl_set_track_def(null);")
        else:
            self._exec(f"window.brl_set_track_def({json.dumps(track)});")

    def set_pick_mode(self, mode: str) -> None:
        """mode ∈ {'off', 'sf1', 'sf2', 'sector_add', 'sector_<n>'}."""
        self._exec(f"window.brl_set_pick_mode({json.dumps(mode)});")

    def request_position(self) -> None:
        """Ask the browser for the current geolocation. The result lands
        on the bridge as `position_received(lat, lon)` (or
        `geolocation_error(msg)`). The user must grant the permission
        prompt — QtWebEngine raises featurePermissionRequested for that;
        callers wire that to grant at construction time."""
        self._exec("window.brl_request_position();")

    def goto(self, lat: float, lon: float, zoom: int = 14) -> None:
        self._exec(f"window.brl_goto({lat}, {lon}, {int(zoom)});")
