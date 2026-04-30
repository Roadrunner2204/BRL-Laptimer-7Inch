/**
 * HomeScreen — landing dashboard. Shows:
 *   - Connection status card (device info or "not connected" with Connect CTA)
 *   - Recent local sessions (quick access to analysis)
 *   - Primary actions: Connect, Browse sessions
 *
 * One-tap to anywhere the user wants to go.
 */

import React, { useCallback, useState, useEffect } from 'react';
import {
  View, Text, StyleSheet, ScrollView, TouchableOpacity,
  ActivityIndicator, RefreshControl, Alert,
} from 'react-native';
import { useFocusEffect } from '@react-navigation/native';
import { NativeStackNavigationProp } from '@react-navigation/native-stack';
import { RootStackParamList } from '../App';
import { C } from '../theme';
import {
  listSessionSummaries, loadIp,
  listLocalTracks, markTrackUploaded, deleteLocalTrack, LocalTrack,
} from '../storage';
import { SessionSummary } from '../types';
import { fetchDeviceInfo, postTrack, setBaseUrl, fetchStatus, resetSession,
         DeviceStatus } from '../api';
import { fmtTime, fmtDate } from '../utils';

type Props = {
  navigation: NativeStackNavigationProp<RootStackParamList, 'Home'>;
};

type DeviceInfo = { device: string; version: string; sd: boolean } | null;

function parseDate(id: string): string {
  const m = id.match(/^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})$/);
  if (!m) return id;
  return `${m[3]}.${m[2]}.${m[1]}  ${m[4]}:${m[5]}`;
}

export default function HomeScreen({ navigation }: Props) {
  const [local, setLocal] = useState<SessionSummary[]>([]);
  const [deviceInfo, setDeviceInfo] = useState<DeviceInfo>(null);
  const [checking, setChecking] = useState(false);
  const [refreshing, setRefreshing] = useState(false);
  const [ip, setIp] = useState('192.168.4.1');
  const [pendingTracks, setPendingTracks] = useState<LocalTrack[]>([]);
  const [syncingTracks, setSyncingTracks] = useState(false);
  const [status, setStatus] = useState<DeviceStatus | null>(null);
  const [resettingSession, setResettingSession] = useState(false);

  const refresh = useCallback(async () => {
    setRefreshing(true);
    try {
      const storedIp = await loadIp();
      setIp(storedIp);
      setBaseUrl(storedIp);
      setLocal(await listSessionSummaries());
      const tracks = await listLocalTracks();
      setPendingTracks(tracks.filter(t => t.pending));

      setChecking(true);
      try {
        const info = await fetchDeviceInfo();
        setDeviceInfo(info);
        // Live-Status-Karte: Track + Lap-Count + GPS/OBD + new-session-button.
        // Older firmware doesn't have /status, then we just hide the card.
        try {
          const st = await fetchStatus();
          setStatus(st);
        } catch { setStatus(null); }
      } catch {
        setDeviceInfo(null);
        setStatus(null);
      } finally {
        setChecking(false);
      }
    } finally {
      setRefreshing(false);
    }
  }, []);

  // Poll status every 3s while focused + connected, so user sees lap count
  // tick up + GPS go from "kein Fix" to "Fix" without manual pull-to-refresh.
  useEffect(() => {
    if (!deviceInfo) return;
    const t = setInterval(async () => {
      try { setStatus(await fetchStatus()); }
      catch {/* ignore — pull-to-refresh resets card state */}
    }, 3000);
    return () => clearInterval(t);
  }, [deviceInfo]);

  async function handleResetSession() {
    Alert.alert('Neue Session starten?',
      'Alle nicht gespeicherten Runden der aktuellen Session werden verworfen.',
      [
        { text: 'Abbrechen', style: 'cancel' },
        { text: 'Neu starten', style: 'destructive', onPress: async () => {
          setResettingSession(true);
          try {
            const r = await resetSession();
            try { setStatus(await fetchStatus()); } catch {}
            Alert.alert('Session zurückgesetzt', `Neue Session: ${r.session_name}`);
          } catch (e: any) {
            Alert.alert('Fehler', e?.message ?? String(e));
          } finally {
            setResettingSession(false);
          }
        } },
      ]);
  }

  async function syncPendingTracks() {
    if (pendingTracks.length === 0) return;
    setSyncingTracks(true);
    let uploaded = 0;
    const failed: string[] = [];
    for (const rec of pendingTracks) {
      try {
        await postTrack(rec.track);
        await markTrackUploaded(rec.id);
        uploaded++;
      } catch (e: any) {
        failed.push(`${rec.track.name}: ${e?.message ?? String(e)}`);
      }
    }
    setSyncingTracks(false);
    await refresh();
    if (failed.length === 0) {
      Alert.alert('Upload fertig',
        `${uploaded} Strecke${uploaded === 1 ? '' : 'n'} auf das Gerät übertragen.`);
    } else {
      Alert.alert('Upload teilweise fertig',
        `${uploaded} erfolgreich, ${failed.length} fehlgeschlagen:\n\n${failed.join('\n')}`);
    }
  }

  function confirmDeleteTrack(rec: LocalTrack) {
    Alert.alert(`"${rec.track.name}" löschen?`,
      'Die lokale Kopie wird entfernt. Falls sie noch nicht hochgeladen wurde, geht sie verloren.',
      [
        { text: 'Abbrechen', style: 'cancel' },
        { text: 'Löschen', style: 'destructive',
          onPress: async () => { await deleteLocalTrack(rec.id); await refresh(); } },
      ]);
  }

  useFocusEffect(useCallback(() => { refresh(); }, [refresh]));

  const recentSessions = local.slice(0, 5);

  return (
    <ScrollView
      style={s.root}
      contentContainerStyle={{ padding: 12, paddingBottom: 32 }}
      refreshControl={<RefreshControl refreshing={refreshing} onRefresh={refresh} tintColor={C.accent} />}
    >
      {/* Brand header */}
      <View style={s.brand}>
        <Text style={s.brandLogo}>BRL</Text>
        <Text style={s.brandSub}>Telemetry</Text>
      </View>

      {/* Connection status card */}
      <View style={s.statusCard}>
        <View style={s.statusHead}>
          <View style={[s.dot, deviceInfo ? s.dotOk : s.dotOff]} />
          <Text style={s.statusTitle}>
            {checking ? 'Prüfe Verbindung…'
                      : deviceInfo ? 'Verbunden' : 'Nicht verbunden'}
          </Text>
          {checking && <ActivityIndicator color={C.accent} size="small" style={{ marginLeft: 8 }} />}
        </View>
        {deviceInfo ? (
          <>
            <Text style={s.statusDetail}>{deviceInfo.device} · v{deviceInfo.version}</Text>
            <Text style={s.statusDetail}>{ip} · HDD {deviceInfo.sd ? '✓' : '✗'}</Text>
            <View style={s.statusActions}>
              <TouchableOpacity
                style={s.primaryBtn}
                onPress={() => navigation.navigate('Sessions', { mode: 'device' })}
              >
                <Text style={s.primaryBtnTxt}>Sessions vom Gerät</Text>
              </TouchableOpacity>
            </View>
          </>
        ) : (
          <>
            <Text style={s.statusDetail}>Mit WLAN BRL-Laptimer verbinden und auf Verbinden tippen.</Text>
            <TouchableOpacity
              style={s.primaryBtn}
              onPress={() => navigation.navigate('Connect')}
            >
              <Text style={s.primaryBtnTxt}>Verbinden</Text>
            </TouchableOpacity>
          </>
        )}
      </View>

      {/* Live status card — only shown when connected AND firmware exposes /status */}
      {deviceInfo && status && (
        <View style={s.liveCard}>
          <View style={s.liveHead}>
            <Text style={s.liveTitle}>Live-Status</Text>
            <View style={s.liveBadges}>
              <View style={[s.badge, status.gps_fix ? s.badgeOk : s.badgeOff]}>
                <Text style={s.badgeTxt}>GPS {status.gps_fix
                  ? `${status.gps_sats}` : '—'}</Text>
              </View>
              <View style={[s.badge, status.obd_connected ? s.badgeOk : s.badgeOff]}>
                <Text style={s.badgeTxt}>OBD</Text>
              </View>
              <View style={[s.badge, status.sd_available ? s.badgeOk : s.badgeOff]}>
                <Text style={s.badgeTxt}>HDD</Text>
              </View>
            </View>
          </View>

          <Text style={s.liveTrack} numberOfLines={1}>
            {status.active_track_idx >= 0
              ? `🏁  ${status.active_track_name}`
              : 'Keine Strecke aktiv'}
          </Text>
          <Text style={s.liveStats}>
            {status.lap_count > 0
              ? `${status.lap_count} Runden${status.best_lap_ms > 0
                  ? `  ·  Best ${fmtTime(status.best_lap_ms)}` : ''}`
              : status.active_track_idx >= 0
                ? 'Noch keine Runden in dieser Session'
                : '— '}
          </Text>

          <View style={s.liveActions}>
            <TouchableOpacity
              style={s.liveBtn}
              onPress={() => navigation.navigate('Tracks')}
            >
              <Text style={s.liveBtnTxt}>
                {status.active_track_idx >= 0 ? 'Strecke wechseln' : 'Strecke wählen'}
              </Text>
            </TouchableOpacity>
            <TouchableOpacity
              style={[s.liveBtn,
                      status.active_track_idx < 0 && { opacity: 0.4 }]}
              onPress={handleResetSession}
              disabled={status.active_track_idx < 0 || resettingSession}
            >
              {resettingSession
                ? <ActivityIndicator color={C.text} size="small" />
                : <Text style={s.liveBtnTxt}>Neue Session</Text>}
            </TouchableOpacity>
          </View>
        </View>
      )}

      {/* Quick actions row */}
      <View style={s.quickRow}>
        <TouchableOpacity
          style={s.quickCard}
          onPress={() => navigation.navigate('Sessions', { mode: 'local' })}
        >
          <Text style={s.quickIcon}>📁</Text>
          <Text style={s.quickTitle}>Sessions</Text>
          <Text style={s.quickSub}>{local.length} gespeichert</Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={s.quickCard}
          onPress={() => navigation.navigate('Videos')}
          disabled={!deviceInfo}
        >
          <Text style={[s.quickIcon, !deviceInfo && { opacity: 0.3 }]}>🎥</Text>
          <Text style={[s.quickTitle, !deviceInfo && { opacity: 0.3 }]}>Videos</Text>
          <Text style={[s.quickSub, !deviceInfo && { opacity: 0.3 }]}>
            {deviceInfo ? 'ansehen & löschen' : 'offline'}
          </Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={s.quickCard}
          onPress={() => {
            if (deviceInfo) navigation.navigate('Tracks');
            else navigation.navigate('TrackCreator', undefined);
          }}
        >
          <Text style={s.quickIcon}>🏁</Text>
          <Text style={s.quickTitle}>Strecken</Text>
          <Text style={s.quickSub}>
            {deviceInfo ? 'ansehen & bearbeiten' : 'offline erstellen'}
          </Text>
        </TouchableOpacity>
      </View>

      {/* Pending track uploads */}
      {pendingTracks.length > 0 && (
        <View style={s.pendingCard}>
          <View style={s.pendingHead}>
            <Text style={s.pendingTitle}>
              {pendingTracks.length} Strecke{pendingTracks.length === 1 ? '' : 'n'} wartet auf Upload
            </Text>
            <TouchableOpacity
              style={[s.syncBtn, (!deviceInfo || syncingTracks) && { opacity: 0.4 }]}
              onPress={syncPendingTracks}
              disabled={!deviceInfo || syncingTracks}
            >
              {syncingTracks
                ? <ActivityIndicator color="#000" size="small" />
                : <Text style={s.syncBtnTxt}>
                    {deviceInfo ? 'Jetzt hochladen' : 'Gerät offline'}
                  </Text>}
            </TouchableOpacity>
          </View>
          {pendingTracks.map(rec => (
            <View key={rec.id} style={s.pendingRow}>
              <View style={{ flex: 1 }}>
                <Text style={s.pendingName} numberOfLines={1}>{rec.track.name}</Text>
                <Text style={s.pendingSub}>
                  {rec.track.is_circuit ? 'Rundkurs' : 'A-B'}
                  {' · '}{rec.track.sectors.length} Sektoren
                  {' · '}{new Date(rec.created_at).toLocaleDateString('de-DE')}
                </Text>
              </View>
              <TouchableOpacity onPress={() => confirmDeleteTrack(rec)} style={s.pendingDel}>
                <Text style={s.pendingDelTxt}>✕</Text>
              </TouchableOpacity>
            </View>
          ))}
          {!deviceInfo && (
            <Text style={s.pendingHint}>
              Verbinde dich mit dem Laptimer, dann kannst du alle auf einmal hochladen.
            </Text>
          )}
        </View>
      )}

      {/* Recent sessions */}
      {recentSessions.length > 0 && (
        <>
          <Text style={s.sectionLabel}>Zuletzt</Text>
          {recentSessions.map(r => (
            <TouchableOpacity
              key={r.id}
              style={s.recentCard}
              onPress={() => navigation.navigate('Detail', { sessionId: r.id })}
            >
              <View style={{ flex: 1 }}>
                <Text style={s.recentTitle} numberOfLines={1}>{r.name}</Text>
                <Text style={s.recentSub}>{parseDate(r.id)}  ·  {r.track || '—'}</Text>
              </View>
              <View style={s.recentRight}>
                <Text style={s.recentBest}>{fmtTime(r.best_ms)}</Text>
                <Text style={s.recentLaps}>{r.lap_count} Runden</Text>
              </View>
            </TouchableOpacity>
          ))}
          {local.length > 5 && (
            <TouchableOpacity
              style={s.moreBtn}
              onPress={() => navigation.navigate('Sessions', { mode: 'local' })}
            >
              <Text style={s.moreBtnTxt}>Alle {local.length} Sessions anzeigen →</Text>
            </TouchableOpacity>
          )}
        </>
      )}

      {recentSessions.length === 0 && (
        <View style={s.empty}>
          <Text style={s.emptyIcon}>🏁</Text>
          <Text style={s.emptyTxt}>Noch keine Sessions gespeichert</Text>
          <Text style={s.emptySub}>Verbinde dich mit dem Laptimer und lade Sessions herunter.</Text>
        </View>
      )}
    </ScrollView>
  );
}

const s = StyleSheet.create({
  root:         { flex: 1, backgroundColor: C.bg },

  brand:        { alignItems: 'center', marginTop: 8, marginBottom: 20 },
  brandLogo:    { color: C.accent, fontSize: 44, fontWeight: '900', letterSpacing: 3 },
  brandSub:     { color: C.text, fontSize: 16, marginTop: -6, letterSpacing: 2, fontWeight: '500' },

  statusCard:   { backgroundColor: C.surface, borderRadius: 12, padding: 16, marginBottom: 14 },
  statusHead:   { flexDirection: 'row', alignItems: 'center', marginBottom: 6 },
  dot:          { width: 10, height: 10, borderRadius: 5, marginRight: 8 },
  dotOk:        { backgroundColor: C.accent, shadowColor: C.accent, shadowOpacity: 0.6,
                  shadowRadius: 6, shadowOffset: { width: 0, height: 0 } },
  dotOff:       { backgroundColor: C.textDark },
  statusTitle:  { color: C.text, fontSize: 16, fontWeight: '700' },
  statusDetail: { color: C.dim, fontSize: 12, marginTop: 2 },
  statusActions:{ marginTop: 12 },

  primaryBtn:   { backgroundColor: C.accent, borderRadius: 8, paddingVertical: 12,
                  alignItems: 'center', marginTop: 6 },
  primaryBtnTxt:{ color: '#000', fontSize: 15, fontWeight: '700' },

  liveCard:     { backgroundColor: C.surface, borderRadius: 12, padding: 14,
                  marginBottom: 14, borderWidth: 1, borderColor: C.border },
  liveHead:     { flexDirection: 'row', alignItems: 'center',
                  justifyContent: 'space-between', marginBottom: 10 },
  liveTitle:    { color: C.dim, fontSize: 11, fontWeight: '700',
                  textTransform: 'uppercase', letterSpacing: 1 },
  liveBadges:   { flexDirection: 'row', gap: 6 },
  badge:        { paddingHorizontal: 8, paddingVertical: 3, borderRadius: 6 },
  badgeOk:      { backgroundColor: C.accent + '22', borderWidth: 1,
                  borderColor: C.accent },
  badgeOff:     { backgroundColor: C.surface2, borderWidth: 1,
                  borderColor: C.border },
  badgeTxt:     { color: C.text, fontSize: 10, fontWeight: '700' },
  liveTrack:    { color: C.text, fontSize: 16, fontWeight: '700', marginBottom: 4 },
  liveStats:    { color: C.dim, fontSize: 12, marginBottom: 12 },
  liveActions:  { flexDirection: 'row', gap: 8 },
  liveBtn:      { flex: 1, backgroundColor: C.surface2, borderRadius: 8,
                  paddingVertical: 10, alignItems: 'center',
                  borderWidth: 1, borderColor: C.border },
  liveBtnTxt:   { color: C.text, fontSize: 13, fontWeight: '700' },

  quickRow:     { flexDirection: 'row', gap: 10, marginBottom: 18 },
  quickCard:    { flex: 1, backgroundColor: C.surface, borderRadius: 12, padding: 14,
                  alignItems: 'center' },
  quickIcon:    { fontSize: 28, marginBottom: 6 },
  quickTitle:   { color: C.text, fontSize: 14, fontWeight: '700' },
  quickSub:     { color: C.dim, fontSize: 11, marginTop: 2 },

  sectionLabel: { color: C.dim, fontSize: 11, fontWeight: '700', textTransform: 'uppercase',
                  marginBottom: 8, paddingHorizontal: 4 },

  recentCard:   { backgroundColor: C.surface, borderRadius: 10, padding: 12, marginBottom: 8,
                  flexDirection: 'row', alignItems: 'center' },
  recentTitle:  { color: C.text, fontSize: 14, fontWeight: '700' },
  recentSub:    { color: C.dim, fontSize: 11, marginTop: 2 },
  recentRight:  { alignItems: 'flex-end' },
  recentBest:   { color: C.accent, fontSize: 15, fontWeight: '800', fontVariant: ['tabular-nums'] },
  recentLaps:   { color: C.dim, fontSize: 10, marginTop: 2 },

  pendingCard:  { backgroundColor: C.surface, borderRadius: 12, padding: 14,
                  marginBottom: 14, borderWidth: 1, borderColor: C.accent },
  pendingHead:  { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between',
                  marginBottom: 10 },
  pendingTitle: { color: C.text, fontSize: 13, fontWeight: '700', flex: 1, marginRight: 10 },
  syncBtn:      { backgroundColor: C.accent, borderRadius: 8,
                  paddingHorizontal: 12, paddingVertical: 8 },
  syncBtnTxt:   { color: '#000', fontSize: 12, fontWeight: '700' },
  pendingRow:   { flexDirection: 'row', alignItems: 'center',
                  paddingVertical: 6, borderTopWidth: 1, borderTopColor: C.border },
  pendingName:  { color: C.text, fontSize: 13, fontWeight: '600' },
  pendingSub:   { color: C.dim, fontSize: 11, marginTop: 1 },
  pendingDel:   { width: 32, height: 32, alignItems: 'center', justifyContent: 'center' },
  pendingDelTxt:{ color: C.dim, fontSize: 16, fontWeight: '700' },
  pendingHint:  { color: C.dim, fontSize: 11, marginTop: 8, fontStyle: 'italic' },

  moreBtn:      { padding: 10, alignItems: 'center', marginTop: 4 },
  moreBtnTxt:   { color: C.accent, fontSize: 13, fontWeight: '600' },

  empty:        { alignItems: 'center', paddingVertical: 40 },
  emptyIcon:    { fontSize: 48, marginBottom: 12 },
  emptyTxt:     { color: C.text, fontSize: 14, fontWeight: '600' },
  emptySub:     { color: C.dim, fontSize: 12, marginTop: 6, textAlign: 'center', paddingHorizontal: 40 },
});
