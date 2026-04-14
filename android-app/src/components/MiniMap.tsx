/**
 * MiniMap — compact SVG track map with a car marker that follows a scrub cursor.
 *
 * Renders the racing line of each selected lap, plus a dot at the current
 * cursor distance (looked up per lap). Auto-fits to the bounding box of all
 * visible points with a small padding.
 */

import React, { useMemo } from 'react';
import { View, StyleSheet, Text } from 'react-native';
import Svg, { Path, Circle, Rect } from 'react-native-svg';
import { C } from '../theme';
import { LapChannels } from '../analysis';

export interface LapTrace {
  lapIdx: number;
  color: string;
  channels: LapChannels;      // precomputed channel data (has dist_m + t_ms)
  points: [number, number][]; // [lat, lon] per sample (aligned with channels)
}

interface Props {
  width: number;
  height: number;
  lapTraces: LapTrace[];
  /** Cursor distance in meters along primary lap. Shows car marker at that pos. */
  cursorDist?: number;
  /** Which lap to treat as "primary" (cursor uses its distance mapping). */
  primaryLapIdx?: number;
}

export default function MiniMap({ width, height, lapTraces, cursorDist, primaryLapIdx }: Props) {
  // Compute bounds from all points — skip non-finite lat/lon
  const bounds = useMemo(() => {
    let minLat =  Infinity, maxLat = -Infinity;
    let minLon =  Infinity, maxLon = -Infinity;
    let count = 0;
    for (const t of lapTraces) {
      for (const [lat, lon] of t.points) {
        if (!isFinite(lat) || !isFinite(lon)) continue;
        if (lat === 0 && lon === 0) continue;  // GPS-fix-placeholder
        if (lat < minLat) minLat = lat;
        if (lat > maxLat) maxLat = lat;
        if (lon < minLon) minLon = lon;
        if (lon > maxLon) maxLon = lon;
        count++;
      }
    }
    if (count < 2 || !isFinite(minLat)) return null;
    const latPad = (maxLat - minLat) * 0.05 || 0.0001;
    const lonPad = (maxLon - minLon) * 0.05 || 0.0001;
    return {
      minLat: minLat - latPad,
      maxLat: maxLat + latPad,
      minLon: minLon - lonPad,
      maxLon: maxLon + lonPad,
    };
  }, [lapTraces]);

  if (!bounds || lapTraces.length === 0) {
    return (
      <View style={[s.root, { width, height }]}>
        <Text style={s.placeholder}>Keine GPS-Punkte</Text>
      </View>
    );
  }

  // Map projection (flat): lon→x, lat→y with aspect correction
  const latRange = bounds.maxLat - bounds.minLat;
  const lonRange = bounds.maxLon - bounds.minLon;
  const aspectLon = Math.cos(((bounds.minLat + bounds.maxLat) / 2) * Math.PI / 180);
  const effLonRange = lonRange * aspectLon;
  // Fit into available px
  const padPx = 8;
  const avail = { w: width - 2 * padPx, h: height - 2 * padPx };
  const scale = Math.min(avail.w / (effLonRange || 1), avail.h / (latRange || 1));
  const mapW = effLonRange * scale;
  const mapH = latRange * scale;
  const offX = padPx + (avail.w - mapW) / 2;
  const offY = padPx + (avail.h - mapH) / 2;

  const lonToX = (lon: number) => offX + ((lon - bounds.minLon) * aspectLon) * scale;
  const latToY = (lat: number) => offY + (bounds.maxLat - lat) * scale;  // flipped (north up)

  // Build a Path per trace — skip bad points so SVG stays valid
  const paths = lapTraces.map(t => {
    let d = '';
    let started = false;
    for (const [lat, lon] of t.points) {
      if (!isFinite(lat) || !isFinite(lon)) { started = false; continue; }
      if (lat === 0 && lon === 0)          { started = false; continue; }
      const x = lonToX(lon), y = latToY(lat);
      if (!isFinite(x) || !isFinite(y))    { started = false; continue; }
      d += started ? ` L${x} ${y}` : `M${x} ${y}`;
      started = true;
    }
    return d;
  });

  // Car marker position — find the point in primary lap at cursor distance
  let markerX: number | null = null;
  let markerY: number | null = null;
  if (cursorDist != null && lapTraces.length > 0) {
    const primary = lapTraces.find(t => t.lapIdx === primaryLapIdx) ?? lapTraces[0];
    const { dist_m } = primary.channels;
    if (dist_m.length > 0) {
      // Find index where dist_m crosses cursorDist
      let j = 0;
      while (j < dist_m.length - 1 && dist_m[j + 1] < cursorDist) j++;
      const p = primary.points[j];
      if (p) {
        markerX = lonToX(p[1]);
        markerY = latToY(p[0]);
      }
    }
  }

  return (
    <View style={[s.root, { width, height }]}>
      <Svg width={width} height={height}>
        <Rect x={0} y={0} width={width} height={height} fill={C.surface} rx={8} />
        {paths.map((d, i) => (
          <Path
            key={i}
            d={d}
            stroke={lapTraces[i].color}
            strokeWidth={lapTraces[i].lapIdx === primaryLapIdx ? 2.5 : 1.5}
            fill="none"
            strokeLinejoin="round"
          />
        ))}
        {markerX != null && markerY != null && (
          <>
            <Circle cx={markerX} cy={markerY} r={8} fill={C.accent} fillOpacity={0.25} />
            <Circle cx={markerX} cy={markerY} r={4} fill={C.accent} stroke="#fff" strokeWidth={1.5} />
          </>
        )}
      </Svg>
    </View>
  );
}

const s = StyleSheet.create({
  root:        { borderRadius: 8, overflow: 'hidden', backgroundColor: C.surface },
  placeholder: { color: C.dim, textAlign: 'center', marginTop: 20, fontSize: 12 },
});
