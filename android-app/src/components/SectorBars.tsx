/**
 * SectorBars — horizontal bar per lap with sectors colored vs reference.
 * Width of each sector = its time share in the lap.
 * Color: cyan = faster than ref, orange = slower, gray = ≈ equal, accent = reference.
 */

import React from 'react';
import { View, Text, StyleSheet } from 'react-native';
import { C } from '../theme';
import { Lap } from '../types';
import { fmtLapTime } from '../analysis';

interface Props {
  laps: Lap[];                 // the laps to display (in order)
  lapIndices: number[];        // original indices in session.laps (for label "R5")
  refLap: Lap;                 // reference lap for coloring
  bestLapIndex: number;        // session's best lap index for star
  width: number;               // available pixel width
}

export default function SectorBars({ laps, lapIndices, refLap, bestLapIndex, width }: Props) {
  const refSecs = refLap.sectors ?? [];
  const refTotal = refLap.total_ms;

  return (
    <View style={[s.root, { width }]}>
      <View style={s.header}>
        <Text style={s.title}>Sektoren-Vergleich</Text>
        <Text style={s.subtitle}>vs. Referenz</Text>
      </View>

      {laps.map((lap, row) => {
        const origIdx = lapIndices[row];
        const isBest = origIdx === bestLapIndex;
        const isRef = lap === refLap;
        const secs = lap.sectors ?? [];
        // Use max(lap total, ref total) to keep bar proportions fair.
        const barTotal = Math.max(lap.total_ms, refTotal, 1);

        return (
          <View key={origIdx} style={s.row}>
            <View style={s.lapLabel}>
              <Text style={[s.lapNum, isRef && { color: C.accent }]}>
                R{lap.lap}{isBest ? '★' : ''}{isRef ? ' ▸' : ''}
              </Text>
              <Text style={s.lapTot}>{fmtLapTime(lap.total_ms)}</Text>
            </View>

            <View style={[s.barOuter, { width: width - 110 }]}>
              {secs.map((secMs, j) => {
                if (secMs <= 0) return null;
                const refMs = refSecs[j] ?? 0;
                const widthPct = (secMs / barTotal) * 100;

                // Delta color vs reference sector
                let color = C.surface2;
                let deltaStr = '';
                if (!isRef && refMs > 0) {
                  const dd = secMs - refMs;
                  if (Math.abs(dd) < 30) {
                    color = C.surface2;         // ≈ equal
                  } else if (dd < 0) {
                    color = C.faster;            // faster (cyan)
                    deltaStr = `${(dd / 1000).toFixed(2)}`;
                  } else {
                    color = C.warn;              // slower (orange)
                    deltaStr = `+${(dd / 1000).toFixed(2)}`;
                  }
                } else if (isRef) {
                  color = C.accent;
                }

                return (
                  <View
                    key={j}
                    style={[
                      s.seg,
                      {
                        width: `${widthPct}%`,
                        backgroundColor: color,
                        borderRightWidth: j < secs.length - 1 ? 1 : 0,
                      },
                    ]}
                  >
                    <Text style={s.segLbl} numberOfLines={1}>
                      S{j + 1}
                    </Text>
                    {deltaStr !== '' && (
                      <Text style={s.segDelta} numberOfLines={1}>
                        {deltaStr}
                      </Text>
                    )}
                  </View>
                );
              })}
            </View>
          </View>
        );
      })}

      {/* Legend */}
      <View style={s.legend}>
        <View style={[s.legDot, { backgroundColor: C.faster }]} />
        <Text style={s.legTxt}>schneller</Text>
        <View style={[s.legDot, { backgroundColor: C.warn, marginLeft: 10 }]} />
        <Text style={s.legTxt}>langsamer</Text>
        <View style={[s.legDot, { backgroundColor: C.accent, marginLeft: 10 }]} />
        <Text style={s.legTxt}>Referenz</Text>
      </View>
    </View>
  );
}

const s = StyleSheet.create({
  root:        { backgroundColor: C.surface, borderRadius: 8, padding: 10, marginTop: 10 },
  header:      { flexDirection: 'row', justifyContent: 'space-between',
                 alignItems: 'baseline', marginBottom: 10 },
  title:       { color: C.text, fontSize: 13, fontWeight: '700' },
  subtitle:    { color: C.dim, fontSize: 11 },
  row:         { flexDirection: 'row', alignItems: 'center', marginBottom: 6 },
  lapLabel:    { width: 100, paddingRight: 6 },
  lapNum:      { color: C.text, fontSize: 12, fontWeight: '700' },
  lapTot:      { color: C.dim, fontSize: 10, marginTop: 1 },
  barOuter:    { height: 24, flexDirection: 'row', borderRadius: 4, overflow: 'hidden',
                 backgroundColor: C.surface2 },
  seg:         { height: '100%', justifyContent: 'center', paddingHorizontal: 4,
                 borderRightColor: C.bg },
  segLbl:      { color: C.text, fontSize: 9, fontWeight: '700' },
  segDelta:    { color: C.text, fontSize: 9, fontWeight: '700' },
  legend:      { flexDirection: 'row', alignItems: 'center', marginTop: 8,
                 paddingLeft: 100 },
  legDot:      { width: 10, height: 10, borderRadius: 2 },
  legTxt:      { color: C.dim, fontSize: 10, marginLeft: 4 },
});
