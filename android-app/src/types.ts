// GPS point as saved by the laptimer: [lat, lon, lap_ms]
export type TrackPoint = [number, number, number];

export interface Lap {
  lap: number;            // lap number (1-based)
  total_ms: number;
  sectors: number[];      // sector times in ms
  track_points: TrackPoint[];
}

export interface Session {
  id: string;
  name: string;    // user-assigned label, e.g. "BRL_Timing_12.04_14:35"
  track: string;
  laps: Lap[];
  // computed client-side:
  best_lap_idx: number;
  downloaded_at: number;  // Unix timestamp ms
}

export interface SessionSummary {
  id: string;
  name: string;
  track: string;
  lap_count: number;
  best_ms: number;
  downloaded_at: number;
}

// Summary delivered by GET /sessions from the device (no downloaded_at yet).
export interface DeviceSessionSummary {
  id: string;
  name: string;
  track: string;
  lap_count: number;
  best_ms: number;
}

// ── Tracks ────────────────────────────────────────────────────────────────
// Matches the on-device JSON format (main/storage/session_store.cpp).
export interface SectorDef {
  lat: number;
  lon: number;
  name: string;   // "S1", "S2", ...
}

export interface Track {
  name: string;
  country: string;
  length_km: number;
  is_circuit: boolean;
  /** [lat1, lon1, lat2, lon2] — start/finish line, two endpoints */
  sf: [number, number, number, number];
  /** [lat1, lon1, lat2, lon2] — finish line, A-B tracks only */
  fin?: [number, number, number, number];
  sectors: SectorDef[];
}

// Summary returned by GET /tracks (no line coords — just list metadata)
export interface DeviceTrackSummary {
  name: string;
  country: string;
  is_circuit: boolean;
  user_created: boolean;
  sector_count: number;
  length_km: number;
}
