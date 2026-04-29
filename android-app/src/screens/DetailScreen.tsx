import React, { useEffect, useState } from 'react';
import {
  View, Text, ScrollView, TouchableOpacity, StyleSheet, ActivityIndicator,
} from 'react-native';
import { NativeStackNavigationProp } from '@react-navigation/native-stack';
import { RouteProp } from '@react-navigation/native';
import { RootStackParamList } from '../App';
import { loadSession, saveSession } from '../storage';
import { Session, Lap } from '../types';
import { fmtTime, fmtDelta } from '../utils';
import { C } from '../theme';
import { isVideoCached, prefetchVideo } from '../videoCache';

type VideoStatus = 'checking' | 'cached' | 'downloading' | 'unavailable';

type Props = {
  navigation: NativeStackNavigationProp<RootStackParamList, 'Detail'>;
  route: RouteProp<RootStackParamList, 'Detail'>;
};

export default function DetailScreen({ navigation, route }: Props) {
  const [session, setSession] = useState<Session | null>(null);
  const [videoStatus, setVideoStatus] = useState<VideoStatus>('checking');
  const [dlProgress, setDlProgress] = useState(0);   // 0..1

  useEffect(() => {
    loadSession(route.params.sessionId).then(setSession);
  }, [route.params.sessionId]);

  // Auto-prefetch the primary video for this session as soon as the
  // user lands on the Detail screen — same behaviour as the Studio's
  // AnalyseView (kicks off a background download in set_laps). On mobile
  // we only pull the best-lap video; multi-lap prefetch would burn the
  // hotspot's quota for clips the user may never open.
  useEffect(() => {
    if (!session) return;
    const best = session.laps[session.best_lap_idx];
    const firstWithVideo = session.laps.find(l => l.video);
    const videoId = best?.video ?? firstWithVideo?.video ?? session.id;
    if (!videoId) {
      setVideoStatus('unavailable'); return;
    }
    let cancelled = false;
    (async () => {
      if (await isVideoCached(videoId)) {
        if (!cancelled) setVideoStatus('cached');
        return;
      }
      if (!cancelled) setVideoStatus('downloading');
      const result = await prefetchVideo(videoId, (p) => {
        if (cancelled) return;
        const pct = p.bytesTotal > 0 ? p.bytesWritten / p.bytesTotal : 0;
        setDlProgress(pct);
      });
      if (cancelled) return;
      setVideoStatus(result ? 'cached' : 'unavailable');
    })();
    return () => { cancelled = true; };
  }, [session]);

  if (!session) return (
    <View style={{ flex:1, backgroundColor: C.bg, justifyContent:'center', alignItems:'center' }}>
      <ActivityIndicator color={C.accent} size="large" />
    </View>
  );

  const best = session.laps[session.best_lap_idx];
  const validLaps = session.laps.filter(l => l.total_ms > 0);
  const avgMs = validLaps.length > 0
    ? validLaps.reduce((s, l) => s + l.total_ms, 0) / validLaps.length : 0;

  // Reference lap — defaults to the fastest lap, user can pick any lap
  // by tapping its row. Persisted on the session JSON so ChartsScreen
  // and any other consumer pick it up.
  const refIdx = session.ref_lap_idx ?? session.best_lap_idx;
  const ref    = session.laps[refIdx];

  async function setRefLap(idx: number) {
    if (!session) return;
    if (session.laps[idx]?.total_ms <= 0) return;   // skip invalid laps
    const next: Session = { ...session, ref_lap_idx: idx };
    setSession(next);
    try { await saveSession(next); } catch { /* keep in-memory anyway */ }
  }

  const sectorCount = best?.sectors?.length ?? 0;

  return (
    <View style={s.root}>
      {/* Header */}
      <View style={s.hdr}>
        <Text style={s.trackName}>{session.name}</Text>
        <Text style={s.sessionId}>{session.track} · {session.id}</Text>
      </View>

      {/* Primary action row — one-tap to each analysis surface */}
      <View style={s.actionsRow}>
        <TouchableOpacity
          style={s.actionPrimary}
          onPress={() => navigation.navigate('Charts', { sessionId: session.id })}
        >
          <Text style={s.actionPrimaryIcon}>📊</Text>
          <Text style={s.actionPrimaryTxt}>Analyse</Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={s.actionSecondary}
          onPress={() => navigation.navigate('Map', { sessionId: session.id })}
        >
          <Text style={s.actionSecondaryIcon}>🗺️</Text>
          <Text style={s.actionSecondaryTxt}>Karte</Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={s.actionSecondary}
          onPress={() => {
            // Firmware now splits video per lap: each lap JSON entry carries
            // its own `video` stem (file basename without .avi). Pick the
            // best lap's video; fall back to the first lap with a video;
            // legacy sessions (no per-lap video) fall back to session.id
            // which matches the old <sessionId>.avi naming.
            const best = session.laps[session.best_lap_idx];
            const firstWithVideo = session.laps.find(l => l.video);
            const videoId = best?.video ?? firstWithVideo?.video ?? session.id;
            navigation.navigate('Video', {
              videoId,
              sessionId: session.id,
              mode: 'stream',
            });
          }}
        >
          <Text style={s.actionSecondaryIcon}>🎥</Text>
          <Text style={s.actionSecondaryTxt}>Video</Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={s.actionSecondary}
          onPress={() => navigation.navigate('Compare', { sessionId: session.id })}
        >
          <Text style={s.actionSecondaryIcon}>⇄</Text>
          <Text style={s.actionSecondaryTxt}>Vergleich</Text>
        </TouchableOpacity>
      </View>

      {/* Stats */}
      <View style={s.statsRow}>
        <StatCard label="Bestzeit" value={fmtTime(best?.total_ms ?? 0)} accent />
        <StatCard label="Runden" value={String(session.laps.length)} />
        <StatCard label="Durchschnitt" value={fmtTime(avgMs)} />
      </View>

      {/* Auto-prefetch status badge — same behaviour as Studio's
          AnalyseView (background download on session open). */}
      <View style={s.videoBadge}>
        {videoStatus === 'checking' && (
          <Text style={s.videoBadgeTxtDim}>🎥  Prüfe Video-Cache…</Text>
        )}
        {videoStatus === 'downloading' && (
          <Text style={s.videoBadgeTxt}>
            🔄  Video lädt {dlProgress > 0
              ? `${Math.round(dlProgress * 100)} %`
              : '…'}
          </Text>
        )}
        {videoStatus === 'cached' && (
          <Text style={s.videoBadgeOk}>🎥  Video lokal verfügbar</Text>
        )}
        {videoStatus === 'unavailable' && (
          <Text style={s.videoBadgeTxtDim}>🎥  Kein Video für diese Session</Text>
        )}
      </View>

      {/* Ref-lap hint */}
      <Text style={s.refHint}>
        Referenz: Runde {ref?.lap ?? '—'} ({fmtTime(ref?.total_ms ?? 0)})
        {refIdx === session.best_lap_idx ? '  · Bestzeit' : ''}
        {'  ·  '}Tippe auf eine Runde um sie als Referenz zu setzen
      </Text>

      {/* Lap table */}
      <ScrollView contentContainerStyle={{ padding: 12 }}>
        {/* Table header */}
        <View style={s.tableHdr}>
          <Text style={[s.col0, s.hdrTxt]}>#</Text>
          <Text style={[s.col1, s.hdrTxt]}>Zeit</Text>
          <Text style={[s.col2, s.hdrTxt]}>Delta</Text>
          {sectorCount > 0 && Array.from({length: sectorCount}, (_,i) =>
            <Text key={i} style={[s.colS, s.hdrTxt]}>S{i+1}</Text>
          )}
        </View>

        {session.laps.map((lap, idx) => {
          const isBest = idx === session.best_lap_idx;
          const isRef  = idx === refIdx;
          const delta  = lap.total_ms - (ref?.total_ms ?? 0);
          return (
            <TouchableOpacity
              key={idx}
              style={[s.lapRow, isBest && s.lapBest, isRef && s.lapRef]}
              onPress={() => setRefLap(idx)}
              activeOpacity={0.7}
            >
              <Text style={[s.col0, s.lapTxt, isBest && s.lapBestTxt]}>{lap.lap}</Text>
              <Text style={[s.col1, s.lapTxt, isBest && { color: C.accent, fontWeight:'700' }]}>
                {fmtTime(lap.total_ms)}
              </Text>
              <Text style={[s.col2, s.lapTxt, isRef ? s.refTxt
                            : delta < 0 ? s.faster
                            : delta > 0 ? s.slower : s.lapTxt]}>
                {isRef ? '▸ REF' : fmtDelta(delta)}
              </Text>
              {lap.sectors?.map((sec, si) => (
                <Text key={si} style={[s.colS, s.lapTxt, s.secTxt]}>
                  {fmtTime(sec)}
                </Text>
              ))}
            </TouchableOpacity>
          );
        })}
      </ScrollView>
    </View>
  );
}

function StatCard({ label, value, accent }: { label:string; value:string; accent?:boolean }) {
  return (
    <View style={sc.card}>
      <Text style={[sc.val, accent && { color: C.accent }]}>{value}</Text>
      <Text style={sc.lbl}>{label}</Text>
    </View>
  );
}

const sc = StyleSheet.create({
  card: { flex:1, backgroundColor: C.surface, borderRadius:10, padding:12, alignItems:'center', margin:4 },
  val:  { color: C.text, fontSize:16, fontWeight:'700' },
  lbl:  { color: C.dim, fontSize:11, marginTop:3 },
});

const s = StyleSheet.create({
  root:       { flex:1, backgroundColor: C.bg },
  hdr:        { padding:16, paddingBottom:8 },
  trackName:  { color: C.text, fontSize:20, fontWeight:'700' },
  sessionId:  { color: C.dim, fontSize:12, marginTop:2 },

  actionsRow: { flexDirection:'row', paddingHorizontal:12, paddingBottom:12, gap:8 },
  actionPrimary:{ flex:1.4, backgroundColor: C.accent, borderRadius:10,
                  paddingVertical:14, alignItems:'center' },
  actionPrimaryIcon:{ fontSize:22 },
  actionPrimaryTxt:{ color:'#000', fontWeight:'800', fontSize:14, marginTop:2 },
  actionSecondary:{ flex:1, backgroundColor: C.surface2, borderRadius:10,
                    paddingVertical:14, alignItems:'center',
                    borderWidth:1, borderColor: C.border },
  actionSecondaryIcon:{ fontSize:22 },
  actionSecondaryTxt:{ color: C.text, fontWeight:'700', fontSize:13, marginTop:2 },

  // Retained legacy styles in case they're referenced below
  hdrBtn:     { borderRadius:8, paddingHorizontal:14, paddingVertical:10, marginLeft:6 },
  analyseBtn: { backgroundColor: C.surface2, borderWidth:1, borderColor: C.accent },
  analyseBtnTxt: { color: C.accent, fontWeight:'700', fontSize:14 },
  mapBtn:     { backgroundColor: C.accent },
  mapBtnTxt:  { color:'#000', fontWeight:'700', fontSize:14 },
  statsRow:   { flexDirection:'row', padding:8 },
  tableHdr:   { flexDirection:'row', paddingVertical:8, borderBottomWidth:1, borderColor:'#333', marginBottom:4 },
  hdrTxt:     { color: C.dim, fontSize:11, fontWeight:'600', textTransform:'uppercase' },
  lapRow:     { flexDirection:'row', paddingVertical:10, borderBottomWidth:1, borderColor:'#1a1a1a', alignItems:'center' },
  lapBest:    { backgroundColor: C.highlight, borderRadius:6 },
  lapRef:     { borderLeftWidth:3, borderLeftColor: C.accent, paddingLeft:4 },
  lapTxt:     { color: C.text, fontSize:14 },
  lapBestTxt: { color: C.text, fontWeight:'700' },
  faster:     { color: C.faster },
  slower:     { color: C.warn },
  refTxt:     { color: C.accent, fontWeight:'800' },
  secTxt:     { color: C.dim, fontSize:12 },
  refHint:    { color: C.dim, fontSize:11, paddingHorizontal:14, paddingBottom:8,
                paddingTop:2, fontStyle:'italic' },
  videoBadge: { paddingHorizontal:14, paddingVertical:6, marginHorizontal:12,
                marginBottom:8, borderRadius:8, backgroundColor: C.surface,
                borderWidth:1, borderColor: C.border, alignSelf:'flex-start' },
  videoBadgeTxt:    { color: C.accent, fontSize:12, fontWeight:'600' },
  videoBadgeTxtDim: { color: C.dim,    fontSize:12, fontWeight:'500' },
  videoBadgeOk:     { color: C.faster ?? '#00CC66', fontSize:12, fontWeight:'700' },
  col0:       { width:28, marginRight:4 },
  col1:       { width:88 },
  col2:       { width:72 },
  colS:       { flex:1 },
});
