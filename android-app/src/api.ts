import { Session, Lap, DeviceSessionSummary, Track, DeviceTrackSummary } from './types';

let baseUrl = 'http://192.168.4.1';

export function setBaseUrl(ip: string) {
  baseUrl = `http://${ip.replace(/^https?:\/\//, '')}`;
}

export function getBaseUrl() { return baseUrl; }

// Promise.race based timeout — works on every JS engine (Hermes 0.74 etc.).
function withTimeout<T>(p: Promise<T>, ms: number, label: string): Promise<T> {
  return Promise.race<T>([
    p,
    new Promise<T>((_, reject) =>
      setTimeout(() => reject(new Error(`${label} timed out after ${ms} ms`)), ms),
    ),
  ]);
}

// Info-level log — goes to Metro/logcat WITHOUT triggering the RN LogBox
// yellow warning overlay. Use console.warn ONLY for real unexpected events.
const log = (...args: any[]) => console.log('[api]', ...args);

export async function fetchDeviceInfo(): Promise<{ device: string; version: string; sd: boolean }> {
  const url = `${baseUrl}/`;
  log('GET', url);
  try {
    const r = await withTimeout(fetch(url), 15000, 'fetchDeviceInfo');
    log('device info status:', r.status);
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const json = await r.json();
    log('device info OK:', JSON.stringify(json));
    return json;
  } catch (e: any) {
    console.warn('[api] device info FAILED:', e?.message ?? String(e));
    throw e;
  }
}

export async function fetchSessionList(): Promise<DeviceSessionSummary[]> {
  const url = `${baseUrl}/sessions`;
  log('GET', url);
  const r = await withTimeout(fetch(url), 15000, 'fetchSessionList');
  log('sessions status:', r.status);
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  const raw = await r.json();
  // Back-compat with older firmware that returned string[] of IDs.
  if (Array.isArray(raw) && raw.length > 0 && typeof raw[0] === 'string') {
    return (raw as string[]).map(id => ({
      id, name: id, track: '', lap_count: 0, best_ms: 0,
    }));
  }
  return raw as DeviceSessionSummary[];
}

export async function fetchSession(id: string): Promise<Session> {
  const url = `${baseUrl}/session/${id}`;
  log('GET', url);
  const r = await withTimeout(fetch(url), 30000, 'fetchSession');
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  const raw = await r.json();
  return enrichSession(raw);
}

export async function deleteSessionOnDevice(id: string): Promise<void> {
  await withTimeout(
    fetch(`${baseUrl}/session/${id}`, { method: 'DELETE' }),
    10000,
    'deleteSession',
  );
}

// ── Video API ──────────────────────────────────────────────────────────────
export interface DeviceVideo { id: string; size: number; }

export async function fetchVideoList(): Promise<DeviceVideo[]> {
  const url = `${baseUrl}/videos`;
  log('GET', url);
  const r = await withTimeout(fetch(url), 15000, 'fetchVideoList');
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}

/** URL to stream/download a video. Use with Video player source={{ uri }} or FileSystem.downloadAsync */
export function videoUrl(id: string): string {
  return `${baseUrl}/video/${id}`;
}

export async function deleteVideoOnDevice(id: string): Promise<void> {
  await withTimeout(
    fetch(`${baseUrl}/video/${id}`, { method: 'DELETE' }),
    10000,
    'deleteVideo',
  );
}

// ── Tracks API ─────────────────────────────────────────────────────────────

export async function fetchTracks(): Promise<DeviceTrackSummary[]> {
  const url = `${baseUrl}/tracks`;
  log('GET', url);
  const r = await withTimeout(fetch(url), 15000, 'fetchTracks');
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}

/** Fetch full TrackDef (sf/fin/sectors with 2-point lines) by device index. */
export async function fetchTrackDetails(idx: number): Promise<Track> {
  const url = `${baseUrl}/track/${idx}`;
  log('GET', url);
  const r = await withTimeout(fetch(url), 15000, 'fetchTrackDetails');
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}

/**
 * Upload a track to the device. Device persists it to HDD and immediately
 * reloads the live catalog, so the track is selectable in the laptimer UI
 * without reboot.
 */
export async function postTrack(track: Track): Promise<{ ok: boolean; name: string; total: number }> {
  const url = `${baseUrl}/track`;
  log('POST', url, track.name);
  const r = await withTimeout(
    fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(track),
    }),
    15000,
    'postTrack',
  );
  const json = await r.json().catch(() => ({}));
  if (!r.ok) {
    const msg = (json && (json as any).error) ? (json as any).error : `HTTP ${r.status}`;
    throw new Error(msg);
  }
  return json;
}

/** Compute best_lap_idx and attach downloaded_at */
export function enrichSession(raw: { id: string; name?: string; track: string; laps: Lap[] }): Session {
  const laps = raw.laps ?? [];
  let bestIdx = 0;
  let bestMs = Infinity;
  laps.forEach((l, i) => {
    if (l.total_ms > 0 && l.total_ms < bestMs) { bestMs = l.total_ms; bestIdx = i; }
  });
  const name = raw.name && raw.name.length > 0 ? raw.name : raw.id;
  return { ...raw, name, laps, best_lap_idx: bestIdx, downloaded_at: Date.now() };
}
