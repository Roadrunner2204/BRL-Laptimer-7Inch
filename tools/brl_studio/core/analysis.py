"""
Math channels derived from raw GPS track points.

Mirrors the algorithms from android-app/src/lib/analysis.ts so the desktop
Studio shows the same numbers as the phone for the same lap. When the
firmware starts persisting OBD/CAN/analog channels in the session JSON,
those flow through directly without going through this module.

Distance/speed use the equirectangular approximation — accurate to <0.5%
at racing-circuit scales (<10 km) and orders of magnitude faster than
haversine; fine for charts.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

from .session import Lap, TrackPoint

EARTH_R_M = 6_371_000.0


def _flat_distance_m(p1: TrackPoint, p2: TrackPoint) -> float:
    lat_avg = math.radians((p1.lat + p2.lat) * 0.5)
    dlat = math.radians(p2.lat - p1.lat)
    dlon = math.radians(p2.lon - p1.lon) * math.cos(lat_avg)
    return EARTH_R_M * math.hypot(dlat, dlon)


@dataclass
class LapChannels:
    """Per-point arrays, all len == len(lap.points)."""

    distance_m: list[float]      # cumulative from lap start
    speed_kmh: list[float]
    g_long: list[float]          # +accel / -brake (g)
    g_lat: list[float]           # cornering g
    heading_deg: list[float]


def _smooth3(a: list[float]) -> list[float]:
    """3-tap centered moving average. Endpoints are passed through.
    Knocks down isolated spikes by 3× without losing real features."""
    n = len(a)
    if n < 3:
        return list(a)
    out = [0.0] * n
    out[0] = a[0]
    out[-1] = a[-1]
    for i in range(1, n - 1):
        out[i] = (a[i - 1] + a[i] + a[i + 1]) / 3.0
    return out


def derive_channels(lap: Lap) -> LapChannels:
    """Speed / G / heading from a lap's track points.

    Algorithm matches the Android app's analysis.ts -- proven to work
    cleanly with the firmware's ~10 Hz GPS stream. The key tricks:
        * Dedup consecutive samples with identical (lat, lon). Firmware
          ticks faster than GPS gives new fixes, so the same position
          is often stored 2-3× back-to-back with rising lap_ms. Naively
          differentiating those produces a 0 / V / 0 / V sawtooth that
          dominates the chart -- exactly the aliasing the user reported.
          Keeping ONLY samples where the position has actually moved
          collapses the duplicates back into a clean d/dt that reflects
          the real GPS update cadence (5-10 Hz adaptive).
        * Cumulative distance via flat-Earth approximation per step.
        * Speed via CENTRAL differences -- (i+1 vs i-1) instead of
          backward (i vs i-1). Halves the per-sample noise sensitivity.
        * Hard cap on physically unreasonable speed values to defang
          single-sample GPS glitches before they propagate into G calc.
        * Final 3-tap moving-average smoothing on speed + both Gs to
          remove residual jitter without hiding real braking / cornering.
    """
    SPEED_MAX_KMH = 400.0
    G_MAX = 4.0
    # ~1 m at typical lats. Anything below this between two samples is
    # GPS jitter, not motion -- treat both samples as the same position.
    DEDUP_LATLON_EPS = 1e-5

    n_raw = len(lap.points)
    if n_raw == 0:
        return LapChannels([], [], [], [], [])

    # Dedup pass: keep first sample, then only samples that moved more
    # than DEDUP_LATLON_EPS from the LAST KEPT sample. lap_ms is taken
    # from each kept sample, so dt naturally widens to span the silent
    # period between real GPS fixes.
    pts = [lap.points[0]]
    for p in lap.points[1:]:
        last = pts[-1]
        if (abs(p.lat - last.lat) > DEDUP_LATLON_EPS
                or abs(p.lon - last.lon) > DEDUP_LATLON_EPS):
            pts.append(p)
    # Mutate lap.points so downstream callers (map plotting, cursor sync
    # via index lookup) stay aligned with the channels arrays. Without
    # this the analyse view's `lap.points[idx]` would walk into the
    # original duplicate-laden list while idx is in the dedupped space,
    # and the map cursor would land at the wrong physical spot.
    lap.points = pts
    n = len(pts)
    t_ms = [float(p.lap_ms) for p in pts]

    # Cumulative distance
    distance = [0.0] * n
    for i in range(1, n):
        distance[i] = distance[i - 1] + _flat_distance_m(pts[i - 1], pts[i])

    # Heading per point: bearing of incoming segment
    heading = [0.0] * n
    for i in range(1, n):
        dy = math.radians(pts[i].lat - pts[i - 1].lat)
        dx = math.radians(pts[i].lon - pts[i - 1].lon) * math.cos(
            math.radians(pts[i].lat))
        heading[i] = math.degrees(math.atan2(dx, dy)) % 360.0
    if n >= 2:
        heading[0] = heading[1]

    # Speed via central differences -- much less noisy than backward diff.
    speed = [0.0] * n
    for i in range(1, n - 1):
        dd = distance[i + 1] - distance[i - 1]
        dt_s = (t_ms[i + 1] - t_ms[i - 1]) / 1000.0
        if dt_s > 0:
            v = (dd / dt_s) * 3.6
            if v < 0 or v > SPEED_MAX_KMH:
                v = speed[i - 1]
            speed[i] = v
    if n >= 2:
        speed[0] = speed[1]
        speed[-1] = speed[-2]

    # Longitudinal g: dv/dt, central differences too.
    g_long = [0.0] * n
    for i in range(1, n - 1):
        dv_ms = (speed[i + 1] - speed[i - 1]) / 3.6
        dt_s = (t_ms[i + 1] - t_ms[i - 1]) / 1000.0
        if dt_s > 0:
            g = dv_ms / dt_s / 9.80665
            if g < -G_MAX or g > G_MAX:
                g = g_long[i - 1]
            g_long[i] = g

    # Lateral g: v² × curvature, where curvature ≈ Δheading / distance.
    g_lat = [0.0] * n
    for i in range(1, n - 1):
        dh = heading[i + 1] - heading[i - 1]
        while dh > 180:
            dh -= 360
        while dh < -180:
            dh += 360
        ds = distance[i + 1] - distance[i - 1]
        v_ms = speed[i] / 3.6
        if ds > 0:
            kappa = math.radians(dh) / ds
            g = (v_ms * v_ms * kappa) / 9.80665
            if g < -G_MAX or g > G_MAX:
                g = g_lat[i - 1]
            g_lat[i] = g

    # Final smoothing pass — same 3-tap MA the Android app uses.
    speed = _smooth3(speed)
    g_long = _smooth3(g_long)
    g_lat = _smooth3(g_lat)

    return LapChannels(
        distance_m=distance,
        speed_kmh=speed,
        g_long=g_long,
        g_lat=g_lat,
        heading_deg=heading,
    )


def delta_vs_reference(lap: Lap, ref: Lap) -> list[float]:
    """
    Per-point time delta in seconds: lap_time_at_position - ref_time_at_position.
    Positive = slower than reference. Uses nearest-point matching with a
    forward-monotonic cursor (same idea as live_delta.cpp on the device).
    """
    if not lap.points or not ref.points:
        return []
    cursor = 0
    out: list[float] = []
    ref_n = len(ref.points)
    for p in lap.points:
        # Forward search around cursor (don't allow long backwards jumps)
        best_i = cursor
        best_d = float("inf")
        end = min(ref_n - 1, cursor + 60)
        for i in range(cursor, end + 1):
            r = ref.points[i]
            dlat = p.lat - r.lat
            dlon = (p.lon - r.lon) * math.cos(math.radians(p.lat))
            d2 = dlat * dlat + dlon * dlon
            if d2 < best_d:
                best_d = d2
                best_i = i
        cursor = best_i
        out.append((p.lap_ms - ref.points[best_i].lap_ms) / 1000.0)
    return out
