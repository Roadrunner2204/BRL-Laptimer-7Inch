import { Session, Lap } from './types';

let baseUrl = 'http://192.168.4.1';  // default AP IP

export function setBaseUrl(ip: string) {
  baseUrl = `http://${ip.replace(/^https?:\/\//, '')}`;
}

export function getBaseUrl() { return baseUrl; }

export async function fetchDeviceInfo(): Promise<{ device: string; version: string; sd: boolean }> {
  const r = await fetch(`${baseUrl}/`, { signal: AbortSignal.timeout(5000) });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}

export async function fetchSessionList(): Promise<string[]> {
  const r = await fetch(`${baseUrl}/sessions`, { signal: AbortSignal.timeout(5000) });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}

export async function fetchSession(id: string): Promise<Session> {
  const r = await fetch(`${baseUrl}/session/${id}`, { signal: AbortSignal.timeout(30000) });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  const raw = await r.json();
  return enrichSession(raw);
}

export async function deleteSessionOnDevice(id: string): Promise<void> {
  await fetch(`${baseUrl}/session/${id}`, {
    method: 'DELETE',
    signal: AbortSignal.timeout(5000),
  });
}

/** Compute best_lap_idx and attach downloaded_at */
export function enrichSession(raw: { id: string; track: string; laps: Lap[] }): Session {
  const laps = raw.laps ?? [];
  let bestIdx = 0;
  let bestMs = Infinity;
  laps.forEach((l, i) => {
    if (l.total_ms > 0 && l.total_ms < bestMs) { bestMs = l.total_ms; bestIdx = i; }
  });
  return { ...raw, laps, best_lap_idx: bestIdx, downloaded_at: Date.now() };
}
