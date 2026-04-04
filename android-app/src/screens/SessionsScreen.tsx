import React, { useState, useCallback } from 'react';
import {
  View, Text, FlatList, TouchableOpacity, StyleSheet,
  ActivityIndicator, Alert, RefreshControl, Switch,
} from 'react-native';
import { useFocusEffect } from '@react-navigation/native';
import { NativeStackNavigationProp } from '@react-navigation/native-stack';
import { RouteProp } from '@react-navigation/native';
import { RootStackParamList } from '../App';
import { fetchSessionList, fetchSession, deleteSessionOnDevice } from '../api';
import { listSessionSummaries, saveSession, deleteSession, loadSession } from '../storage';
import { SessionSummary } from '../types';
import { fmtTime, fmtDate } from '../utils';
import { C } from '../theme';

type Props = {
  navigation: NativeStackNavigationProp<RootStackParamList, 'Sessions'>;
  route: RouteProp<RootStackParamList, 'Sessions'>;
};

export default function SessionsScreen({ navigation, route }: Props) {
  const [deviceIds, setDeviceIds]     = useState<string[]>([]);
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
        const ids = await fetchSessionList();
        setDeviceIds(ids);
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

  const displayIds = showDevice ? deviceIds : local.map(s => s.id);

  return (
    <View style={s.root}>
      {/* Toggle */}
      <View style={s.toggle}>
        <TouchableOpacity style={[s.tab, !showDevice && s.tabActive]} onPress={() => setShowDevice(false)}>
          <Text style={[s.tabTxt, !showDevice && s.tabTxtActive]}>Lokal ({local.length})</Text>
        </TouchableOpacity>
        <TouchableOpacity style={[s.tab, showDevice && s.tabActive]} onPress={() => { setShowDevice(true); refresh(); }}>
          <Text style={[s.tabTxt, showDevice && s.tabTxtActive]}>Gerät</Text>
        </TouchableOpacity>
      </View>

      <FlatList
        data={displayIds}
        keyExtractor={id => id}
        refreshControl={<RefreshControl refreshing={refreshing} onRefresh={refresh} tintColor={C.accent} />}
        contentContainerStyle={{ padding: 12, paddingBottom: 32 }}
        ListEmptyComponent={
          <Text style={s.empty}>
            {showDevice ? 'Keine Sessions auf dem Gerät' : 'Noch keine Sessions gespeichert'}
          </Text>
        }
        renderItem={({ item: id }) => {
          const summary = local.find(s => s.id === id);
          const isLocal = localIds.has(id);
          const isDownloading = downloading === id;
          return (
            <TouchableOpacity
              style={s.card}
              onPress={() => isLocal ? openSession(id) : undefined}
              disabled={!isLocal}
            >
              <View style={s.cardTop}>
                <Text style={s.track}>{summary?.track ?? id}</Text>
                {isLocal && <Text style={s.savedBadge}>✓ Gespeichert</Text>}
              </View>

              {summary && (
                <View style={s.row}>
                  <View style={s.stat}>
                    <Text style={s.statVal}>{summary.lap_count}</Text>
                    <Text style={s.statLbl}>Runden</Text>
                  </View>
                  <View style={s.stat}>
                    <Text style={[s.statVal, { color: C.accent }]}>{fmtTime(summary.best_ms)}</Text>
                    <Text style={s.statLbl}>Bestzeit</Text>
                  </View>
                  <View style={s.stat}>
                    <Text style={s.statVal}>{fmtDate(summary.downloaded_at)}</Text>
                    <Text style={s.statLbl}>Gespeichert</Text>
                  </View>
                </View>
              )}

              <View style={s.actions}>
                {isLocal && (
                  <TouchableOpacity style={s.actionBtn} onPress={() => openSession(id)}>
                    <Text style={s.actionTxt}>Ansehen</Text>
                  </TouchableOpacity>
                )}
                {showDevice && !isLocal && (
                  <TouchableOpacity style={[s.actionBtn, s.dlBtn]} onPress={() => download(id)} disabled={isDownloading}>
                    {isDownloading
                      ? <ActivityIndicator color="#000" size="small" />
                      : <Text style={[s.actionTxt, { color: '#000' }]}>Herunterladen</Text>}
                  </TouchableOpacity>
                )}
                {isLocal && (
                  <TouchableOpacity style={[s.actionBtn, s.delBtn]} onPress={() => confirmDelete(id)}>
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
  cardTop:      { flexDirection:'row', justifyContent:'space-between', alignItems:'center', marginBottom:10 },
  track:        { color: C.text, fontSize:17, fontWeight:'700', flex:1 },
  savedBadge:   { color: C.accent, fontSize:12, fontWeight:'600' },
  row:          { flexDirection:'row', marginBottom:12 },
  stat:         { flex:1 },
  statVal:      { color: C.text, fontSize:15, fontWeight:'700' },
  statLbl:      { color: C.dim, fontSize:11, marginTop:2 },
  actions:      { flexDirection:'row', gap:8 },
  actionBtn:    { flex:1, borderRadius:6, borderWidth:1, borderColor:'#333', padding:8, alignItems:'center' },
  dlBtn:        { backgroundColor: C.accent, borderColor: C.accent },
  delBtn:       { borderColor: C.danger },
  actionTxt:    { color: C.text, fontWeight:'600', fontSize:13 },
});
