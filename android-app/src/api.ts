import { Session, Lap, DeviceSessionSummary, Track, DeviceTrackSummary } from './types';
import { wifiFetch } from './network';

// All display calls go through wifiFetch so they reach the laptimer AP
// even while the default process network is temporarily bound to cellular
// (e.g. TrackCreator map view). Falls back to plain fetch on iOS /
// older builds without the native module.
const dfetch = wifiFetch;

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
    const r = await withTimeout(dfetch(url), 15000, 'fetchDeviceInfo');
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
  const r = await withTimeout(dfetch(url), 15000, 'fetchSessionList');
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
  const r = await withTimeout(dfetch(url), 30000, 'fetchSession');
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  const raw = await r.json();
  return enrichSession(raw);
}

export async function deleteSessionOnDevice(id: string): Promise<void> {
  await withTimeout(
    dfetch(`${baseUrl}/session/${id}`, { method: 'DELETE' }),
    10000,
    'deleteSession',
  );
}

// ── Video API ──────────────────────────────────────────────────────────────
export interface DeviceVideo { id: string; size: number; }

export async function fetchVideoList(): Promise<DeviceVideo[]> {
  const url = `${baseUrl}/videos`;
  log('GET', url);
  const r = await withTimeout(dfetch(url), 15000, 'fetchVideoList');
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}

/** URL to stream/download a video. Use with Video player source={{ uri }} or FileSystem.downloadAsync */
export function videoUrl(id: string): string {
  return `${baseUrl}/video/${id}`;
}

export async function deleteVideoOnDevice(id: string): Promise<void> {
  await withTimeout(
    dfetch(`${baseUrl}/video/${id}`, { method: 'DELETE' }),
    10000,
    'deleteVideo',
  );
}

// ── OBD live-status ────────────────────────────────────────────────────────
//
// Mirror of the firmware's per-FieldId freshness tracker (see
// main/obd/obd_status.cpp). Schema:
//
//   { "now": <ms>, "fields": { "<field_id>": <last_ms>, ... } }
//
// Used by future overlay/slot pickers to grey out fields the connected
// car/adapter isn't actually delivering. The same `is_obd_field` /
// 5 s-window logic applies as in the Studio's core/obd_status.py — keep
// the three frontends aligned per the unified-architecture rule.

export interface ObdStatus {
  now: number;
  fields: Record<string, number>;
}

export const OBD_STATUS_LIVE_WINDOW_MS = 5000;

export async function fetchObdStatus(): Promise<ObdStatus> {
  const url = `${baseUrl}/obd_status`;
  log('GET', url);
  const r = await withTimeout(dfetch(url), 5000, 'fetchObdStatus');
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  const doc = await r.json();
  return {
    now: typeof doc?.now === 'number' ? doc.now : 0,
    fields: doc?.fields && typeof doc.fields === 'object' ? doc.fields : {},
  };
}

/** True if the firmware reported the given FieldId within the live window. */
export function isFieldLive(status: ObdStatus, fieldId: number): boolean {
  const last = status.fields[String(fieldId)];
  if (typeof last !== 'number') return false;
  return status.now - last < OBD_STATUS_LIVE_WINDOW_MS;
}

/** OBD/Analog FIELD_IDs (32-47, 64-67); GPS/timing fields are always
 *  considered "available" and exempt from grey-out. Includes the
 *  dynamic ranges 128..255 (per-vehicle .brl sensors). */
export function isObdField(fieldId: number): boolean {
  return (
    (fieldId >= 32 && fieldId <= 47) ||
    (fieldId >= 64 && fieldId <= 67) ||
    fieldId >= 128
  );
}

/** Dynamic slot ranges — mirrors main/ui/dash_config.h. */
export const DASH_SLOT_OBD_DYN_BASE = 128;
export const DASH_SLOT_OBD_DYN_END  = 192;
export const DASH_SLOT_CAN_DYN_BASE = 192;
export const DASH_SLOT_CAN_DYN_END  = 256;

export function isDynamicObd(slotId: number): boolean {
  return slotId >= DASH_SLOT_OBD_DYN_BASE && slotId < DASH_SLOT_OBD_DYN_END;
}
export function isDynamicCan(slotId: number): boolean {
  return slotId >= DASH_SLOT_CAN_DYN_BASE && slotId < DASH_SLOT_CAN_DYN_END;
}

/** One entry from /sensors. The firmware emits this per VIN, so the
 *  app's slot picker shows exactly what the laptimer would show in
 *  Settings → Slot konfigurieren — the unified-architecture rule. */
export interface DynamicSensor {
  slot_id: number;            // dash_config slot ID (128+ for OBD, 192+ for CAN)
  name: string;               // e.g. "RPM", "WaterT", "BattVolt"
  pid?: number;               // OBD-BLE: Mode-01 PID byte
  can_id?: number;            // CAN-direct: full CAN ID
  type: number;               // 0=generic 1=pressure 2=temp 3=speed 4=lambda
  live: boolean;              // is the sensor producing data right now
  cached_dead?: boolean;      // OBD-BLE: persistently dead in cache
}

export interface SensorsResponse {
  veh_conn_mode: number;      // 0=OBD-BLE, 1=CAN-direct
  source: string;             // human-readable source name
  sensors: DynamicSensor[];
}

/** Pull the dynamic sensor list. Used by the dashboard config picker
 *  to populate Z3 slots from the *actual* sensors this car answered
 *  rather than a hardcoded list. */
export async function fetchSensors(): Promise<SensorsResponse> {
  const url = `${baseUrl}/sensors`;
  log('GET', url);
  const r = await withTimeout(dfetch(url), 5000, 'fetchSensors');
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  const doc = await r.json();
  return {
    veh_conn_mode: typeof doc?.veh_conn_mode === 'number' ? doc.veh_conn_mode : 0,
    source: typeof doc?.source === 'string' ? doc.source : '',
    sensors: Array.isArray(doc?.sensors) ? doc.sensors : [],
  };
}

// ── Tracks API ─────────────────────────────────────────────────────────────

export async function fetchTracks(): Promise<DeviceTrackSummary[]> {
  const url = `${baseUrl}/tracks`;
  log('GET', url);
  const r = await withTimeout(dfetch(url), 15000, 'fetchTracks');
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}

/** Fetch full TrackDef (sf/fin/sectors with 2-point lines) by device index. */
export async function fetchTrackDetails(idx: number): Promise<Track> {
  const url = `${baseUrl}/track/${idx}`;
  log('GET', url);
  const r = await withTimeout(dfetch(url), 15000, 'fetchTrackDetails');
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
  // Route via wifiFetch explicitly — TrackCreator binds the process
  // default to cellular for map tiles; without wifiFetch here the POST
  // would try 192.168.4.1 over mobile data and hang.
  const r = await withTimeout(
    dfetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(track),
    }),
    30000,
    'postTrack',
  );
  const json = await r.json().catch(() => ({}));
  if (!r.ok) {
    const msg = (json && (json as any).error) ? (json as any).error : `HTTP ${r.status}`;
    throw new Error(msg);
  }
  return json;
}

// ───────────────────────────────────────────────────────────────────────────
// Remote control endpoints (used when the device touchscreen is broken /
// inaccessible). All three were added to the firmware on 2026-04-30.
// ───────────────────────────────────────────────────────────────────────────

export interface DeviceStatus {
  sd_available: boolean;
  obd_connected: boolean;
  gps_fix: boolean;
  gps_sats: number;
  speed_kmh: number;
  active_track_idx: number;            // -1 = none selected
  active_track_name: string;           // empty when none
  active_track_is_circuit?: boolean;
  active_track_sector_count?: number;
  lap_count: number;
  best_lap_ms: number;                 // 0 when no valid lap yet
  session_id: string;
}

export async function fetchStatus(): Promise<DeviceStatus> {
  const url = `${baseUrl}/status`;
  const r = await withTimeout(dfetch(url), 8000, 'fetchStatus');
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}

export interface SelectTrackResp {
  active_track_idx: number;
  active_track_name: string;
  is_circuit: boolean;
  sector_count: number;
}

/** Activate a track for live timing on the device. */
export async function selectTrack(index: number): Promise<SelectTrackResp> {
  const url = `${baseUrl}/track/select`;
  log('POST', url, 'index=', index);
  const r = await withTimeout(
    dfetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ index }),
    }),
    10000, 'selectTrack');
  const json = await r.json().catch(() => ({}));
  if (!r.ok) {
    const msg = (json as any).error ?? `HTTP ${r.status}`;
    throw new Error(msg);
  }
  return json;
}

// ───────────────────────────────────────────────────────────────────────────
// Screen-mirror endpoints (added 2026-05-01).
//
//   - The MJPEG stream lives on a SEPARATE httpd instance on port 8081 so
//     the streaming handler can block freely without stalling /touch and
//     /status on port 80. esp_http_server is single-threaded; this is the
//     simplest way to have one long-lived stream + many short requests.
//   - /touch takes display logical coords (0..1023, 0..599). The phone
//     scales its on-screen pixel coords to that range before posting.
// ───────────────────────────────────────────────────────────────────────────

/** Display logical resolution -- matches BSP_LCD_H_RES x BSP_LCD_V_RES. */
export const MIRROR_W = 1024;
export const MIRROR_H = 600;

/** URL of the live MJPEG stream. Use inside a WebView <img src=...>. */
export function mirrorMjpegUrl(): string {
  // Strip http(s):// from baseUrl, splice in the mirror port. The host
  // includes scheme already (http://192.168.4.1) so we just swap :80 -> :8081.
  const host = baseUrl.replace(/^https?:\/\//, '').split(':')[0];
  return `http://${host}:8081/mirror.mjpeg`;
}

/** Single-frame JPEG URL -- fallback for clients that can't display MJPEG. */
export function screenJpgUrl(): string {
  const host = baseUrl.replace(/^https?:\/\//, '').split(':')[0];
  return `http://${host}:8081/screen.jpg`;
}

/** Phone-keyboard relay: append text and/or backspace from the textarea
 *  bound to the visible LVGL keyboard. Returns whether a textarea was
 *  actually targeted (hit: false means no keyboard is on screen, so the
 *  user has to tap a text field on the laptimer first). */
export async function postTextEdit(args: { add?: string; bs?: number }): Promise<{ hit: boolean }> {
  const url = `${baseUrl}/text`;
  const r = await withTimeout(
    dfetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(args),
      connectTimeoutMs: 2000,
      readTimeoutMs: 2000,
    }),
    3000,
    'postTextEdit',
  );
  const json = await r.json().catch(() => ({}));
  return { hit: !!(json as any).hit };
}

/** Inject a touch event into LVGL on the laptimer.
 *  x/y are in display logical pixels (0..1023, 0..599). down=false ends
 *  the gesture; LVGL needs the explicit release to fire click events. */
export async function postTouch(x: number, y: number, down: boolean): Promise<void> {
  const url = `${baseUrl}/touch`;
  // Round + clamp here so the firmware never sees fractional or out-of-range
  // values; saves a parser branch on every drag-move event.
  const ix = Math.max(0, Math.min(MIRROR_W - 1, Math.round(x)));
  const iy = Math.max(0, Math.min(MIRROR_H - 1, Math.round(y)));
  await withTimeout(
    dfetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ x: ix, y: iy, down }),
      // Tight timeouts: a touch event that misses by 2s is useless anyway,
      // and we don't want a backed-up queue if WiFi briefly stalls.
      connectTimeoutMs: 2000,
      readTimeoutMs: 2000,
    }),
    3000,
    'postTouch',
  );
}

/** Wipe in-memory laps and start a fresh session file. */
export async function resetSession(): Promise<{ session_name: string }> {
  const url = `${baseUrl}/session/reset`;
  log('POST', url);
  const r = await withTimeout(
    dfetch(url, { method: 'POST' }),
    10000, 'resetSession');
  const json = await r.json().catch(() => ({}));
  if (!r.ok) {
    const msg = (json as any).error ?? `HTTP ${r.status}`;
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
