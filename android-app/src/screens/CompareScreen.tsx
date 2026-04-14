/**
 * CompareScreen — side-by-side video comparison of two laps from the same
 * recording. Both videos play against the same <Video> source but each is
 * seeked to its lap's start. Shared play/pause/scrub synchronises them.
 *
 * Layout (portrait): stacked vertically, each ~50% screen height.
 * Each video has its own full overlay HUD.
 */

import React, { useEffect, useMemo, useRef, useState } from 'react';
import {
  View, Text, StyleSheet, ActivityIndicator, TouchableOpacity,
  useWindowDimensions, ScrollView,
} from 'react-native';
import { Video, ResizeMode, AVPlaybackStatus } from 'expo-av';
import { RouteProp, useFocusEffect } from '@react-navigation/native';
import { RootStackParamList } from '../App';
import { C } from '../theme';
import { videoUrl } from '../api';
import { loadSession } from '../storage';
import { Session, Lap } from '../types';
import { deriveChannels, deltaToReference, LapChannels, fmtLapTime } from '../analysis';
import VideoOverlay from '../components/VideoOverlay';
import { loadOverlayConfig, OverlayConfig, DEFAULT_OVERLAY_CONFIG } from '../overlayConfig';

type Props = { route: RouteProp<RootStackParamList, 'Compare'> };

function fmtMs(ms: number): string {
  const total_s = Math.max(0, Math.floor(ms / 1000));
  const m = Math.floor(total_s / 60);
  const s = total_s % 60;
  return `${m}:${String(s).padStart(2, '0')}`;
}

interface LapSlot {
  lapIdx: number;
  lapStartMs: number;  // cumulative ms in the video where this lap begins
  channels: LapChannels;
  delta: number[] | null;
}

export default function CompareScreen({ route }: Props) {
  const { width: winW, height: winH } = useWindowDimensions();
  const videoAw = winW;
  const videoAh = Math.min(Math.round(winW * 9 / 16), Math.round((winH - 180) / 2));
  const { sessionId } = route.params;

  const videoRefA = useRef<Video>(null);
  const videoRefB = useRef<Video>(null);
  const [session, setSession] = useState<Session | null>(null);
  const [status, setStatus] = useState<AVPlaybackStatus | null>(null);  // primary status (A)
  const [isPlaying, setIsPlaying] = useState(false);
  const [lapA, setLapA] = useState<number | null>(null);
  const [lapB, setLapB] = useState<number | null>(null);
  const [overlayCfg, setOverlayCfg] = useState<OverlayConfig>(DEFAULT_OVERLAY_CONFIG);

  // Load overlay cfg on focus
  useFocusEffect(
    React.useCallback(() => {
      loadOverlayConfig().then(setOverlayCfg);
    }, [])
  );

  useEffect(() => {
    loadSession(sessionId).then(s => {
      if (!s) return;
      setSession(s);
      // Default: compare best lap vs 2nd best
      const sorted = s.laps
        .map((l, i) => ({ i, ms: l.total_ms }))
        .filter(x => x.ms > 0)
        .sort((a, b) => a.ms - b.ms);
      if (sorted.length >= 1) setLapA(sorted[0].i);
      if (sorted.length >= 2) setLapB(sorted[1].i);
      else if (sorted.length >= 1) setLapB(sorted[0].i);
    });
  }, [sessionId]);

  // Build slot info — need lap start offset in the video. Without explicit
  // offset metadata we approximate by summing prior laps' total_ms.
  const slotA: LapSlot | null = useMemo(() => buildSlot(session, lapA), [session, lapA]);
  const slotB: LapSlot | null = useMemo(() => buildSlot(session, lapB), [session, lapB]);

  // Seek both videos to the right lap offset once they're loaded
  useEffect(() => {
    if (slotA && videoRefA.current) videoRefA.current.setPositionAsync(slotA.lapStartMs);
  }, [slotA]);
  useEffect(() => {
    if (slotB && videoRefB.current) videoRefB.current.setPositionAsync(slotB.lapStartMs);
  }, [slotB]);

  async function togglePlay() {
    if (!videoRefA.current || !videoRefB.current) return;
    if (isPlaying) {
      await Promise.all([videoRefA.current.pauseAsync(), videoRefB.current.pauseAsync()]);
      setIsPlaying(false);
    } else {
      await Promise.all([videoRefA.current.playAsync(),  videoRefB.current.playAsync()]);
      setIsPlaying(true);
    }
  }

  const source = useMemo(() => ({ uri: videoUrl(sessionId) }), [sessionId]);

  if (!session) {
    return (
      <View style={s.loading}>
        <ActivityIndicator color={C.accent} size="large" />
      </View>
    );
  }

  function renderVideo(
    ref: React.RefObject<Video>,
    slot: LapSlot | null,
    label: string,
    isPrimary: boolean,
  ) {
    if (!slot) {
      return (
        <View style={[s.videoPane, { height: videoAh }]}>
          <Text style={s.placeholder}>Keine Runde ausgewählt</Text>
        </View>
      );
    }
    // Video playback position relative to lap start
    const posMs = (isPrimary && status?.isLoaded ? status.positionMillis : slot.lapStartMs);
    const timeInLap = Math.max(0, posMs - slot.lapStartMs);

    return (
      <View style={[s.videoPane, { height: videoAh }]}>
        <Video
          ref={ref}
          source={source}
          style={{ width: videoAw, height: videoAh }}
          resizeMode={ResizeMode.CONTAIN}
          useNativeControls={false}
          shouldPlay={false}
          onPlaybackStatusUpdate={isPrimary ? setStatus : undefined}
        />
        <VideoOverlay
          width={videoAw}
          height={videoAh}
          timeMs={timeInLap}
          channels={slot.channels}
          delta={slot.delta ?? undefined}
          lapNumber={session!.laps[slot.lapIdx]?.lap}
          config={overlayCfg}
        />
        <View style={s.label}>
          <Text style={s.labelTxt}>{label}  ·  R{session!.laps[slot.lapIdx].lap}  ·  {fmtLapTime(session!.laps[slot.lapIdx].total_ms)}</Text>
        </View>
      </View>
    );
  }

  return (
    <ScrollView style={s.root}>
      {renderVideo(videoRefA, slotA, 'A', true)}
      {renderVideo(videoRefB, slotB, 'B', false)}

      {/* Controls */}
      <View style={s.controls}>
        <TouchableOpacity style={s.playBtn} onPress={togglePlay}>
          <Text style={s.playTxt}>{isPlaying ? '❚❚' : '▶'}</Text>
        </TouchableOpacity>
        <Text style={s.time}>
          {status?.isLoaded ? fmtMs(Math.max(0, (status.positionMillis - (slotA?.lapStartMs ?? 0)))) : '0:00'}
        </Text>
      </View>

      {/* Lap pickers */}
      <Text style={s.pickerLbl}>Runde A</Text>
      <LapPicker laps={session.laps} selected={lapA} onChange={setLapA} />
      <Text style={s.pickerLbl}>Runde B</Text>
      <LapPicker laps={session.laps} selected={lapB} onChange={setLapB} />
    </ScrollView>
  );
}

function LapPicker({ laps, selected, onChange }: {
  laps: Lap[]; selected: number | null; onChange: (i: number) => void;
}) {
  return (
    <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={{ paddingHorizontal: 10 }}>
      {laps.map((lap, i) => (
        <TouchableOpacity
          key={i}
          style={[ps.chip, selected === i && ps.chipActive]}
          onPress={() => onChange(i)}
        >
          <Text style={[ps.chipTxt, selected === i && { color: '#000' }]}>
            R{lap.lap}
          </Text>
          <Text style={[ps.chipTime, selected === i && { color: '#000' }]}>
            {fmtLapTime(lap.total_ms)}
          </Text>
        </TouchableOpacity>
      ))}
    </ScrollView>
  );
}

function buildSlot(session: Session | null, lapIdx: number | null): LapSlot | null {
  if (!session || lapIdx == null) return null;
  const lap = session.laps[lapIdx];
  if (!lap) return null;
  // Approximate lap start offset = sum of total_ms of earlier laps.
  // Assumes recording started at lap 1. If not, user sees wrong offset.
  let startMs = 0;
  for (let i = 0; i < lapIdx; i++) startMs += session.laps[i]?.total_ms ?? 0;

  const channels = deriveChannels(lap);
  // Delta vs best lap
  const best = session.laps[session.best_lap_idx];
  const delta = best && lapIdx !== session.best_lap_idx
    ? deltaToReference(channels, deriveChannels(best))
    : null;

  return { lapIdx, lapStartMs: startMs, channels, delta };
}

const s = StyleSheet.create({
  root:        { flex: 1, backgroundColor: '#000' },
  loading:     { flex: 1, backgroundColor: C.bg, justifyContent: 'center', alignItems: 'center' },
  videoPane:   { width: '100%', backgroundColor: '#000', position: 'relative' },
  placeholder: { color: C.dim, textAlign: 'center', marginTop: 40, fontSize: 13 },
  label:       { position: 'absolute', bottom: 8, left: 8,
                 backgroundColor: 'rgba(0,0,0,0.6)', paddingHorizontal: 8, paddingVertical: 4,
                 borderRadius: 4 },
  labelTxt:    { color: C.accent, fontSize: 11, fontWeight: '700' },

  controls:    { flexDirection: 'row', alignItems: 'center',
                 padding: 12, backgroundColor: C.surface },
  playBtn:     { width: 48, height: 48, borderRadius: 24, backgroundColor: C.accent,
                 justifyContent: 'center', alignItems: 'center', marginRight: 14 },
  playTxt:     { color: '#000', fontSize: 20, fontWeight: '700' },
  time:        { color: C.text, fontSize: 14, fontVariant: ['tabular-nums'] },

  pickerLbl:   { color: C.dim, fontSize: 11, fontWeight: '700',
                 textTransform: 'uppercase', paddingHorizontal: 12, paddingTop: 10,
                 paddingBottom: 6, backgroundColor: C.bg },
});
const ps = StyleSheet.create({
  chip:       { backgroundColor: C.surface2, paddingHorizontal: 12, paddingVertical: 8,
                borderRadius: 6, marginRight: 6, alignItems: 'center', minWidth: 64 },
  chipActive: { backgroundColor: C.accent },
  chipTxt:    { color: C.text, fontSize: 13, fontWeight: '700' },
  chipTime:   { color: C.dim, fontSize: 11, marginTop: 2 },
});
