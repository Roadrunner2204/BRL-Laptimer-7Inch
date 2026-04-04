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
  track: string;
  laps: Lap[];
  // computed client-side:
  best_lap_idx: number;
  downloaded_at: number;  // Unix timestamp ms
}

export interface SessionSummary {
  id: string;
  track: string;
  lap_count: number;
  best_ms: number;
  downloaded_at: number;
}
