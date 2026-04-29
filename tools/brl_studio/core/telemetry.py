"""
NDJSON telemetry parser — companion to the AVI from the cam module.

Schema (cam-firmware/main/recorder/sidecar.c):
    {"t":"session_start","ms":0,"utc":...,"id":...,"track":...,"car":...}
    {"t":"gps","ms":42,"utc":...,"lat":..,"lon":..,"spd":..,"hdg":..,"alt":..,
     "hdop":..,"sats":..,"v":..}
    {"t":"obd","ms":42,"utc":...,"rpm":..,"tps":..,"map":..,"lam":..,
     "brk":..,"str":..,"clt":..,"iat":..,"c":..}
    {"t":"ana","ms":42,"utc":...,"mv":[..,..,..,..],"v":[..,..,..,..],"mask":..}
    {"t":"lap","ms":42,"utc":...,"no":..,"total_ms":..,"best":..,
     "frame":..,"sectors":[..]}
    {"t":"session_end","ms":..,"frames":..}

`ms` is the recording-relative milliseconds (sidecar.c rec_rel_ms()) — the
same time base as video PTS, so HUD-Overlay can index directly by player
position.
"""

from __future__ import annotations

import bisect
import json
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class GpsSample:
    ms: int
    utc_ms: int
    lat: float
    lon: float
    speed_kmh: float
    heading_deg: float
    altitude_m: float
    hdop: float
    satellites: int
    valid: bool


@dataclass
class ObdSample:
    ms: int
    utc_ms: int
    rpm: float
    throttle_pct: float
    boost_kpa: float
    lambda_: float
    brake_pct: float
    steering_deg: float
    coolant_c: float
    intake_c: float
    connected: bool


@dataclass
class AnalogSample:
    ms: int
    utc_ms: int
    raw_mv: list[int]
    value: list[float]
    valid_mask: int


@dataclass
class LapMarker:
    ms: int
    utc_ms: int
    lap_no: int
    total_ms: int
    is_best: bool
    video_frame: int
    sectors_ms: list[int]


@dataclass
class Telemetry:
    """Parsed NDJSON file. Lookup helpers index by recording-relative ms."""

    session_id: str = ""
    track: str = ""
    car: str = ""
    utc_anchor_ms: int = 0
    duration_ms: int = 0
    final_frames: int = 0

    gps: list[GpsSample] = field(default_factory=list)
    obd: list[ObdSample] = field(default_factory=list)
    analog: list[AnalogSample] = field(default_factory=list)
    laps: list[LapMarker] = field(default_factory=list)

    # Sorted ms-arrays for bisect lookups
    _gps_ms: list[int] = field(default_factory=list, repr=False)
    _obd_ms: list[int] = field(default_factory=list, repr=False)
    _ana_ms: list[int] = field(default_factory=list, repr=False)

    def _build_indexes(self) -> None:
        self._gps_ms = [s.ms for s in self.gps]
        self._obd_ms = [s.ms for s in self.obd]
        self._ana_ms = [s.ms for s in self.analog]

    @staticmethod
    def _nearest(arr: list[int], samples: list, ms: int):  # noqa: ANN001
        if not arr:
            return None
        i = bisect.bisect_left(arr, ms)
        if i >= len(arr):
            return samples[-1]
        if i == 0:
            return samples[0]
        before = samples[i - 1]
        after = samples[i]
        if abs(arr[i - 1] - ms) <= abs(arr[i] - ms):
            return before
        return after

    def gps_at(self, ms: int) -> GpsSample | None:
        return self._nearest(self._gps_ms, self.gps, ms)

    def obd_at(self, ms: int) -> ObdSample | None:
        return self._nearest(self._obd_ms, self.obd, ms)

    def analog_at(self, ms: int) -> AnalogSample | None:
        return self._nearest(self._ana_ms, self.analog, ms)

    def current_lap(self, ms: int) -> tuple[int, int]:
        """
        Return (lap_no_in_progress, lap_relative_ms_at_recording_ms).

        Walk the lap markers: the lap "in progress" at a given recording
        time runs from the previous marker's ms (or 0) to the next marker's
        ms. The relative time is `ms - previous_marker_ms`.
        """
        if not self.laps:
            return (1, ms)
        prev_ms = 0
        for i, m in enumerate(self.laps):
            if ms < m.ms:
                return (i + 1, ms - prev_ms)
            prev_ms = m.ms
        # Past the last completed lap
        return (len(self.laps) + 1, ms - prev_ms)


def parse_telemetry(text: str) -> Telemetry:
    t = Telemetry()
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        kind = obj.get("t")
        ms = int(obj.get("ms", 0))

        if kind == "session_start":
            t.session_id = str(obj.get("id", ""))
            t.track = str(obj.get("track", ""))
            t.car = str(obj.get("car", ""))
            t.utc_anchor_ms = int(obj.get("utc", 0))
        elif kind == "session_end":
            t.duration_ms = ms
            t.final_frames = int(obj.get("frames", 0))
        elif kind == "gps":
            t.gps.append(GpsSample(
                ms=ms, utc_ms=int(obj.get("utc", 0)),
                lat=float(obj.get("lat", 0.0)),
                lon=float(obj.get("lon", 0.0)),
                speed_kmh=float(obj.get("spd", 0.0)),
                heading_deg=float(obj.get("hdg", 0.0)),
                altitude_m=float(obj.get("alt", 0.0)),
                hdop=float(obj.get("hdop", 0.0)),
                satellites=int(obj.get("sats", 0)),
                valid=bool(obj.get("v", 0)),
            ))
        elif kind == "obd":
            t.obd.append(ObdSample(
                ms=ms, utc_ms=int(obj.get("utc", 0)),
                rpm=float(obj.get("rpm", 0.0)),
                throttle_pct=float(obj.get("tps", 0.0)),
                boost_kpa=float(obj.get("map", 0.0)),
                lambda_=float(obj.get("lam", 0.0)),
                brake_pct=float(obj.get("brk", 0.0)),
                steering_deg=float(obj.get("str", 0.0)),
                coolant_c=float(obj.get("clt", 0.0)),
                intake_c=float(obj.get("iat", 0.0)),
                connected=bool(obj.get("c", 0)),
            ))
        elif kind == "ana":
            mv = obj.get("mv") or [0, 0, 0, 0]
            v = obj.get("v") or [0.0, 0.0, 0.0, 0.0]
            t.analog.append(AnalogSample(
                ms=ms, utc_ms=int(obj.get("utc", 0)),
                raw_mv=[int(x) for x in mv[:4]],
                value=[float(x) for x in v[:4]],
                valid_mask=int(obj.get("mask", 0)),
            ))
        elif kind == "lap":
            t.laps.append(LapMarker(
                ms=ms, utc_ms=int(obj.get("utc", 0)),
                lap_no=int(obj.get("no", 0)),
                total_ms=int(obj.get("total_ms", 0)),
                is_best=bool(obj.get("best", 0)),
                video_frame=int(obj.get("frame", 0)),
                sectors_ms=[int(x) for x in (obj.get("sectors") or [])],
            ))

    t._build_indexes()
    if t.duration_ms == 0 and t.gps:
        t.duration_ms = t.gps[-1].ms
    return t


def load_telemetry_file(path: Path) -> Telemetry:
    with path.open("r", encoding="utf-8") as f:
        return parse_telemetry(f.read())
