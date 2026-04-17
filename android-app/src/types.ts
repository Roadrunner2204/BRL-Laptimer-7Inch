// GPS point as saved by the laptimer: [lat, lon, lap_ms]
export type TrackPoint = [number, number, number];

export interface Lap {
  lap: number;            // lap number (1-based)
  total_ms: number;
  sectors: number[];      // sector times in ms
  track_points: TrackPoint[];
  /** Filename stem (no .avi) of the per-lap AVI covering this lap, if
   *  recording was active. Fetch via GET /video/<stem>. Absent on older
   *  sessions recorded before the per-lap video split was added. */
  video?: string;
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
  sectors: (SectorDef | SectorLineDef)[];
  /** Populated only by fetchTrackDetails from GET /track/<idx> */
  index?: number;
  user_created?: boolean;
}

/** VBOX-style 2-point sector line (accepted by firmware since v1.0.2) */
export interface SectorLineDef {
  name: string;
  lat1: number;
  lon1: number;
  lat2: number;
  lon2: number;
}

// Summary returned by GET /tracks (no line coords — just list metadata)
export interface DeviceTrackSummary {
  /** Device-side real index into track_get(); server sends this so the
   *  app doesn't have to guess based on array position (which breaks
   *  when the firmware dedupes bundle tracks shadowed by user edits). */
  index?: number;
  name: string;
  country: string;
  is_circuit: boolean;
  user_created: boolean;
  sector_count: number;
  length_km: number;
}
