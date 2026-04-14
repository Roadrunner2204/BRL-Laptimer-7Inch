import React, { useState, useCallback } from 'react';
import {
  View, Text, FlatList, TouchableOpacity, StyleSheet,
  ActivityIndicator, Alert, RefreshControl,
} from 'react-native';
import { useFocusEffect } from '@react-navigation/native';
import { NativeStackNavigationProp } from '@react-navigation/native-stack';
import { RouteProp } from '@react-navigation/native';
import { RootStackParamList } from '../App';
import { fetchSessionList, fetchSession, deleteSessionOnDevice,
         fetchVideoList, DeviceVideo } from '../api';
import { listSessionSummaries, saveSession, deleteSession, loadSession } from '../storage';
import { SessionSummary, DeviceSessionSummary } from '../types';
import { fmtTime, fmtDate } from '../utils';
import { C } from '../theme';

type Props = {
  navigation: NativeStackNavigationProp<RootStackParamList, 'Sessions'>;
  route: RouteProp<RootStackParamList, 'Sessions'>;
};

// Parse the timestamp in the session ID "YYYYMMDD_HHmmss" into a pretty string.
// Falls back to the raw ID if format doesn't match.
function parseSessionDate(id: string): string {
  const m = id.match(/^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})$/);
  if (!m) return id;
  const [, y, mo, d, h, mi] = m;
  return `${d}.${mo}.${y}  ${h}:${mi}`;
}

export default function SessionsScreen({ navigation, route }: Props) {
  const [deviceSessions, setDeviceSessions] = useState<DeviceSessionSummary[]>([]);
  const [deviceVideos,   setDeviceVideos]   = useState<DeviceVideo[]>([]);
  const [local, setLocal]             = useState<SessionSummary[]>([]);
  const [downloading, setDownloading] = useState<string | null>(null);
  const [refreshing, setRefreshing]   = useState(false);
  const [showDevice, setShowDevice]   = useState(route.params?.mode === 'device');

  const refresh = useCallback(async () => {
    setRefreshing(true);
    try {
      const summaries = await listSessionSummaries();
      setLocal(summaries);
      if (showDevice) {
        const list = await fetchSessionList();
        setDeviceSessions(list);
        // Fetch video list in parallel — failure is non-fatal.
        try {
          const vids = await fetchVideoList();
          setDeviceVideos(vids);
        } catch (e) {
          setDeviceVideos([]);
        }
      }
    } catch (e: any) {
      if (showDevice) Alert.alert('Fehler', 'Gerät nicht erreichbar: ' + e.message);
    } finally {
      setRefreshing(false);
    }
  }, [showDevice]);

  useFocusEffect(useCallback(() => { refresh(); }, [refresh]));

  const localIds = new Set(local.map(s => s.id));

  async function download(id: string) {
    setDownloading(id);
    try {
      const session = await fetchSession(id);
      await saveSession(session);
      setLocal(await listSessionSummaries());
    } catch (e: any) {
      Alert.alert('Fehler', 'Download fehlgeschlagen: ' + e.message);
    } finally {
      setDownloading(null);
    }
  }

  async function openSession(id: string) {
    const s = await loadSession(id);
    if (s) navigation.navigate('Detail', { sessionId: id });
  }

  async function confirmDelete(id: string) {
    Alert.alert('Session löschen', 'Lokal gespeicherte Session löschen?', [
      { text: 'Abbrechen', style: 'cancel' },
      { text: 'Löschen', style: 'destructive', onPress: async () => {
        await deleteSession(id);
        setLocal(await listSessionSummaries());
      }},
    ]);
  }

  // Unified render: device view shows device summaries merged with local flag,
  // local view shows only downloaded sessions.
  type Row = {
    id: string;
    name: string;
    track: string;
    lap_count: number;
    best_ms: number;
    date: string;
    isLocal: boolean;
    fromDevice: boolean;
    downloaded_at?: number;
  };

  const rows: Row[] = showDevice
    ? deviceSessions.map(d => {
        const loc = local.find(l => l.id === d.id);
        return {
          id: d.id,
          name: d.name || d.id,
          track: d.track || '—',
          lap_count: d.lap_count,
          best_ms: d.best_ms,
          date: parseSessionDate(d.id),
          isLocal: !!loc,
          fromDevice: true,
          downloaded_at: loc?.downloaded_at,
        };
      })
    : local.map(l => ({
        id: l.id,
        name: l.name,
        track: l.track || '—',
        lap_count: l.lap_count,
        best_ms: l.best_ms,
        date: parseSessionDate(l.id),
        isLocal: true,
        fromDevice: false,
        downloaded_at: l.downloaded_at,
      }));

  return (
    <View style={s.root}>
      <View style={s.toggle}>
        <TouchableOpacity style={[s.tab, !showDevice && s.tabActive]} onPress={() => setShowDevice(false)}>
          <Text style={[s.tabTxt, !showDevice && s.tabTxtActive]}>Lokal ({local.length})</Text>
        </TouchableOpacity>
        <TouchableOpacity style={[s.tab, showDevice && s.tabActive]} onPress={() => { setShowDevice(true); refresh(); }}>
          <Text style={[s.tabTxt, showDevice && s.tabTxtActive]}>Gerät ({deviceSessions.length})</Text>
        </TouchableOpacity>
      </View>

      <FlatList
        data={rows}
        keyExtractor={r => r.id}
        refreshControl={<RefreshControl refreshing={refreshing} onRefresh={refresh} tintColor={C.accent} />}
        contentContainerStyle={{ padding: 12, paddingBottom: 32 }}
        ListEmptyComponent={
          <Text style={s.empty}>
            {showDevice ? 'Keine Sessions auf dem Gerät' : 'Noch keine Sessions gespeichert'}
          </Text>
        }
        renderItem={({ item: r }) => {
          const isDownloading = downloading === r.id;
          return (
            <TouchableOpacity
              style={s.card}
              onPress={() => r.isLocal ? openSession(r.id) : undefined}
              disabled={!r.isLocal}
              activeOpacity={r.isLocal ? 0.6 : 1}
            >
              {/* Header line: date/time + saved badge */}
              <View style={s.cardTop}>
                <Text style={s.date}>{r.date}</Text>
                {r.isLocal && <Text style={s.savedBadge}>✓ Gespeichert</Text>}
              </View>

              {/* Name + track */}
              <Text style={s.name} numberOfLines={1}>{r.name}</Text>
              <Text style={s.track} numberOfLines={1}>{r.track}</Text>

              {/* Stats */}
              <View style={s.statsRow}>
                <View style={s.stat}>
                  <Text style={s.statVal}>{r.lap_count}</Text>
                  <Text style={s.statLbl}>Runden</Text>
                </View>
                <View style={s.stat}>
                  <Text style={[s.statVal, { color: C.accent }]}>
                    {r.best_ms > 0 ? fmtTime(r.best_ms) : '—'}
                  </Text>
                  <Text style={s.statLbl}>Bestzeit</Text>
                </View>
                {r.downloaded_at ? (
                  <View style={s.stat}>
                    <Text style={s.statVal}>{fmtDate(r.downloaded_at)}</Text>
                    <Text style={s.statLbl}>Geladen</Text>
                  </View>
                ) : (
                  <View style={s.stat}>
                    <Text style={[s.statVal, { color: C.dim }]}>—</Text>
                    <Text style={s.statLbl}>Geladen</Text>
                  </View>
                )}
              </View>

              {/* Video badge (if video exists for this session on device) */}
              {(() => {
                const video = deviceVideos.find(v => v.id === r.id);
                if (!video) return null;
                const mb = (video.size / (1024 * 1024)).toFixed(1);
                return (
                  <TouchableOpacity
                    style={s.videoBadge}
                    onPress={() => navigation.navigate('Video', { videoId: r.id, mode: 'stream' })}
                  >
                    <Text style={s.videoBadgeTxt}>🎥  Video {mb} MB</Text>
                  </TouchableOpacity>
                );
              })()}

              {/* Actions */}
              <View style={s.actions}>
                {r.isLocal && (
                  <TouchableOpacity style={s.actionBtn} onPress={() => openSession(r.id)}>
                    <Text style={s.actionTxt}>Ansehen</Text>
                  </TouchableOpacity>
                )}
                {r.fromDevice && !r.isLocal && (
                  <TouchableOpacity
                    style={[s.actionBtn, s.dlBtn]}
                    onPress={() => download(r.id)}
                    disabled={isDownloading}
                  >
                    {isDownloading
                      ? <ActivityIndicator color="#000" size="small" />
                      : <Text style={[s.actionTxt, { color: '#000' }]}>Herunterladen</Text>}
                  </TouchableOpacity>
                )}
                {r.isLocal && (
                  <TouchableOpacity style={[s.actionBtn, s.delBtn]} onPress={() => confirmDelete(r.id)}>
                    <Text style={[s.actionTxt, { color: C.danger }]}>Löschen</Text>
                  </TouchableOpacity>
                )}
              </View>
            </TouchableOpacity>
          );
        }}
      />
    </View>
  );
}

const s = StyleSheet.create({
  root:         { flex:1, backgroundColor: C.bg },
  toggle:       { flexDirection:'row', margin:12, backgroundColor: C.surface, borderRadius:8, padding:3 },
  tab:          { flex:1, paddingVertical:8, alignItems:'center', borderRadius:6 },
  tabActive:    { backgroundColor: C.accent },
  tabTxt:       { color: C.dim, fontWeight:'600', fontSize:14 },
  tabTxtActive: { color: '#000' },
  empty:        { color: C.dim, textAlign:'center', marginTop:48, fontSize:15 },
  card:         { backgroundColor: C.surface, borderRadius:12, padding:16, marginBottom:10 },
  cardTop:      { flexDirection:'row', justifyContent:'space-between', alignItems:'center', marginBottom:8 },
  date:         { color: C.accent, fontSize:13, fontWeight:'700', letterSpacing:0.5 },
  savedBadge:   { color: C.accent, fontSize:11, fontWeight:'600' },
  name:         { color: C.text, fontSize:17, fontWeight:'700' },
  track:        { color: C.dim, fontSize:13, marginTop:2, marginBottom:12 },
  statsRow:     { flexDirection:'row', marginBottom:12 },
  stat:         { flex:1 },
  statVal:      { color: C.text, fontSize:15, fontWeight:'700' },
  statLbl:      { color: C.dim, fontSize:11, marginTop:2 },
  actions:      { flexDirection:'row', gap:8 },
  actionBtn:    { flex:1, borderRadius:6, borderWidth:1, borderColor:'#333', padding:8, alignItems:'center' },
  dlBtn:        { backgroundColor: C.accent, borderColor: C.accent },
  delBtn:       { borderColor: C.danger },
  actionTxt:    { color: C.text, fontWeight:'600', fontSize:13 },

  videoBadge:   { flexDirection:'row', alignItems:'center', paddingVertical:6,
                  paddingHorizontal:10, borderRadius:6, backgroundColor: C.surface2,
                  borderWidth:1, borderColor: C.accent, marginBottom:8,
                  alignSelf:'flex-start' },
  videoBadgeTxt:{ color: C.accent, fontSize:12, fontWeight:'600' },
});
