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
  const [selectMode, setSelectMode]   = useState(false);
  const [selected, setSelected]       = useState<Set<string>>(new Set());
  const [bulkBusy, setBulkBusy]       = useState(false);

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

  function toggleSelect(id: string) {
    const next = new Set(selected);
    if (next.has(id)) next.delete(id); else next.add(id);
    setSelected(next);
  }

  function exitSelectMode() {
    setSelectMode(false);
    setSelected(new Set());
  }

  async function bulkDelete() {
    if (selected.size === 0) return;
    const targets = Array.from(selected);
    Alert.alert(
      `${targets.length} Session${targets.length === 1 ? '' : 's'} löschen?`,
      showDevice
        ? 'Diese Sessions werden vom Gerät UND aus der lokalen Kopie (falls vorhanden) entfernt. Nicht rückgängig.'
        : 'Lokale Kopie wird entfernt.',
      [
        { text: 'Abbrechen', style: 'cancel' },
        { text: 'Löschen', style: 'destructive', onPress: async () => {
          setBulkBusy(true);
          let okCount = 0;
          const fails: string[] = [];
          for (const id of targets) {
            try {
              if (showDevice) await deleteSessionOnDevice(id);
              await deleteSession(id);   // also drop local copy if any
              okCount++;
            } catch (e: any) {
              fails.push(`${id}: ${e?.message ?? String(e)}`);
            }
          }
          setBulkBusy(false);
          exitSelectMode();
          await refresh();
          if (fails.length === 0) {
            Alert.alert('Fertig', `${okCount} Session${okCount === 1 ? '' : 's'} gelöscht.`);
          } else {
            Alert.alert('Teilweise fehlgeschlagen',
              `${okCount} OK, ${fails.length} Fehler:\n\n${fails.join('\n')}`);
          }
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
        <TouchableOpacity
          style={[s.tab, !showDevice && s.tabActive]}
          onPress={() => { exitSelectMode(); setShowDevice(false); }}
        >
          <Text style={[s.tabTxt, !showDevice && s.tabTxtActive]}>Lokal ({local.length})</Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={[s.tab, showDevice && s.tabActive]}
          onPress={() => { exitSelectMode(); setShowDevice(true); refresh(); }}
        >
          <Text style={[s.tabTxt, showDevice && s.tabTxtActive]}>Gerät ({deviceSessions.length})</Text>
        </TouchableOpacity>
      </View>

      {/* Select-mode action bar */}
      <View style={s.selectBar}>
        {!selectMode ? (
          <TouchableOpacity style={s.selectStartBtn} onPress={() => setSelectMode(true)}>
            <Text style={s.selectStartTxt}>Mehrfach auswählen</Text>
          </TouchableOpacity>
        ) : (
          <View style={s.selectActions}>
            <TouchableOpacity style={s.selectExitBtn} onPress={exitSelectMode}>
              <Text style={s.selectExitTxt}>✕ Abbrechen</Text>
            </TouchableOpacity>
            <Text style={s.selectCount}>{selected.size} ausgewählt</Text>
            <TouchableOpacity
              style={[s.selectAllBtn, selected.size === rows.length && s.selectAllBtnActive]}
              onPress={() => {
                if (selected.size === rows.length) setSelected(new Set());
                else setSelected(new Set(rows.map(r => r.id)));
              }}
            >
              <Text style={s.selectAllTxt}>
                {selected.size === rows.length ? 'Keine' : 'Alle'}
              </Text>
            </TouchableOpacity>
            <TouchableOpacity
              style={[s.selectDelBtn, (selected.size === 0 || bulkBusy) && { opacity: 0.4 }]}
              onPress={bulkDelete}
              disabled={selected.size === 0 || bulkBusy}
            >
              {bulkBusy
                ? <ActivityIndicator color="#fff" size="small" />
                : <Text style={s.selectDelTxt}>🗑  Löschen</Text>}
            </TouchableOpacity>
          </View>
        )}
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
          const isSelected = selected.has(r.id);
          return (
            <TouchableOpacity
              style={[s.card, isSelected && s.cardSelected]}
              onPress={() => {
                if (selectMode) toggleSelect(r.id);
                else if (r.isLocal) openSession(r.id);
              }}
              onLongPress={() => {
                if (!selectMode) {
                  setSelectMode(true);
                  toggleSelect(r.id);
                }
              }}
              disabled={!selectMode && !r.isLocal}
              activeOpacity={selectMode || r.isLocal ? 0.6 : 1}
            >
              {selectMode && (
                <View style={s.checkbox}>
                  <Text style={s.checkboxMark}>{isSelected ? '✓' : ''}</Text>
                </View>
              )}
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

              {/* Video badge — per-lap naming (<sid>_lapN) means one session
                  can have many videos. Count + sum them; tap navigates to
                  the Videos list for selection. Legacy <sid>.avi still
                  matches and plays directly. */}
              {(() => {
                const laps = deviceVideos.filter(
                  v => v.id === r.id || v.id.startsWith(r.id + '_lap'));
                if (laps.length === 0) return null;
                const mb = (laps.reduce((n, v) => n + v.size, 0) / (1024 * 1024)).toFixed(1);
                const legacy = laps.length === 1 && laps[0].id === r.id;
                return (
                  <TouchableOpacity
                    style={s.videoBadge}
                    onPress={() => {
                      if (legacy) {
                        navigation.navigate('Video', {
                          videoId:   r.id,
                          sessionId: r.id,
                          mode:      'stream',
                        });
                      } else {
                        navigation.navigate('Videos');
                      }
                    }}
                  >
                    <Text style={s.videoBadgeTxt}>
                      🎥  {laps.length === 1 ? 'Video' : `${laps.length} Videos`}  ·  {mb} MB
                    </Text>
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

  // Multi-select bar
  selectBar:        { paddingHorizontal:12, paddingBottom:8 },
  selectStartBtn:   { backgroundColor: C.surface2, borderRadius:8, paddingVertical:8,
                      alignItems:'center', borderWidth:1, borderColor: C.border },
  selectStartTxt:   { color: C.dim, fontSize:13, fontWeight:'600' },
  selectActions:    { flexDirection:'row', alignItems:'center', gap:6 },
  selectExitBtn:    { paddingHorizontal:12, paddingVertical:8, borderRadius:8,
                      backgroundColor: C.surface2, borderWidth:1, borderColor: C.border },
  selectExitTxt:    { color: C.dim, fontSize:12, fontWeight:'700' },
  selectCount:      { color: C.text, fontSize:13, fontWeight:'700', flex:1, textAlign:'center' },
  selectAllBtn:     { paddingHorizontal:12, paddingVertical:8, borderRadius:8,
                      backgroundColor: C.surface2, borderWidth:1, borderColor: C.border },
  selectAllBtnActive:{ backgroundColor: C.accent, borderColor: C.accent },
  selectAllTxt:     { color: C.text, fontSize:12, fontWeight:'700' },
  selectDelBtn:     { paddingHorizontal:14, paddingVertical:8, borderRadius:8,
                      backgroundColor: C.danger ?? '#FF3B30' },
  selectDelTxt:     { color:'#fff', fontSize:13, fontWeight:'800' },

  // Selected card visual
  cardSelected:     { borderWidth:2, borderColor: C.accent },
  checkbox:         { position:'absolute', top:10, right:10, width:24, height:24,
                      borderRadius:12, borderWidth:2, borderColor: C.accent,
                      alignItems:'center', justifyContent:'center',
                      backgroundColor: C.bg },
  checkboxMark:     { color: C.accent, fontSize:14, fontWeight:'900', lineHeight:14 },
});
