/**
 * TracksScreen — list all tracks on the device, open any of them in the
 * TrackCreator in edit mode.
 *
 * Groups:
 *   - Meine Strecken (user_created=true)   → fully editable, saved back as JSON
 *   - Datenbank      (user_created=false)  → VBOX/.tbrl imports, editable too,
 *                                            but save creates a persistent
 *                                            user-copy on SD (that's how the
 *                                            firmware POST /track works).
 */

import React, { useCallback, useState } from 'react';
import {
  View, Text, StyleSheet, TouchableOpacity, ActivityIndicator,
  FlatList, Alert, RefreshControl, TextInput,
} from 'react-native';
import { useFocusEffect } from '@react-navigation/native';
import { NativeStackNavigationProp } from '@react-navigation/native-stack';
import { RootStackParamList } from '../App';
import { C } from '../theme';
import { DeviceTrackSummary } from '../types';
import { fetchTracks, fetchTrackDetails, selectTrack, fetchStatus } from '../api';

type Props = {
  navigation: NativeStackNavigationProp<RootStackParamList, 'Tracks'>;
};

type Row = DeviceTrackSummary & { index: number };

export default function TracksScreen({ navigation }: Props) {
  const [tracks, setTracks] = useState<Row[]>([]);
  const [loading, setLoading] = useState(false);
  const [query, setQuery] = useState('');
  const [loadingIdx, setLoadingIdx] = useState<number | null>(null);
  const [activeIdx, setActiveIdx] = useState<number | null>(null);

  const refresh = useCallback(async () => {
    setLoading(true);
    try {
      const list = await fetchTracks();
      // Prefer the explicit device-side index when present (firmware >= v1.0.4).
      // Also dedupe by name as a safety net — if the firmware didn't shadow a
      // bundle entry that was edited as a user track, hide the bundle copy.
      const seenUserNames = new Set(
        list.filter(t => t.user_created).map(t => t.name.toLowerCase()));
      const deduped = list.filter(t =>
        t.user_created || !seenUserNames.has(t.name.toLowerCase()));
      setTracks(deduped.map((t, i) => ({
        ...t,
        index: typeof t.index === 'number' ? t.index : i,
      })));
      // Pull current active-track marker so the row highlights. Failure is
      // non-fatal — it just means the device runs older firmware without
      // /status, in which case we just skip the badge.
      try {
        const st = await fetchStatus();
        setActiveIdx(st.active_track_idx >= 0 ? st.active_track_idx : null);
      } catch {/* older firmware, no /status — fine */}
    } catch (e: any) {
      Alert.alert('Konnte Streckenliste nicht laden',
        e?.message ?? String(e));
    } finally {
      setLoading(false);
    }
  }, []);

  useFocusEffect(useCallback(() => { refresh(); }, [refresh]));

  async function activate(row: Row) {
    setLoadingIdx(row.index);
    try {
      const resp = await selectTrack(row.index);
      setActiveIdx(resp.active_track_idx);
      Alert.alert('Strecke aktiv',
        `${resp.active_track_name} ist jetzt aktiv.\n${resp.is_circuit ? 'Rundkurs' : 'A-B'}, ${resp.sector_count} Sektoren.`,
        [{ text: 'OK', onPress: () => navigation.navigate('Home') }]);
    } catch (e: any) {
      Alert.alert('Konnte Strecke nicht aktivieren',
        e?.message ?? String(e));
    } finally {
      setLoadingIdx(null);
    }
  }

  async function openForEdit(row: Row) {
    setLoadingIdx(row.index);
    try {
      const full = await fetchTrackDetails(row.index);
      navigation.navigate('TrackCreator', { initial: full });
    } catch (e: any) {
      Alert.alert('Konnte Streckendetails nicht laden',
        e?.message ?? String(e));
    } finally {
      setLoadingIdx(null);
    }
  }

  // Tap = activate (with confirm), long-press = open editor.
  function onRowPress(row: Row) {
    Alert.alert(
      row.name,
      'Diese Strecke aktivieren?\nDas Display zeigt danach den Timing-Bildschirm.',
      [
        { text: 'Abbrechen', style: 'cancel' },
        { text: 'Bearbeiten', onPress: () => openForEdit(row) },
        { text: 'Aktivieren', style: 'default', onPress: () => activate(row) },
      ],
    );
  }

  const filtered = query.trim().length === 0
    ? tracks
    : tracks.filter(t => {
        const q = query.toLowerCase();
        return t.name.toLowerCase().includes(q) ||
               (t.country ?? '').toLowerCase().includes(q);
      });
  const userTracks = filtered.filter(t => t.user_created);
  const dbTracks   = filtered.filter(t => !t.user_created);

  const renderItem = ({ item }: { item: Row }) => {
    const isActive = activeIdx === item.index;
    return (
      <TouchableOpacity
        style={[s.row,
                isActive && { borderColor: C.accent, borderWidth: 2 },
                loadingIdx === item.index && { opacity: 0.5 }]}
        onPress={() => onRowPress(item)}
        disabled={loadingIdx !== null}
      >
        <View style={{ flex: 1 }}>
          <Text style={s.rowName} numberOfLines={1}>
            {isActive ? '● ' : ''}{item.name}
          </Text>
          <Text style={s.rowSub}>
            {item.country || '—'}
            {item.length_km > 0 ? `  ·  ${item.length_km.toFixed(2)} km` : ''}
            {'  ·  '}{item.is_circuit ? 'Rundkurs' : 'A-B'}
            {item.sector_count > 0 ? `  ·  ${item.sector_count} Sektoren` : ''}
            {isActive ? '  ·  AKTIV' : ''}
          </Text>
        </View>
        {loadingIdx === item.index
          ? <ActivityIndicator color={C.accent} size="small" />
          : <Text style={s.chev}>›</Text>}
      </TouchableOpacity>
    );
  };

  return (
    <View style={s.root}>
      <View style={s.searchBar}>
        <TextInput
          style={s.search}
          placeholder="Suchen (Name oder Land)…"
          placeholderTextColor={C.dim}
          value={query}
          onChangeText={setQuery}
          autoCorrect={false}
        />
        <TouchableOpacity
          style={s.newBtn}
          onPress={() => navigation.navigate('TrackCreator', undefined)}
        >
          <Text style={s.newBtnTxt}>+ Neu</Text>
        </TouchableOpacity>
      </View>

      {loading && tracks.length === 0 ? (
        <View style={s.loadingBox}>
          <ActivityIndicator color={C.accent} size="large" />
          <Text style={s.loadingTxt}>Lade Strecken vom Gerät…</Text>
        </View>
      ) : (
        <FlatList
          data={[
            ...(userTracks.length > 0
              ? [{ _header: `Meine Strecken (${userTracks.length})` } as any]
              : []),
            ...userTracks,
            ...(dbTracks.length > 0
              ? [{ _header: `Datenbank (${dbTracks.length})` } as any]
              : []),
            ...dbTracks,
          ]}
          keyExtractor={(it: any, i) =>
            it._header ? `h${i}` : `t${it.index}`}
          renderItem={({ item }: any) =>
            item._header
              ? <Text style={s.section}>{item._header}</Text>
              : renderItem({ item })
          }
          refreshControl={
            <RefreshControl
              refreshing={loading}
              onRefresh={refresh}
              tintColor={C.accent}
            />
          }
          ListEmptyComponent={
            <Text style={s.empty}>
              {tracks.length === 0
                ? 'Gerät hat keine Strecken gemeldet.\n\nLade via "UPDATE" im Display oder erstelle eine neue.'
                : 'Keine Treffer für die Suche.'}
            </Text>
          }
          contentContainerStyle={{ paddingBottom: 24 }}
        />
      )}
    </View>
  );
}

const s = StyleSheet.create({
  root:         { flex: 1, backgroundColor: C.bg },
  searchBar:    { flexDirection: 'row', alignItems: 'center', gap: 8,
                  padding: 10, backgroundColor: C.surface,
                  borderBottomWidth: 1, borderBottomColor: C.border },
  search:       { flex: 1, backgroundColor: C.surface2, color: C.text,
                  borderRadius: 8, paddingHorizontal: 12, paddingVertical: 9,
                  fontSize: 14 },
  newBtn:       { backgroundColor: C.accent, borderRadius: 8,
                  paddingHorizontal: 14, paddingVertical: 9 },
  newBtnTxt:    { color: '#000', fontWeight: '800', fontSize: 14 },

  section:      { color: C.dim, fontSize: 11, fontWeight: '700',
                  textTransform: 'uppercase',
                  paddingHorizontal: 14, paddingTop: 14, paddingBottom: 6 },

  row:          { flexDirection: 'row', alignItems: 'center',
                  backgroundColor: C.surface,
                  marginHorizontal: 10, marginVertical: 3,
                  paddingHorizontal: 14, paddingVertical: 12,
                  borderRadius: 10,
                  borderWidth: 1, borderColor: C.border },
  rowName:      { color: C.text, fontSize: 15, fontWeight: '700' },
  rowSub:       { color: C.dim, fontSize: 11, marginTop: 2 },
  chev:         { color: C.dim, fontSize: 24, marginLeft: 10, lineHeight: 24 },

  loadingBox:   { flex: 1, alignItems: 'center', justifyContent: 'center' },
  loadingTxt:   { color: C.dim, marginTop: 14, fontSize: 13 },
  empty:        { color: C.dim, fontSize: 13, textAlign: 'center',
                  padding: 28, lineHeight: 20 },
});
