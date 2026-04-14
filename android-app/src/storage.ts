import AsyncStorage from '@react-native-async-storage/async-storage';
import { Session, SessionSummary, Track } from './types';

const KEY_IP       = 'laptimer_ip';
const KEY_IDS      = 'session_ids';
const KEY_SESSION  = (id: string) => `session_${id}`;
const KEY_TRACK_IDS = 'local_track_ids';
const KEY_TRACK     = (id: string) => `local_track_${id}`;

// ── IP ────────────────────────────────────────────────────────────────────────

export async function loadIp(): Promise<string> {
  return (await AsyncStorage.getItem(KEY_IP)) ?? '192.168.4.1';
}

export async function saveIp(ip: string): Promise<void> {
  await AsyncStorage.setItem(KEY_IP, ip);
}

// ── Sessions ──────────────────────────────────────────────────────────────────

export async function saveSession(session: Session): Promise<void> {
  await AsyncStorage.setItem(KEY_SESSION(session.id), JSON.stringify(session));
  const ids = await listSessionIds();
  if (!ids.includes(session.id)) {
    await AsyncStorage.setItem(KEY_IDS, JSON.stringify([session.id, ...ids]));
  }
}

export async function loadSession(id: string): Promise<Session | null> {
  const raw = await AsyncStorage.getItem(KEY_SESSION(id));
  return raw ? JSON.parse(raw) : null;
}

export async function deleteSession(id: string): Promise<void> {
  await AsyncStorage.removeItem(KEY_SESSION(id));
  const ids = await listSessionIds();
  await AsyncStorage.setItem(KEY_IDS, JSON.stringify(ids.filter(i => i !== id)));
}

export async function listSessionIds(): Promise<string[]> {
  const raw = await AsyncStorage.getItem(KEY_IDS);
  return raw ? JSON.parse(raw) : [];
}

// ── Local tracks (offline-created, queued for upload) ───────────────────────

export interface LocalTrack {
  id: string;          // uuid-ish, local-only
  track: Track;        // the full Track payload for POST /track
  created_at: number;  // Date.now()
  pending: boolean;    // true = not yet uploaded to device
}

function genId(): string {
  return `trk_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 8)}`;
}

export async function saveLocalTrack(track: Track, pending: boolean): Promise<LocalTrack> {
  const rec: LocalTrack = { id: genId(), track, created_at: Date.now(), pending };
  await AsyncStorage.setItem(KEY_TRACK(rec.id), JSON.stringify(rec));
  const ids = await listLocalTrackIds();
  if (!ids.includes(rec.id)) {
    await AsyncStorage.setItem(KEY_TRACK_IDS, JSON.stringify([rec.id, ...ids]));
  }
  return rec;
}

export async function listLocalTrackIds(): Promise<string[]> {
  const raw = await AsyncStorage.getItem(KEY_TRACK_IDS);
  return raw ? JSON.parse(raw) : [];
}

export async function listLocalTracks(): Promise<LocalTrack[]> {
  const ids = await listLocalTrackIds();
  const out: LocalTrack[] = [];
  for (const id of ids) {
    const raw = await AsyncStorage.getItem(KEY_TRACK(id));
    if (raw) out.push(JSON.parse(raw));
  }
  return out;
}

export async function deleteLocalTrack(id: string): Promise<void> {
  await AsyncStorage.removeItem(KEY_TRACK(id));
  const ids = await listLocalTrackIds();
  await AsyncStorage.setItem(KEY_TRACK_IDS, JSON.stringify(ids.filter(i => i !== id)));
}

export async function markTrackUploaded(id: string): Promise<void> {
  const raw = await AsyncStorage.getItem(KEY_TRACK(id));
  if (!raw) return;
  const rec: LocalTrack = JSON.parse(raw);
  rec.pending = false;
  await AsyncStorage.setItem(KEY_TRACK(id), JSON.stringify(rec));
}

// ────────────────────────────────────────────────────────────────────────────

export async function listSessionSummaries(): Promise<SessionSummary[]> {
  const ids = await listSessionIds();
  const summaries: SessionSummary[] = [];
  for (const id of ids) {
    const s = await loadSession(id);
    if (s) {
      summaries.push({
        id: s.id,
        name: s.name,
        track: s.track,
        lap_count: s.laps.length,
        best_ms: s.laps[s.best_lap_idx]?.total_ms ?? 0,
        downloaded_at: s.downloaded_at,
      });
    }
  }
  return summaries;
}
