/**
 * Dual-network helpers (Android-only).
 *
 * Android routes all default fetch() through the single "validated" network.
 * The laptimer AP returns HTTP 204 to the captive-portal probe so Android
 * keeps the WiFi connected — but that also means all internet traffic
 * (map tiles, Nominatim, GitHub, etc.) tries to go through the laptimer,
 * which has no uplink.
 *
 * The native module BrlNetwork acquires BOTH transports (WiFi + Cellular)
 * simultaneously and exposes:
 *   - preferCellularDefault(): process default = cellular (so the map
 *     WebView and any unspecified fetch pull tiles via mobile data).
 *   - wifiFetch(url, init): explicit request bound to WiFi (for display
 *     calls to http://192.168.4.1 that must go via the laptimer AP).
 *   - cellFetch(url, init): explicit cellular, rarely needed when the
 *     default is already cellular.
 *
 * iOS: no-ops (iOS does per-destination routing automatically for local
 * IPs; not a concern for our use case).
 */

import { NativeModules, Platform } from 'react-native';

type Init = {
  method?: 'GET' | 'POST' | 'PUT' | 'DELETE' | 'PATCH';
  headers?: Record<string, string>;
  body?: string;
  connectTimeoutMs?: number;
  readTimeoutMs?: number;
};

interface NativeResult {
  status: number;
  bodyBase64: string;
  headers: Record<string, string>;
}

const Native: any = NativeModules.BrlNetwork;
const hasNative = Platform.OS === 'android' && !!Native;

function atobCompat(b64: string): Uint8Array {
  // React Native lacks global atob; use a small inline decoder.
  const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  const lookup = new Uint8Array(256);
  for (let i = 0; i < chars.length; i++) lookup[chars.charCodeAt(i)] = i;
  const clean = b64.replace(/=+$/, '');
  const len = (clean.length * 3) >> 2;
  const out = new Uint8Array(len);
  let o = 0;
  for (let i = 0; i < clean.length; i += 4) {
    const c0 = lookup[clean.charCodeAt(i)];
    const c1 = lookup[clean.charCodeAt(i + 1)];
    const c2 = lookup[clean.charCodeAt(i + 2)];
    const c3 = lookup[clean.charCodeAt(i + 3)];
    out[o++] = (c0 << 2) | (c1 >> 4);
    if (o < len) out[o++] = ((c1 & 0xf) << 4) | (c2 >> 2);
    if (o < len) out[o++] = ((c2 & 0x3) << 6) | c3;
  }
  return out;
}

function bytesToString(b: Uint8Array): string {
  // UTF-8 decode — TextDecoder is present on modern Hermes but be safe.
  try {
    // @ts-ignore
    return new TextDecoder('utf-8').decode(b);
  } catch {
    let s = '';
    for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
    return s;
  }
}

function resultToResponse(r: NativeResult): Response {
  const bytes = atobCompat(r.bodyBase64);
  const text  = bytesToString(bytes);
  // Minimal Response-like wrapper. fetch()'s .json()/.text() callers work.
  const resp = {
    ok: r.status >= 200 && r.status < 300,
    status: r.status,
    statusText: '',
    headers: new Headers(r.headers as any),
    url: '',
    json:   async () => JSON.parse(text),
    text:   async () => text,
    bytes:  async () => bytes,
  } as unknown as Response;
  return resp;
}

export async function wifiFetch(url: string, init?: Init): Promise<Response> {
  if (!hasNative) return fetch(url, init as any);
  const r: NativeResult = await Native.fetchViaWifi(url, init ?? null);
  return resultToResponse(r);
}

export async function cellFetch(url: string, init?: Init): Promise<Response> {
  if (!hasNative) return fetch(url, init as any);
  const r: NativeResult = await Native.fetchViaCellular(url, init ?? null);
  return resultToResponse(r);
}

export async function preferCellularDefault(): Promise<void> {
  if (!hasNative) return;
  try { await Native.preferCellularDefault(); }
  catch (e) { console.warn('[network] preferCellularDefault:', e); }
}

export async function preferWifiDefault(): Promise<void> {
  if (!hasNative) return;
  try { await Native.preferWifiDefault(); }
  catch (e) { console.warn('[network] preferWifiDefault:', e); }
}

export async function unbindDefault(): Promise<void> {
  if (!hasNative) return;
  try { await Native.unbindDefault(); }
  catch (e) { console.warn('[network] unbindDefault:', e); }
}

export async function networkState(): Promise<{ wifiAvailable: boolean; cellularAvailable: boolean }> {
  if (!hasNative) return { wifiAvailable: true, cellularAvailable: true };
  return Native.getState();
}
