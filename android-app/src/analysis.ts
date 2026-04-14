/**
 * analysis.ts — derive telemetry channels from a lap's raw GPS track.
 *
 * The ESP saves every 5th GPS point (~2 Hz) as [lat, lon, lap_ms].
 * We can derive:
 *   - distance along the track (cumulative meters)
 *   - speed (km/h, from position/time derivative)
 *   - longitudinal G (change of speed over time)
 *   - lateral G (change of heading over time × speed)
 *   - delta-to-reference (time difference at the same track distance)
 *
 * All functions are pure — they take a Lap and return arrays sampled at
 * the same indices as lap.track_points.
 */

import { Lap } from './types';

const DEG2RAD = Math.PI / 180;
const EARTH_R = 6371000;  // meters

/** Haversine distance in meters between two GPS points. */
export function haversineM(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const dLat = (lat2 - lat1) * DEG2RAD;
  const dLon = (lon2 - lon1) * DEG2RAD;
  const a = Math.sin(dLat / 2) ** 2
          + Math.cos(lat1 * DEG2RAD) * Math.cos(lat2 * DEG2RAD) * Math.sin(dLon / 2) ** 2;
  return 2 * EARTH_R * Math.asin(Math.sqrt(a));
}

/** Heading (true north = 0) from p1 to p2 in degrees [0, 360). */
function bearingDeg(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const φ1 = lat1 * DEG2RAD, φ2 = lat2 * DEG2RAD;
  const Δλ = (lon2 - lon1) * DEG2RAD;
  const y = Math.sin(Δλ) * Math.cos(φ2);
  const x = Math.cos(φ1) * Math.sin(φ2) - Math.sin(φ1) * Math.cos(φ2) * Math.cos(Δλ);
  return (Math.atan2(y, x) / DEG2RAD + 360) % 360;
}

export interface LapChannels {
  /** ms since lap start, length N */
  t_ms:  number[];
  /** cumulative distance in meters, length N */
  dist_m: number[];
  /** speed in km/h, length N */
  speed_kmh: number[];
  /** longitudinal G (+ = accel, - = braking), length N */
  g_long: number[];
  /** lateral G (+ = right turn, - = left), length N */
  g_lat: number[];
  /** heading in degrees, length N */
  heading: number[];
}

/** Compute all derived channels for one lap. Returns empty arrays if <2 points. */
export function deriveChannels(lap: Lap): LapChannels {
  const pts = lap.track_points ?? [];
  const N = pts.length;
  const out: LapChannels = {
    t_ms: [], dist_m: [], speed_kmh: [], g_long: [], g_lat: [], heading: [],
  };
  if (N < 2) return out;

  // Time + cumulative distance
  let cum = 0;
  out.t_ms.push(pts[0][2]);
  out.dist_m.push(0);
  for (let i = 1; i < N; i++) {
    const [lat0, lon0] = pts[i - 1];
    const [lat1, lon1, ms1] = pts[i];
    cum += haversineM(lat0, lon0, lat1, lon1);
    out.t_ms.push(ms1);
    out.dist_m.push(cum);
  }

  // Speed (km/h) at each point: central differences, except endpoints.
  out.speed_kmh.push(0);
  for (let i = 1; i < N - 1; i++) {
    const dd = out.dist_m[i + 1] - out.dist_m[i - 1];
    const dt_s = (out.t_ms[i + 1] - out.t_ms[i - 1]) / 1000;
    out.speed_kmh.push(dt_s > 0 ? (dd / dt_s) * 3.6 : 0);
  }
  out.speed_kmh.push(out.speed_kmh[N - 2] ?? 0);

  // Heading: from each segment, clamp to point index by using the bearing
  // from previous to current point.
  out.heading.push(0);
  for (let i = 1; i < N; i++) {
    out.heading.push(bearingDeg(pts[i - 1][0], pts[i - 1][1], pts[i][0], pts[i][1]));
  }

  // Longitudinal G: dv/dt, /9.81
  out.g_long.push(0);
  for (let i = 1; i < N - 1; i++) {
    const dv = (out.speed_kmh[i + 1] - out.speed_kmh[i - 1]) / 3.6;  // m/s
    const dt = (out.t_ms[i + 1] - out.t_ms[i - 1]) / 1000;
    out.g_long.push(dt > 0 ? dv / dt / 9.81 : 0);
  }
  out.g_long.push(0);

  // Lateral G: v² × curvature. Curvature ≈ Δheading / distance.
  out.g_lat.push(0);
  for (let i = 1; i < N - 1; i++) {
    let dHead = out.heading[i + 1] - out.heading[i - 1];
    // Wrap to [-180, 180]
    while (dHead > 180)  dHead -= 360;
    while (dHead < -180) dHead += 360;
    const dHeadRad = dHead * DEG2RAD;
    const ds = out.dist_m[i + 1] - out.dist_m[i - 1];
    const v = out.speed_kmh[i] / 3.6;  // m/s
    const kappa = ds > 0 ? dHeadRad / ds : 0;
    out.g_lat.push((v * v * kappa) / 9.81);
  }
  out.g_lat.push(0);

  // Light smoothing pass on G-channels (3-tap moving average) to reduce noise.
  out.g_long = smooth3(out.g_long);
  out.g_lat  = smooth3(out.g_lat);
  out.speed_kmh = smooth3(out.speed_kmh);

  return out;
}

function smooth3(arr: number[]): number[] {
  const n = arr.length;
  if (n < 3) return arr.slice();
  const out = new Array(n);
  out[0] = arr[0];
  out[n - 1] = arr[n - 1];
  for (let i = 1; i < n - 1; i++) {
    out[i] = (arr[i - 1] + arr[i] + arr[i + 1]) / 3;
  }
  return out;
}

/**
 * Delta-to-reference. For each distance d along the TARGET lap, find the time
 * it took the REFERENCE lap to reach that same distance. Delta = target_time
 * - reference_time (positive means target is slower).
 *
 * Returns delta in ms, one value per target-lap sample.
 */
export function deltaToReference(target: LapChannels, reference: LapChannels): number[] {
  const out: number[] = [];
  const refN = reference.dist_m.length;
  if (refN < 2) return target.t_ms.map(() => 0);

  let j = 0;  // running index into reference
  for (let i = 0; i < target.t_ms.length; i++) {
    const d = target.dist_m[i];
    // advance j while reference distance is still < d
    while (j < refN - 1 && reference.dist_m[j + 1] < d) j++;
    // interpolate reference time at distance d
    const d0 = reference.dist_m[j];
    const d1 = reference.dist_m[Math.min(j + 1, refN - 1)];
    const t0 = reference.t_ms[j];
    const t1 = reference.t_ms[Math.min(j + 1, refN - 1)];
    let ref_t: number;
    if (d1 <= d0) {
      ref_t = t1;
    } else if (d >= d1) {
      ref_t = t1;  // target went further than reference — cap
    } else {
      const frac = (d - d0) / (d1 - d0);
      ref_t = t0 + frac * (t1 - t0);
    }
    out.push(target.t_ms[i] - ref_t);
  }
  return out;
}

/**
 * Min/max across a set of series — for Y-axis auto-scale.
 * Pads the range by 5 % to avoid lines touching the chart edge.
 */
export function yRange(series: number[][]): [number, number] {
  let min = Infinity, max = -Infinity;
  for (const s of series) {
    for (const v of s) {
      if (!isFinite(v)) continue;
      if (v < min) min = v;
      if (v > max) max = v;
    }
  }
  if (!isFinite(min) || !isFinite(max)) return [0, 1];
  if (min === max) { min -= 0.5; max += 0.5; }
  const pad = (max - min) * 0.05;
  return [min - pad, max + pad];
}

/** Nicely format a duration (ms) as "1:23.45" or "23.45s" */
export function fmtLapTime(ms: number): string {
  if (!isFinite(ms) || ms <= 0) return '—';
  const m = Math.floor(ms / 60000);
  const s = (ms % 60000) / 1000;
  if (m > 0) return `${m}:${s.toFixed(2).padStart(5, '0')}`;
  return `${s.toFixed(2)}s`;
}

/** Format delta ms as "+0.34 s" or "-1.02 s" */
export function fmtDelta(ms: number): string {
  if (!isFinite(ms) || ms === 0) return '±0.00 s';
  const sign = ms > 0 ? '+' : '';
  return `${sign}${(ms / 1000).toFixed(2)} s`;
}
