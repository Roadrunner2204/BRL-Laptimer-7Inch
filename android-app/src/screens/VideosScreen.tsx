/**
 * VideosScreen — browse, play, and delete the AVI files that live on the
 * laptimer's HDD, independent of the session metadata. Since the firmware
 * switched to per-lap video splits (2026-04-16), one session has N video
 * files on the device — this screen is the natural place to prune them.
 */
import React, { useState, useCallback } from 'react';
import {
  View, Text, FlatList, TouchableOpacity, StyleSheet,
  ActivityIndicator, Alert, RefreshControl,
} from 'react-native';
import * as FileSystem from 'expo-file-system';
import { useFocusEffect } from '@react-navigation/native';
import { NativeStackNavigationProp } from '@react-navigation/native-stack';
import { RootStackParamList } from '../App';
import { fetchVideoList, deleteVideoOnDevice, DeviceVideo } from '../api';
import { C } from '../theme';

type Props = {
  navigation: NativeStackNavigationProp<RootStackParamList, 'Videos'>;
};

// Parse a video-file stem (basename without .avi) into a human label.
// Firmware stems look like:
//   YYYYMMDD_HHMMSS_lapN   – per-lap video inside a timed session
//   YYYYMMDD_HHMMSS        – legacy single-file session recording
//   REC_<ms>_lapN          – manual recording started mid-session
//   REC_<ms>               – manual recording, no session context
function parseStem(stem: string): { label: string; sub: string } {
  let m = stem.match(/^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})_lap(\d+)$/);
  if (m) return {
    label: `Runde ${m[7]}`,
    sub:   `${m[3]}.${m[2]}.${m[1]}  ${m[4]}:${m[5]}`,
  };
  m = stem.match(/^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})$/);
  if (m) return {
    label: 'Session (gesamt)',
    sub:   `${m[3]}.${m[2]}.${m[1]}  ${m[4]}:${m[5]}`,
  };
  m = stem.match(/^REC_\d+_lap(\d+)$/);
  if (m) return { label: `Manuell · Runde ${m[1]}`, sub: stem };
  if (stem.startsWith('REC_')) return { label: 'Manuelle Aufnahme', sub: stem };
  return { label: stem, sub: '' };
}

// Recover the parent session ID (YYYYMMDD_HHMMSS) from a lap-video stem so
// the player can still load session channels for the overlay.
function sessionIdFromStem(stem: string): string | undefined {
  const m = stem.match(/^(\d{8}_\d{6})(?:_lap\d+)?$/);
  return m ? m[1] : undefined;
}

async function deleteLocalCache(stem: string) {
  const uri = FileSystem.documentDirectory + `videos/${stem}.avi`;
  try { await FileSystem.deleteAsync(uri, { idempotent: true }); }
  catch { /* best-effort — no local copy, nothing to do */ }
}

export default function VideosScreen({ navigation }: Props) {
  const [videos, setVideos] = useState<DeviceVideo[]>([]);
  const [refreshing, setRefreshing] = useState(false);
  const [selectMode, setSelectMode] = useState(false);
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    setRefreshing(true);
    setError(null);
    try {
      const list = await fetchVideoList();
      // Newest first — stems starting with YYYYMMDD_HHMMSS sort correctly,
      // REC_<ms>_lap ones sort by millis too. Fallback: lexical.
      list.sort((a, b) => b.id.localeCompare(a.id));
      setVideos(list);
    } catch (e: any) {
      setError(e?.message ?? String(e));
      setVideos([]);
    } finally {
      setRefreshing(false);
    }
  }, []);

  useFocusEffect(useCallback(() => { refresh(); }, [refresh]));

  function toggleSelect(id: string) {
    const next = new Set(selected);
    if (next.has(id)) next.delete(id); else next.add(id);
    setSelected(next);
  }

  function exitSelectMode() {
    setSelectMode(false);
    setSelected(new Set());
  }

  async function deleteOne(id: string) {
    Alert.alert('Video löschen?',
      'Das Video wird unwiderruflich von der HDD gelöscht.',
      [
        { text: 'Abbrechen', style: 'cancel' },
        { text: 'Löschen', style: 'destructive', onPress: async () => {
          try {
            await deleteVideoOnDevice(id);
            await deleteLocalCache(id);
            await refresh();
          } catch (e: any) {
            Alert.alert('Fehler', e?.message ?? String(e));
          }
        }},
      ]);
  }

  async function bulkDelete() {
    if (selected.size === 0) return;
    const targets = Array.from(selected);
    Alert.alert(
      `${targets.length} Video${targets.length === 1 ? '' : 's'} löschen?`,
      'Die Dateien werden von der HDD entfernt. Nicht rückgängig.',
      [
        { text: 'Abbrechen', style: 'cancel' },
        { text: 'Löschen', style: 'destructive', onPress: async () => {
          setBusy(true);
          let okCount = 0;
          const fails: string[] = [];
          for (const id of targets) {
            try {
              await deleteVideoOnDevice(id);
              await deleteLocalCache(id);
              okCount++;
            } catch (e: any) {
              fails.push(`${id}: ${e?.message ?? String(e)}`);
            }
          }
          setBusy(false);
          exitSelectMode();
          await refresh();
          if (fails.length === 0) {
            Alert.alert('Fertig', `${okCount} Video${okCount === 1 ? '' : 's'} gelöscht.`);
          } else {
            Alert.alert('Teilweise fehlgeschlagen',
              `${okCount} OK, ${fails.length} Fehler:\n\n${fails.join('\n')}`);
          }
        }},
      ]);
  }

  const totalBytes = videos.reduce((n, v) => n + v.size, 0);

  return (
    <View style={s.root}>
      {/* Header summary */}
      <View style={s.headerCard}>
        <Text style={s.headerTitle}>
          {videos.length} Video{videos.length === 1 ? '' : 's'}
        </Text>
        <Text style={s.headerSub}>
          {(totalBytes / (1024 * 1024)).toFixed(1)} MB belegt auf HDD
        </Text>
      </View>

      {/* Select-mode action bar */}
      <View style={s.selectBar}>
        {!selectMode ? (
          <TouchableOpacity style={s.selectStartBtn} onPress={() => setSelectMode(true)}
                            disabled={videos.length === 0}>
            <Text style={[s.selectStartTxt, videos.length === 0 && { opacity: 0.4 }]}>
              Mehrfach auswählen
            </Text>
          </TouchableOpacity>
        ) : (
          <View style={s.selectActions}>
            <TouchableOpacity style={s.selectExitBtn} onPress={exitSelectMode}>
              <Text style={s.selectExitTxt}>✕ Abbrechen</Text>
            </TouchableOpacity>
            <Text style={s.selectCount}>{selected.size} ausgewählt</Text>
            <TouchableOpacity
              style={[s.selectAllBtn, selected.size === videos.length && s.selectAllBtnActive]}
              onPress={() => {
                if (selected.size === videos.length) setSelected(new Set());
                else setSelected(new Set(videos.map(v => v.id)));
              }}
            >
              <Text style={s.selectAllTxt}>
                {selected.size === videos.length ? 'Keine' : 'Alle'}
              </Text>
            </TouchableOpacity>
            <TouchableOpacity
              style={[s.selectDelBtn, (selected.size === 0 || busy) && { opacity: 0.4 }]}
              onPress={bulkDelete}
              disabled={selected.size === 0 || busy}
            >
              {busy
                ? <ActivityIndicator color="#fff" size="small" />
                : <Text style={s.selectDelTxt}>🗑  Löschen</Text>}
            </TouchableOpacity>
          </View>
        )}
      </View>

      {error && (
        <Text style={s.errorLine}>Gerät nicht erreichbar: {error}</Text>
      )}

      <FlatList
        data={videos}
        keyExtractor={v => v.id}
        refreshControl={<RefreshControl refreshing={refreshing} onRefresh={refresh} tintColor={C.accent} />}
        contentContainerStyle={{ padding: 12, paddingBottom: 32 }}
        ListEmptyComponent={
          !refreshing ? (
            <Text style={s.empty}>
              {error ? 'Verbindung prüfen und erneut laden.' : 'Keine Videos auf dem Gerät'}
            </Text>
          ) : null
        }
        renderItem={({ item: v }) => {
          const meta = parseStem(v.id);
          const mb = (v.size / (1024 * 1024)).toFixed(1);
          const isSelected = selected.has(v.id);
          return (
            <TouchableOpacity
              style={[s.card, isSelected && s.cardSelected]}
              onPress={() => {
                if (selectMode) {
                  toggleSelect(v.id);
                } else {
                  navigation.navigate('Video', {
                    videoId:   v.id,
                    sessionId: sessionIdFromStem(v.id),
                    mode:      'stream',
                  });
                }
              }}
              onLongPress={() => {
                if (!selectMode) {
                  setSelectMode(true);
                  toggleSelect(v.id);
                }
              }}
              activeOpacity={0.6}
            >
              {selectMode && (
                <View style={s.checkbox}>
                  <Text style={s.checkboxMark}>{isSelected ? '✓' : ''}</Text>
                </View>
              )}
              <View style={{ flex: 1 }}>
                <Text style={s.cardTitle} numberOfLines={1}>🎥  {meta.label}</Text>
                {meta.sub.length > 0 && (
                  <Text style={s.cardSub} numberOfLines={1}>{meta.sub}</Text>
                )}
                <Text style={s.cardStem} numberOfLines={1}>{v.id}.avi  ·  {mb} MB</Text>
              </View>
              {!selectMode && (
                <TouchableOpacity style={s.rowDelBtn} onPress={() => deleteOne(v.id)}>
                  <Text style={s.rowDelTxt}>✕</Text>
                </TouchableOpacity>
              )}
            </TouchableOpacity>
          );
        }}
      />
    </View>
  );
}

const s = StyleSheet.create({
  root:          { flex: 1, backgroundColor: C.bg },

  headerCard:    { backgroundColor: C.surface, margin: 12, marginBottom: 6,
                   borderRadius: 10, padding: 14 },
  headerTitle:   { color: C.text, fontSize: 17, fontWeight: '700' },
  headerSub:     { color: C.dim, fontSize: 12, marginTop: 2 },

  empty:         { color: C.dim, textAlign: 'center', marginTop: 48, fontSize: 15 },
  errorLine:     { color: C.danger, fontSize: 12, textAlign: 'center',
                   paddingHorizontal: 20, paddingBottom: 8 },

  card:          { backgroundColor: C.surface, borderRadius: 10, padding: 14,
                   marginBottom: 8, flexDirection: 'row', alignItems: 'center' },
  cardSelected:  { borderWidth: 2, borderColor: C.accent },
  cardTitle:     { color: C.text, fontSize: 15, fontWeight: '700' },
  cardSub:       { color: C.accent, fontSize: 12, marginTop: 2, fontWeight: '600' },
  cardStem:      { color: C.dim, fontSize: 11, marginTop: 4, fontVariant: ['tabular-nums'] },
  rowDelBtn:     { width: 34, height: 34, borderRadius: 17, alignItems: 'center',
                   justifyContent: 'center', backgroundColor: C.surface2,
                   borderWidth: 1, borderColor: C.border, marginLeft: 8 },
  rowDelTxt:     { color: C.danger, fontSize: 14, fontWeight: '800' },

  // Multi-select bar (mirrors SessionsScreen style)
  selectBar:         { paddingHorizontal: 12, paddingBottom: 8 },
  selectStartBtn:    { backgroundColor: C.surface2, borderRadius: 8, paddingVertical: 8,
                       alignItems: 'center', borderWidth: 1, borderColor: C.border },
  selectStartTxt:    { color: C.dim, fontSize: 13, fontWeight: '600' },
  selectActions:     { flexDirection: 'row', alignItems: 'center', gap: 6 },
  selectExitBtn:     { paddingHorizontal: 12, paddingVertical: 8, borderRadius: 8,
                       backgroundColor: C.surface2, borderWidth: 1, borderColor: C.border },
  selectExitTxt:     { color: C.dim, fontSize: 12, fontWeight: '700' },
  selectCount:       { color: C.text, fontSize: 13, fontWeight: '700',
                       flex: 1, textAlign: 'center' },
  selectAllBtn:      { paddingHorizontal: 12, paddingVertical: 8, borderRadius: 8,
                       backgroundColor: C.surface2, borderWidth: 1, borderColor: C.border },
  selectAllBtnActive:{ backgroundColor: C.accent, borderColor: C.accent },
  selectAllTxt:      { color: C.text, fontSize: 12, fontWeight: '700' },
  selectDelBtn:      { paddingHorizontal: 14, paddingVertical: 8, borderRadius: 8,
                       backgroundColor: C.danger ?? '#FF3B30' },
  selectDelTxt:      { color: '#fff', fontSize: 13, fontWeight: '800' },

  checkbox:          { position: 'absolute', top: 10, right: 10, width: 24, height: 24,
                       borderRadius: 12, borderWidth: 2, borderColor: C.accent,
                       alignItems: 'center', justifyContent: 'center',
                       backgroundColor: C.bg },
  checkboxMark:      { color: C.accent, fontSize: 14, fontWeight: '900', lineHeight: 14 },
});
