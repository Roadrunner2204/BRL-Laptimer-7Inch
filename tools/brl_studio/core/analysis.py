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


def derive_channels(lap: Lap) -> LapChannels:
    n = len(lap.points)
    if n == 0:
        return LapChannels([], [], [], [], [])

    # Cumulative distance + speed
    distance = [0.0] * n
    speed = [0.0] * n
    heading = [0.0] * n
    for i in range(1, n):
        d = _flat_distance_m(lap.points[i - 1], lap.points[i])
        distance[i] = distance[i - 1] + d
        dt = (lap.points[i].lap_ms - lap.points[i - 1].lap_ms) / 1000.0
        speed[i] = (d / dt) * 3.6 if dt > 0 else speed[i - 1]
        # heading from previous → current point (degrees, 0=N, 90=E)
        dy = math.radians(lap.points[i].lat - lap.points[i - 1].lat)
        dx = math.radians(lap.points[i].lon - lap.points[i - 1].lon) * math.cos(
            math.radians(lap.points[i].lat)
        )
        heading[i] = math.degrees(math.atan2(dx, dy)) % 360.0
    if n >= 2:
        speed[0] = speed[1]
        heading[0] = heading[1]

    # Longitudinal g — derivative of speed wrt time (1 g ≈ 9.80665 m/s²)
    g_long = [0.0] * n
    for i in range(1, n):
        dv_ms = (speed[i] - speed[i - 1]) / 3.6
        dt = (lap.points[i].lap_ms - lap.points[i - 1].lap_ms) / 1000.0
        g_long[i] = (dv_ms / dt) / 9.80665 if dt > 0 else 0.0

    # Lateral g — speed * heading rate
    g_lat = [0.0] * n
    for i in range(1, n):
        dh = heading[i] - heading[i - 1]
        # wrap to [-180, 180]
        while dh > 180:
            dh -= 360
        while dh < -180:
            dh += 360
        dt = (lap.points[i].lap_ms - lap.points[i - 1].lap_ms) / 1000.0
        if dt > 0:
            yaw_rate = math.radians(dh) / dt
            v_ms = speed[i] / 3.6
            g_lat[i] = (v_ms * yaw_rate) / 9.80665

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
