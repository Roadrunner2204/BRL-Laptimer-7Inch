/**
 * ChartsScreen — professional multi-channel telemetry analysis.
 *
 * Top:    Lap chips (primary + overlays, tap to toggle)
 * Middle: Channel chips (Speed, G-Lat, G-Long, Delta, Sectors)
 * Main:   Stacked charts for the enabled channels, shared X-axis (distance),
 *         draggable vertical cursor syncs all charts.
 */

import React, { useMemo, useState, useEffect } from 'react';
import {
  View, Text, StyleSheet, ScrollView, TouchableOpacity, ActivityIndicator,
  useWindowDimensions, Dimensions,
} from 'react-native';
import { RouteProp } from '@react-navigation/native';
import { RootStackParamList } from '../App';
import { loadSession } from '../storage';
import { Session, Lap } from '../types';
import { C } from '../theme';
import { deriveChannels, deltaToReference, LapChannels,
         fmtLapTime, fmtDelta } from '../analysis';
import LineChart, { Trace } from '../components/LineChart';
import SectorBars from '../components/SectorBars';
import LeafletMap, { LeafletLap } from '../components/LeafletMap';
import ErrorBoundary from '../components/ErrorBoundary';

type Props = { route: RouteProp<RootStackParamList, 'Charts'> };

// Color palette for lap overlays — best = accent (blue), others from palette.
const LAP_COLORS = ['#0096FF', '#00C8FF', '#FF9500', '#FF3B30', '#BB88FF', '#44E0A8', '#FFEE55', '#FF6688'];

interface ChannelDef {
  key: 'speed' | 'gLong' | 'gLat' | 'delta';
  label: string;
  unit: string;
  getY: (ch: LapChannels, delta?: number[]) => number[];
  gridY?: number[];
  baselineY?: number;
  fmtY?: (v: number) => string;
}

const CHANNELS: ChannelDef[] = [
  {
    key: 'speed',
    label: 'Geschwindigkeit',
    unit: 'km/h',
    getY: (ch) => ch.speed_kmh,
    gridY: [0, 50, 100, 150, 200, 250, 300],
    fmtY: v => v.toFixed(0),
  },
  {
    key: 'gLat',
    label: 'Querbeschleunigung',
    unit: 'g',
    getY: (ch) => ch.g_lat,
    gridY: [-2, -1, 0, 1, 2],
    baselineY: 0,
    fmtY: v => v.toFixed(1),
  },
  {
    key: 'gLong',
    label: 'Längsbeschleunigung',
    unit: 'g',
    getY: (ch) => ch.g_long,
    gridY: [-2, -1, 0, 1, 2],
    baselineY: 0,
    fmtY: v => v.toFixed(1),
  },
  {
    key: 'delta',
    label: 'Delta zur Referenz',
    unit: 's',
    getY: (_ch, delta) => (delta ?? []).map(d => d / 1000),
    gridY: [-3, -1.5, 0, 1.5, 3],
    baselineY: 0,
    fmtY: v => (v >= 0 ? '+' : '') + v.toFixed(2),
  },
];

export default function ChartsScreenWrapper(props: Props) {
  return (
    <ErrorBoundary>
      <ChartsScreen {...props} />
    </ErrorBoundary>
  );
}

function ChartsScreen({ route }: Props) {
  const { width: winW } = useWindowDimensions();
  const chartW = winW - 16;   // full width minus padding
  const chartH = 150;

  const [session, setSession] = useState<Session | null>(null);
  const [refLap, setRefLap] = useState<number | null>(null);  // reference (usually best)
  const [selectedLaps, setSelectedLaps] = useState<Set<number>>(new Set());
  const [activeChannels, setActiveChannels] = useState<Set<ChannelDef['key']>>(
    new Set(['speed', 'gLat', 'gLong', 'delta'])
  );
  const [cursorX, setCursorX] = useState<number | null>(null);
  // Timeline zoom window (null = full range). Shared across all charts.
  const [xWindow, setXWindow] = useState<[number, number] | null>(null);

  useEffect(() => {
    loadSession(route.params.sessionId).then(s => {
      if (!s) return;
      setSession(s);
      setRefLap(s.best_lap_idx);
      setSelectedLaps(new Set([s.best_lap_idx]));
    });
  }, [route.params.sessionId]);

  // Precompute channels per selected lap — memoized
  const channelsByLap = useMemo(() => {
    if (!session) return new Map<number, LapChannels>();
    const m = new Map<number, LapChannels>();
    selectedLaps.forEach(idx => {
      const lap = session.laps[idx];
      if (lap) m.set(idx, deriveChannels(lap));
    });
    // Always need reference for delta computation
    if (refLap != null && !m.has(refLap)) {
      const lap = session.laps[refLap];
      if (lap) m.set(refLap, deriveChannels(lap));
    }
    return m;
  }, [session, selectedLaps, refLap]);

  const deltasByLap = useMemo(() => {
    const m = new Map<number, number[]>();
    if (refLap == null) return m;
    const ref = channelsByLap.get(refLap);
    if (!ref) return m;
    selectedLaps.forEach(idx => {
      const ch = channelsByLap.get(idx);
      if (!ch) return;
      if (idx === refLap) {
        m.set(idx, ch.t_ms.map(() => 0));
      } else {
        m.set(idx, deltaToReference(ch, ref));
      }
    });
    return m;
  }, [channelsByLap, refLap, selectedLaps]);

  // Shared X-domain = distance range of the longest selected lap
  const xDomainFull: [number, number] = useMemo(() => {
    let max = 0;
    channelsByLap.forEach(ch => {
      const d = ch.dist_m[ch.dist_m.length - 1];
      if (d > max) max = d;
    });
    return [0, Math.max(max, 1)];
  }, [channelsByLap]);
  const xDomain: [number, number] = xWindow ?? xDomainFull;

  // Reset zoom window if full range changes (new laps selected)
  useEffect(() => { setXWindow(null); }, [xDomainFull[1]]);

  // Zoom helpers — center around cursor if present, else midpoint.
  function zoomIn() {
    const [lo, hi] = xDomain;
    const span = hi - lo;
    const newSpan = span / 1.6;
    if (newSpan < (xDomainFull[1] - xDomainFull[0]) / 200) return;  // max 200×
    const c = cursorX ?? (lo + hi) / 2;
    let nl = c - newSpan / 2, nh = c + newSpan / 2;
    if (nl < xDomainFull[0]) { nh += xDomainFull[0] - nl; nl = xDomainFull[0]; }
    if (nh > xDomainFull[1]) { nl -= nh - xDomainFull[1]; nh = xDomainFull[1]; }
    setXWindow([Math.max(xDomainFull[0], nl), Math.min(xDomainFull[1], nh)]);
  }
  function zoomOut() {
    const [lo, hi] = xDomain;
    const span = hi - lo;
    const newSpan = Math.min(xDomainFull[1] - xDomainFull[0], span * 1.6);
    const c = cursorX ?? (lo + hi) / 2;
    let nl = c - newSpan / 2, nh = c + newSpan / 2;
    if (nl < xDomainFull[0]) { nh += xDomainFull[0] - nl; nl = xDomainFull[0]; }
    if (nh > xDomainFull[1]) { nl -= nh - xDomainFull[1]; nh = xDomainFull[1]; }
    if (nh - nl >= (xDomainFull[1] - xDomainFull[0]) - 1) setXWindow(null);
    else setXWindow([nl, nh]);
  }
  function panBy(frac: number) {
    if (!xWindow) return;
    const [lo, hi] = xWindow;
    const span = hi - lo;
    const delta = span * frac;
    let nl = lo + delta, nh = hi + delta;
    if (nl < xDomainFull[0]) { nh += xDomainFull[0] - nl; nl = xDomainFull[0]; }
    if (nh > xDomainFull[1]) { nl -= nh - xDomainFull[1]; nh = xDomainFull[1]; }
    setXWindow([nl, nh]);
  }

  // Build LeafletLap array — points + cumDist + color per selected lap.
  // Hook must run EVERY render (before the early `if (!session)` return).
  const mapLaps: LeafletLap[] = useMemo(() => {
    if (!session) return [];
    const out: LeafletLap[] = [];
    let colorIdx = 0;
    selectedLaps.forEach(idx => {
      const lap = session.laps[idx];
      const ch = channelsByLap.get(idx);
      if (!lap || !ch || !lap.track_points) return;
      const color = idx === session.best_lap_idx
        ? C.accent
        : LAP_COLORS[(colorIdx++ + 1) % LAP_COLORS.length];
      out.push({
        lapIdx: idx,
        color,
        points: lap.track_points.map(p => [p[0], p[1]] as [number, number]),
        cumDist: ch.dist_m,
        label: `R${lap.lap}`,
      });
    });
    return out;
  }, [session, selectedLaps, channelsByLap]);

  if (!session) {
    return (
      <View style={s.loading}>
        <ActivityIndicator color={C.accent} size="large" />
      </View>
    );
  }

  // Build trace arrays per active channel (plain function — not a hook)
  function tracesFor(ch: ChannelDef): Trace[] {
    const arr: Trace[] = [];
    let colorIdx = 0;
    selectedLaps.forEach(idx => {
      const lapCh = channelsByLap.get(idx);
      if (!lapCh) return;
      const delta = deltasByLap.get(idx);
      const y = ch.getY(lapCh, delta);
      const color = idx === session!.best_lap_idx
        ? C.accent
        : LAP_COLORS[(colorIdx++ + 1) % LAP_COLORS.length];
      arr.push({
        x: lapCh.dist_m,
        y,
        color,
        width: idx === refLap ? 2 : 1.3,
      });
    });
    return arr;
  }

  function toggleLap(idx: number) {
    const next = new Set(selectedLaps);
    if (next.has(idx)) {
      if (next.size === 1) return;  // always keep one selected
      next.delete(idx);
    } else {
      if (next.size >= 4) {
        // Cap at 4 to keep things readable
        return;
      }
      next.add(idx);
    }
    setSelectedLaps(next);
  }

  function toggleChannel(k: ChannelDef['key']) {
    const next = new Set(activeChannels);
    if (next.has(k)) {
      if (next.size === 1) return;
      next.delete(k);
    } else {
      next.add(k);
    }
    setActiveChannels(next);
  }

  const zoomed = xWindow != null;
  const zoomPct = zoomed
    ? Math.round((xDomainFull[1] - xDomainFull[0]) / (xDomain[1] - xDomain[0]))
    : 1;

  return (
    <View style={s.root}>
      {/* Map — OUTSIDE scroll view so pinch/zoom gestures work natively */}
      <LeafletMap
        laps={mapLaps}
        primaryLapIdx={refLap ?? undefined}
        cursorDist={cursorX ?? undefined}
        height={260}
      />

      <ScrollView contentContainerStyle={{ padding: 8 }}>
      {/* Header */}
      <View style={s.header}>
        <Text style={s.sessionName}>{session.name}</Text>
        <Text style={s.sessionSub}>{session.track} · Referenz: Runde {refLap != null ? refLap + 1 : '—'}</Text>
      </View>

      {/* Lap chips */}
      <Text style={s.sectionLabel}>Runden (max 4)</Text>
      <ScrollView horizontal showsHorizontalScrollIndicator={false} style={s.chipScroll}>
        {session.laps.map((lap, i) => {
          const active = selectedLaps.has(i);
          const isBest = i === session.best_lap_idx;
          const isRef  = i === refLap;
          return (
            <TouchableOpacity
              key={i}
              style={[s.lapChip, active && s.lapChipActive, !active && { opacity: 0.55 }]}
              onPress={() => toggleLap(i)}
              onLongPress={() => setRefLap(i)}
            >
              <Text style={[s.lapChipTxt, active && s.lapChipTxtActive]}>
                R{lap.lap}{isBest ? ' ★' : ''}{isRef ? ' ▸' : ''}
              </Text>
              <Text style={[s.lapChipTime, active && { color: C.text }]}>
                {fmtLapTime(lap.total_ms)}
              </Text>
            </TouchableOpacity>
          );
        })}
      </ScrollView>
      <Text style={s.hint}>Tippen = an/aus, Halten = als Referenz</Text>

      {/* Channel chips */}
      <Text style={s.sectionLabel}>Kanäle</Text>
      <View style={s.chRow}>
        {CHANNELS.map(ch => {
          const active = activeChannels.has(ch.key);
          return (
            <TouchableOpacity
              key={ch.key}
              style={[s.chChip, active && s.chChipActive]}
              onPress={() => toggleChannel(ch.key)}
            >
              <Text style={[s.chChipTxt, active && { color: '#000', fontWeight: '700' }]}>
                {ch.label}
              </Text>
            </TouchableOpacity>
          );
        })}
      </View>

      {/* Timeline zoom controls */}
      <View style={s.zoomBar}>
        <TouchableOpacity style={s.zoomBtn} onPress={() => panBy(-0.3)} disabled={!zoomed}>
          <Text style={[s.zoomBtnTxt, !zoomed && { opacity: 0.3 }]}>◀</Text>
        </TouchableOpacity>
        <TouchableOpacity style={s.zoomBtn} onPress={zoomOut}>
          <Text style={s.zoomBtnTxt}>−</Text>
        </TouchableOpacity>
        <Text style={s.zoomLbl}>{zoomed ? `${zoomPct}×` : 'Zoom'}</Text>
        <TouchableOpacity style={s.zoomBtn} onPress={zoomIn}>
          <Text style={s.zoomBtnTxt}>+</Text>
        </TouchableOpacity>
        <TouchableOpacity style={s.zoomBtn} onPress={() => panBy(0.3)} disabled={!zoomed}>
          <Text style={[s.zoomBtnTxt, !zoomed && { opacity: 0.3 }]}>▶</Text>
        </TouchableOpacity>
        <TouchableOpacity style={[s.zoomBtn, { flex: 1 }]} onPress={() => setXWindow(null)} disabled={!zoomed}>
          <Text style={[s.zoomBtnTxt, { fontSize: 11 }, !zoomed && { opacity: 0.3 }]}>Reset</Text>
        </TouchableOpacity>
      </View>

      {/* Charts */}
      <View style={{ marginTop: 6 }}>
        {CHANNELS.filter(ch => activeChannels.has(ch.key)).map(ch => (
          <LineChart
            key={ch.key}
            width={chartW}
            height={chartH}
            xDomain={xDomain}
            traces={tracesFor(ch)}
            gridY={ch.gridY}
            baselineY={ch.baselineY}
            label={ch.label}
            unit={ch.unit}
            cursorX={cursorX ?? undefined}
            onScrub={setCursorX}
            fmtY={ch.fmtY}
          />
        ))}
      </View>

      {/* Sector comparison bars */}
      {refLap != null && session.laps[refLap] && (
        <SectorBars
          width={chartW}
          laps={[...selectedLaps].map(i => session.laps[i])}
          lapIndices={[...selectedLaps]}
          refLap={session.laps[refLap]}
          bestLapIndex={session.best_lap_idx}
        />
      )}

      {/* Delta summary */}
      {refLap != null && selectedLaps.size > 1 && (
        <View style={s.deltaSummary}>
          <Text style={s.deltaSummaryTitle}>Delta zu R{refLap + 1}</Text>
          {[...selectedLaps].filter(i => i !== refLap).map(i => {
            const lapDelta = deltasByLap.get(i);
            const final = lapDelta?.[lapDelta.length - 1] ?? 0;
            const color = final < 0 ? C.faster : final > 0 ? C.warn : C.text;
            return (
              <View key={i} style={s.deltaRow}>
                <Text style={s.deltaLapLbl}>R{session.laps[i].lap}</Text>
                <Text style={[s.deltaVal, { color }]}>{fmtDelta(final)}</Text>
              </View>
            );
          })}
        </View>
      )}

      {/* Cursor readout */}
      {cursorX != null && (
        <View style={s.cursorInfo}>
          <Text style={s.cursorInfoTxt}>Position: {cursorX.toFixed(0)} m</Text>
          <TouchableOpacity onPress={() => setCursorX(null)}>
            <Text style={s.cursorClear}>×</Text>
          </TouchableOpacity>
        </View>
      )}
      </ScrollView>
    </View>
  );
}

const s = StyleSheet.create({
  root:           { flex: 1, backgroundColor: C.bg },
  loading:        { flex: 1, backgroundColor: C.bg, justifyContent: 'center', alignItems: 'center' },
  header:         { marginBottom: 8, paddingHorizontal: 4 },
  sessionName:    { color: C.text, fontSize: 18, fontWeight: '700' },
  sessionSub:     { color: C.dim, fontSize: 12, marginTop: 2 },
  sectionLabel:   { color: C.dim, fontSize: 11, fontWeight: '700',
                    textTransform: 'uppercase', marginTop: 10, marginBottom: 6,
                    paddingHorizontal: 4 },
  chipScroll:     { paddingHorizontal: 4 },
  lapChip:        { backgroundColor: C.surface2, paddingHorizontal: 10, paddingVertical: 6,
                    borderRadius: 8, marginRight: 6, alignItems: 'center', minWidth: 64,
                    borderWidth: 1, borderColor: 'transparent' },
  lapChipActive:  { borderColor: C.accent, backgroundColor: C.highlight },
  lapChipTxt:     { color: C.dim, fontSize: 12, fontWeight: '700' },
  lapChipTxtActive: { color: C.text },
  lapChipTime:    { color: C.dim, fontSize: 10, marginTop: 2 },
  hint:           { color: C.textDark, fontSize: 10, marginTop: 4, paddingHorizontal: 4 },

  chRow:          { flexDirection: 'row', flexWrap: 'wrap', paddingHorizontal: 4, gap: 6 },
  chChip:         { backgroundColor: C.surface2, paddingHorizontal: 12, paddingVertical: 6,
                    borderRadius: 14 },
  chChipActive:   { backgroundColor: C.accent },
  chChipTxt:      { color: C.dim, fontSize: 12, fontWeight: '600' },

  deltaSummary:   { marginTop: 14, backgroundColor: C.surface, borderRadius: 8,
                    padding: 12 },
  deltaSummaryTitle: { color: C.dim, fontSize: 11, fontWeight: '700',
                       textTransform: 'uppercase', marginBottom: 8 },
  deltaRow:       { flexDirection: 'row', justifyContent: 'space-between',
                    paddingVertical: 4 },
  deltaLapLbl:    { color: C.text, fontSize: 14, fontWeight: '600' },
  deltaVal:       { fontSize: 14, fontWeight: '700' },

  zoomBar:        { flexDirection: 'row', alignItems: 'center', gap: 6,
                    marginTop: 10, paddingHorizontal: 4 },
  zoomBtn:        { minWidth: 38, paddingVertical: 8, paddingHorizontal: 10,
                    backgroundColor: C.surface2, borderRadius: 8,
                    alignItems: 'center', justifyContent: 'center',
                    borderWidth: 1, borderColor: C.border },
  zoomBtnTxt:     { color: C.text, fontSize: 14, fontWeight: '700' },
  zoomLbl:        { color: C.dim, fontSize: 11, fontWeight: '700',
                    paddingHorizontal: 6, minWidth: 46, textAlign: 'center' },

  cursorInfo:     { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between',
                    backgroundColor: C.surface, padding: 10, borderRadius: 8,
                    marginTop: 10 },
  cursorInfoTxt:  { color: C.text, fontSize: 13, fontWeight: '600' },
  cursorClear:    { color: C.danger, fontSize: 20, paddingHorizontal: 8, fontWeight: '700' },
});
