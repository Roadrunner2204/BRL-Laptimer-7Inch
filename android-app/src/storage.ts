import AsyncStorage from '@react-native-async-storage/async-storage';
import { Session, SessionSummary } from './types';

const KEY_IP       = 'laptimer_ip';
const KEY_IDS      = 'session_ids';
const KEY_SESSION  = (id: string) => `session_${id}`;

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

export async function listSessionSummaries(): Promise<SessionSummary[]> {
  const ids = await listSessionIds();
  const summaries: SessionSummary[] = [];
  for (const id of ids) {
    const s = await loadSession(id);
    if (s) {
      summaries.push({
        id: s.id,
        track: s.track,
        lap_count: s.laps.length,
        best_ms: s.laps[s.best_lap_idx]?.total_ms ?? 0,
        downloaded_at: s.downloaded_at,
      });
    }
  }
  return summaries;
}
