/**
 * VideoOverlay — configurable telemetry HUD on top of a Video player.
 *
 * All HUD elements (speed, lap time, delta, G-meter) can be toggled and
 * repositioned via `OverlayConfig`. Reads telemetry at the given time via
 * linear interpolation so it never depends on video frame rate.
 */

import React, { useMemo } from 'react';
import Svg, { Rect, Circle, Line, Text as SvgText } from 'react-native-svg';
import { StyleSheet, View } from 'react-native';
import { LapChannels, fmtLapTime, fmtDelta } from '../analysis';
import { C } from '../theme';
import { OverlayConfig, Corner, DEFAULT_OVERLAY_CONFIG } from '../overlayConfig';

interface Props {
  width: number;
  height: number;
  timeMs: number;
  channels: LapChannels;
  delta?: number[];
  lapNumber?: number;
  trackName?: string;
  config?: OverlayConfig;
}

function sampleAt(ts: number[], vs: number[], t: number): number {
  if (ts.length === 0 || vs.length === 0) return 0;
  if (t <= ts[0]) return vs[0];
  if (t >= ts[ts.length - 1]) return vs[vs.length - 1];
  let j = 0;
  while (j < ts.length - 1 && ts[j + 1] < t) j++;
  const t0 = ts[j], t1 = ts[Math.min(j + 1, ts.length - 1)];
  const v0 = vs[j], v1 = vs[Math.min(j + 1, vs.length - 1)];
  if (t1 <= t0) return v1;
  const f = (t - t0) / (t1 - t0);
  return v0 + f * (v1 - v0);
}

/** Return anchor pixel coords for a corner. */
function cornerAnchor(corner: Corner, width: number, height: number, pad = 16) {
  switch (corner) {
    case 'tl': return { x: pad, y: pad, ax: 'start' as const, ay: 'top' as const };
    case 'tr': return { x: width - pad, y: pad, ax: 'end' as const, ay: 'top' as const };
    case 'bl': return { x: pad, y: height - pad, ax: 'start' as const, ay: 'bottom' as const };
    case 'br': return { x: width - pad, y: height - pad, ax: 'end' as const, ay: 'bottom' as const };
  }
}

export default function VideoOverlay({
  width, height, timeMs, channels, delta, lapNumber, trackName, config,
}: Props) {
  const cfg = config ?? DEFAULT_OVERLAY_CONFIG;
  const fs = cfg.fontScale;

  const { speed, gLat, gLong, dlt } = useMemo(() => ({
    speed: sampleAt(channels.t_ms, channels.speed_kmh, timeMs),
    gLat:  sampleAt(channels.t_ms, channels.g_lat,     timeMs),
    gLong: sampleAt(channels.t_ms, channels.g_long,    timeMs),
    dlt:   delta ? sampleAt(channels.t_ms, delta, timeMs) : null,
  }), [channels, delta, timeMs]);

  // G-meter layout
  const gR = Math.min(60, height * 0.15) * fs;
  const gAnchor = cornerAnchor(cfg.positionGMeter, width, height);
  const gCX = gAnchor.ax === 'start' ? gAnchor.x + gR : gAnchor.x - gR;
  const gCY = gAnchor.ay === 'top'   ? gAnchor.y + gR : gAnchor.y - gR;
  const gVecX = Math.max(-2, Math.min(2, gLat))  / 2 * gR;
  const gVecY = -Math.max(-2, Math.min(2, gLong)) / 2 * gR;

  // Speed
  const spdAnchor = cornerAnchor(cfg.positionSpeed, width, height);
  const spdY = spdAnchor.ay === 'top' ? spdAnchor.y + 36 * fs : spdAnchor.y - 18 * fs;

  // Lap time / delta column
  const ltAnchor = cornerAnchor(cfg.positionLapTime, width, height);

  // Backdrop rects — only where content is shown
  const showTop = cfg.showSpeed && (cfg.positionSpeed === 'tl' || cfg.positionSpeed === 'tr')
                || cfg.showLapTime && (cfg.positionLapTime === 'tl' || cfg.positionLapTime === 'tr')
                || (cfg.showTrackName && trackName);

  const deltaColor = dlt == null ? '#fff'
                    : dlt < 0 ? cfg.accentColor  // faster — use accent
                    : dlt > 0 ? '#FF9500'        // slower — orange
                    : '#fff';

  return (
    <View style={[styles.root, { width, height }]} pointerEvents="none">
      <Svg width={width} height={height}>
        {/* Top backdrop (covers top 72px if any top-corner element is enabled) */}
        {showTop && (
          <Rect x={0} y={0} width={width} height={72 * fs}
                fill="#000" fillOpacity={cfg.backdropAlpha / 255} />
        )}

        {/* Track name banner (center top) */}
        {cfg.showTrackName && trackName && (
          <SvgText x={width / 2} y={20 * fs} fill={cfg.accentColor}
                   fontSize={12 * fs} fontWeight="700" textAnchor="middle">
            {trackName}
          </SvgText>
        )}

        {/* Speed (big) */}
        {cfg.showSpeed && (
          <>
            <SvgText
              x={spdAnchor.x} y={spdY}
              fill="#fff" fontSize={40 * fs} fontWeight="900"
              textAnchor={spdAnchor.ax}
            >
              {speed.toFixed(0)}
            </SvgText>
            <SvgText
              x={spdAnchor.x} y={spdY + 16 * fs}
              fill="#aaa" fontSize={12 * fs} fontWeight="600"
              textAnchor={spdAnchor.ax}
            >
              km/h
            </SvgText>
          </>
        )}

        {/* Lap time */}
        {cfg.showLapTime && (
          <SvgText
            x={ltAnchor.x}
            y={ltAnchor.ay === 'top' ? ltAnchor.y + 22 * fs : ltAnchor.y - 30 * fs}
            fill="#fff" fontSize={14 * fs} fontWeight="700"
            textAnchor={ltAnchor.ax}
          >
            {lapNumber ? `R${lapNumber}` : ''} {fmtLapTime(timeMs)}
          </SvgText>
        )}

        {/* Delta */}
        {cfg.showDelta && dlt != null && (
          <SvgText
            x={ltAnchor.x}
            y={ltAnchor.ay === 'top' ? ltAnchor.y + 46 * fs : ltAnchor.y - 8 * fs}
            fill={deltaColor} fontSize={18 * fs} fontWeight="800"
            textAnchor={ltAnchor.ax}
          >
            {fmtDelta(dlt)}
          </SvgText>
        )}

        {/* G-meter */}
        {cfg.showGMeter && (
          <>
            <Rect
              x={gCX - gR - 6 * fs} y={gCY - gR - 6 * fs}
              width={gR * 2 + 12 * fs} height={gR * 2 + 12 * fs}
              rx={8} fill="#000" fillOpacity={cfg.backdropAlpha / 255}
            />
            <Circle cx={gCX} cy={gCY} r={gR * 0.5} fill="none" stroke="#555" strokeWidth={1} strokeDasharray="2 2" />
            <Circle cx={gCX} cy={gCY} r={gR} fill="none" stroke="#888" strokeWidth={1} />
            <Line x1={gCX - gR} y1={gCY} x2={gCX + gR} y2={gCY} stroke="#555" strokeWidth={0.7} />
            <Line x1={gCX} y1={gCY - gR} x2={gCX} y2={gCY + gR} stroke="#555" strokeWidth={0.7} />
            <Line
              x1={gCX} y1={gCY} x2={gCX + gVecX} y2={gCY + gVecY}
              stroke={cfg.accentColor} strokeWidth={3} strokeLinecap="round"
            />
            <Circle cx={gCX + gVecX} cy={gCY + gVecY} r={5 * fs}
                    fill={cfg.accentColor} stroke="#fff" strokeWidth={1.5} />
            <SvgText x={gCX} y={gCY + gR + 16 * fs} fill="#fff" fontSize={10 * fs}
                     textAnchor="middle" fontWeight="700">
              {Math.sqrt(gLat * gLat + gLong * gLong).toFixed(1)} g
            </SvgText>
          </>
        )}
      </Svg>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { position: 'absolute', top: 0, left: 0 },
});
