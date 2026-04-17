/**
 * VideoOverlay — configurable telemetry HUD on top of a Video player.
 *
 * v2: renders purely from `config.widgets` — each widget has its own
 * anchor point (x, y as fractions of video dims) and anchor alignment.
 * Any widget can sit anywhere on the frame. Older corner-based configs
 * are migrated to widgets on load (see overlayConfig.ts).
 *
 * Telemetry is sampled at the given time via linear interpolation so
 * the overlay never depends on video frame rate.
 */

import React, { useMemo } from 'react';
import Svg, { Rect, Circle, Line, Text as SvgText, Polyline } from 'react-native-svg';
import { StyleSheet, View } from 'react-native';
import { LapChannels, fmtLapTime, fmtDelta } from '../analysis';
import {
  OverlayConfig, DEFAULT_OVERLAY_CONFIG, WidgetAnchor, WidgetType,
} from '../overlayConfig';

interface Props {
  width: number;
  height: number;
  timeMs: number;
  channels: LapChannels;
  delta?: number[];
  lapNumber?: number;
  trackName?: string;
  config?: OverlayConfig;
  /** Optional [lat, lon, lap_ms] track points for the current lap — used
   *  by the MiniMap widget to draw the circuit outline + car dot. When
   *  omitted, MiniMap falls back to a placeholder circle. */
  trackPath?: Array<[number, number, number]>;
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

// Map our widget anchor to SVG textAnchor.
function svgTextAnchor(a: WidgetAnchor): 'start' | 'middle' | 'end' {
  return a === 'center' ? 'middle' : a;
}

// Nominal bounding box (width × height, at fontScale=1) per widget type.
// Used by the editor for drag handles AND here for per-widget backdrops.
// Keep in sync with the actual SVG drawing below.
export const WIDGET_SIZES: Record<WidgetType, { w: number; h: number }> = {
  speed:     { w: 112, h: 70  },
  lapTime:   { w: 160, h: 28  },
  delta:     { w: 130, h: 32  },
  gMeter:    { w: 140, h: 160 },
  trackName: { w: 200, h: 24  },
  speedBar:  { w: 220, h: 24  },
  gForce:    { w: 120, h: 62  },
  lapNumber: { w: 90,  h: 50  },
  miniMap:   { w: 170, h: 130 },
};

// Left edge of a widget's bounding box given its anchor point + width.
function leftOf(anchorPx: number, w: number, a: WidgetAnchor): number {
  return a === 'start' ? anchorPx
       : a === 'end'   ? anchorPx - w
       :                 anchorPx - w / 2;
}

export default function VideoOverlay({
  width, height, timeMs, channels, delta, lapNumber, trackName, config, trackPath,
}: Props) {
  const cfg = config ?? DEFAULT_OVERLAY_CONFIG;
  const fs = cfg.fontScale;
  const alpha = cfg.backdropAlpha / 255;

  const { speed, gLat, gLong, dlt } = useMemo(() => ({
    speed: sampleAt(channels.t_ms, channels.speed_kmh, timeMs),
    gLat:  sampleAt(channels.t_ms, channels.g_lat,     timeMs),
    gLong: sampleAt(channels.t_ms, channels.g_long,    timeMs),
    dlt:   delta ? sampleAt(channels.t_ms, delta, timeMs) : null,
  }), [channels, delta, timeMs]);

  const deltaColor = dlt == null ? '#fff'
                    : dlt < 0 ? cfg.accentColor
                    : dlt > 0 ? '#FF9500'
                    : '#fff';

  return (
    <View style={[styles.root, { width, height }]} pointerEvents="none">
      <Svg width={width} height={height}>
        {cfg.widgets.filter(w => w.visible).map(w => {
          const px = w.x * width;
          const py = w.y * height;
          // Effective font scale = global × per-widget. Clamped so an
          // accidental 0 doesn't make text invisible; ceiling prevents
          // runaway size from eating the whole frame.
          const wfs = fs * Math.max(0.3, Math.min(3, w.scale ?? 1.0));
          const key = w.id;
          switch (w.type) {
            case 'speed':
              return <SpeedWidget key={key} px={px} py={py} anchor={w.anchor}
                       fs={wfs} alpha={alpha} color={w.color} value={speed} />;
            case 'lapTime':
              return <LapTimeWidget key={key} px={px} py={py} anchor={w.anchor}
                       fs={wfs} alpha={alpha} color={w.color}
                       timeMs={timeMs} lapNumber={lapNumber} />;
            case 'delta':
              if (dlt == null) return null;
              // widget.color overrides the faster/slower auto-color so the
              // user can pick a fixed tone if they prefer.
              return <DeltaWidget key={key} px={px} py={py} anchor={w.anchor}
                       fs={wfs} alpha={alpha}
                       value={dlt} color={w.color ?? deltaColor} />;
            case 'gMeter':
              return <GMeterWidget key={key} px={px} py={py} anchor={w.anchor}
                       fs={wfs} alpha={alpha} accent={w.color ?? cfg.accentColor}
                       gLat={gLat} gLong={gLong} />;
            case 'trackName':
              if (!trackName) return null;
              return <TrackNameWidget key={key} px={px} py={py} anchor={w.anchor}
                       fs={wfs} alpha={alpha}
                       accent={w.color ?? cfg.accentColor} name={trackName} />;
            case 'speedBar':
              return <SpeedBarWidget key={key} px={px} py={py} anchor={w.anchor}
                       fs={wfs} alpha={alpha}
                       accent={w.color ?? cfg.accentColor} value={speed} />;
            case 'gForce':
              return <GForceWidget key={key} px={px} py={py} anchor={w.anchor}
                       fs={wfs} alpha={alpha} color={w.color}
                       gLat={gLat} gLong={gLong} />;
            case 'lapNumber':
              return <LapNumberWidget key={key} px={px} py={py} anchor={w.anchor}
                       fs={wfs} alpha={alpha}
                       color={w.color ?? cfg.accentColor} lapNumber={lapNumber} />;
            case 'miniMap':
              return <MiniMapWidget key={key} px={px} py={py} anchor={w.anchor}
                       fs={wfs} alpha={alpha}
                       accent={w.color ?? cfg.accentColor}
                       path={trackPath} timeMs={timeMs} />;
            default:
              return null;
          }
        })}
      </Svg>
    </View>
  );
}

// ── Per-widget renderers ────────────────────────────────────────────

interface WidgetRenderProps {
  px: number;            // anchor X in pixels
  py: number;            // anchor Y (top of box) in pixels
  anchor: WidgetAnchor;
  fs: number;
  alpha: number;
}

function SpeedWidget({ px, py, anchor, fs, alpha, color, value }:
                      WidgetRenderProps & { color?: string; value: number }) {
  const w = WIDGET_SIZES.speed.w * fs;
  const h = WIDGET_SIZES.speed.h * fs;
  const left = leftOf(px, w, anchor);
  return (
    <>
      <Rect x={left} y={py} width={w} height={h} rx={8}
            fill="#000" fillOpacity={alpha} />
      <SvgText x={px} y={py + 42 * fs} fill={color ?? '#fff'}
               fontSize={40 * fs} fontWeight="900"
               textAnchor={svgTextAnchor(anchor)}>
        {value.toFixed(0)}
      </SvgText>
      <SvgText x={px} y={py + 60 * fs} fill="#aaa"
               fontSize={12 * fs} fontWeight="600"
               textAnchor={svgTextAnchor(anchor)}>
        km/h
      </SvgText>
    </>
  );
}

function LapTimeWidget({ px, py, anchor, fs, alpha, color, timeMs, lapNumber }:
                        WidgetRenderProps & { color?: string; timeMs: number; lapNumber?: number }) {
  const label = `${lapNumber ? `R${lapNumber}  ` : ''}${fmtLapTime(timeMs)}`;
  const w = WIDGET_SIZES.lapTime.w * fs;
  const h = WIDGET_SIZES.lapTime.h * fs;
  const left = leftOf(px, w, anchor);
  return (
    <>
      <Rect x={left} y={py} width={w} height={h} rx={6}
            fill="#000" fillOpacity={alpha} />
      <SvgText x={px} y={py + 20 * fs} fill={color ?? '#fff'}
               fontSize={14 * fs} fontWeight="700"
               textAnchor={svgTextAnchor(anchor)}>
        {label}
      </SvgText>
    </>
  );
}

function DeltaWidget({ px, py, anchor, fs, alpha, value, color }:
                      WidgetRenderProps & { value: number; color: string }) {
  const label = fmtDelta(value);
  const w = WIDGET_SIZES.delta.w * fs;
  const h = WIDGET_SIZES.delta.h * fs;
  const left = leftOf(px, w, anchor);
  return (
    <>
      <Rect x={left} y={py} width={w} height={h} rx={6}
            fill="#000" fillOpacity={alpha} />
      <SvgText x={px} y={py + 24 * fs} fill={color}
               fontSize={18 * fs} fontWeight="800"
               textAnchor={svgTextAnchor(anchor)}>
        {label}
      </SvgText>
    </>
  );
}

function TrackNameWidget({ px, py, anchor, fs, alpha, accent, name }:
                          WidgetRenderProps & { accent: string; name: string }) {
  const w = WIDGET_SIZES.trackName.w * fs;
  const h = WIDGET_SIZES.trackName.h * fs;
  const left = leftOf(px, w, anchor);
  return (
    <>
      <Rect x={left} y={py} width={w} height={h} rx={6}
            fill="#000" fillOpacity={alpha} />
      <SvgText x={px} y={py + 16 * fs} fill={accent}
               fontSize={12 * fs} fontWeight="700"
               textAnchor={svgTextAnchor(anchor)}>
        {name}
      </SvgText>
    </>
  );
}

function GMeterWidget({ px, py, anchor, fs, alpha, accent, gLat, gLong }:
                       WidgetRenderProps & { accent: string; gLat: number; gLong: number }) {
  const boxW = WIDGET_SIZES.gMeter.w * fs;
  const boxH = WIDGET_SIZES.gMeter.h * fs;
  const left = leftOf(px, boxW, anchor);
  const r   = Math.min(boxW, boxH - 28 * fs) * 0.45;
  const cx  = left + boxW / 2;
  const cy  = py + r + 8 * fs;
  const vx  =  Math.max(-2, Math.min(2, gLat))  / 2 * r;
  const vy  = -Math.max(-2, Math.min(2, gLong)) / 2 * r;
  return (
    <>
      <Rect x={left} y={py} width={boxW} height={boxH} rx={8}
            fill="#000" fillOpacity={alpha} />
      <Circle cx={cx} cy={cy} r={r * 0.5} fill="none"
              stroke="#555" strokeWidth={1} strokeDasharray="2 2" />
      <Circle cx={cx} cy={cy} r={r} fill="none" stroke="#888" strokeWidth={1} />
      <Line x1={cx - r} y1={cy} x2={cx + r} y2={cy} stroke="#555" strokeWidth={0.7} />
      <Line x1={cx} y1={cy - r} x2={cx} y2={cy + r} stroke="#555" strokeWidth={0.7} />
      <Line x1={cx} y1={cy} x2={cx + vx} y2={cy + vy}
            stroke={accent} strokeWidth={3} strokeLinecap="round" />
      <Circle cx={cx + vx} cy={cy + vy} r={5 * fs}
              fill={accent} stroke="#fff" strokeWidth={1.5} />
      <SvgText x={cx} y={cy + r + 16 * fs} fill="#fff"
               fontSize={10 * fs} fontWeight="700" textAnchor="middle">
        {Math.sqrt(gLat * gLat + gLong * gLong).toFixed(1)} g
      </SvgText>
    </>
  );
}

// ── Phase 3 widgets ────────────────────────────────────────────────

function SpeedBarWidget({ px, py, anchor, fs, alpha, accent, value }:
                         WidgetRenderProps & { accent: string; value: number }) {
  const w = WIDGET_SIZES.speedBar.w * fs;
  const h = WIDGET_SIZES.speedBar.h * fs;
  const left = leftOf(px, w, anchor);
  const MAX = 250;   // km/h full-scale; picked wide enough for motorsport
  const pct = Math.max(0, Math.min(1, value / MAX));
  return (
    <>
      <Rect x={left} y={py} width={w} height={h} rx={h / 2}
            fill="#000" fillOpacity={alpha} />
      <Rect x={left + 2} y={py + 2} width={Math.max(0, pct * w - 4)}
            height={h - 4} rx={(h - 4) / 2} fill={accent} />
      <SvgText x={left + w / 2} y={py + h / 2 + 4 * fs} fill="#fff"
               fontSize={11 * fs} fontWeight="700" textAnchor="middle">
        {value.toFixed(0)} km/h
      </SvgText>
    </>
  );
}

function GForceWidget({ px, py, anchor, fs, alpha, color, gLat, gLong }:
                       WidgetRenderProps & { color?: string; gLat: number; gLong: number }) {
  const w = WIDGET_SIZES.gForce.w * fs;
  const h = WIDGET_SIZES.gForce.h * fs;
  const left = leftOf(px, w, anchor);
  const total = Math.sqrt(gLat * gLat + gLong * gLong);
  return (
    <>
      <Rect x={left} y={py} width={w} height={h} rx={8}
            fill="#000" fillOpacity={alpha} />
      <SvgText x={px} y={py + 40 * fs} fill={color ?? '#fff'}
               fontSize={34 * fs} fontWeight="900"
               textAnchor={svgTextAnchor(anchor)}>
        {total.toFixed(1)}
      </SvgText>
      <SvgText x={px} y={py + 56 * fs} fill="#aaa"
               fontSize={11 * fs} fontWeight="600"
               textAnchor={svgTextAnchor(anchor)}>
        g gesamt
      </SvgText>
    </>
  );
}

function LapNumberWidget({ px, py, anchor, fs, alpha, color, lapNumber }:
                          WidgetRenderProps & { color: string; lapNumber?: number }) {
  const w = WIDGET_SIZES.lapNumber.w * fs;
  const h = WIDGET_SIZES.lapNumber.h * fs;
  const left = leftOf(px, w, anchor);
  const lap = lapNumber ?? 0;
  return (
    <>
      <Rect x={left} y={py} width={w} height={h} rx={8}
            fill="#000" fillOpacity={alpha} />
      <SvgText x={px} y={py + 18 * fs} fill="#aaa"
               fontSize={10 * fs} fontWeight="700"
               textAnchor={svgTextAnchor(anchor)}>
        RUNDE
      </SvgText>
      <SvgText x={px} y={py + 42 * fs} fill={color}
               fontSize={28 * fs} fontWeight="900"
               textAnchor={svgTextAnchor(anchor)}>
        {lap}
      </SvgText>
    </>
  );
}

function MiniMapWidget({ px, py, anchor, fs, alpha, accent, path, timeMs }:
                        WidgetRenderProps & {
                          accent: string;
                          path?: Array<[number, number, number]>;
                          timeMs: number;
                        }) {
  const boxW = WIDGET_SIZES.miniMap.w * fs;
  const boxH = WIDGET_SIZES.miniMap.h * fs;
  const left = leftOf(px, boxW, anchor);
  const pad = 8 * fs;
  const innerW = boxW - 2 * pad;
  const innerH = boxH - 2 * pad;

  // Fallback: no track data → draw a hollow rounded box with a hint.
  if (!path || path.length < 3) {
    return (
      <>
        <Rect x={left} y={py} width={boxW} height={boxH} rx={8}
              fill="#000" fillOpacity={alpha} />
        <SvgText x={left + boxW / 2} y={py + boxH / 2 + 4 * fs} fill="#666"
                 fontSize={10 * fs} textAnchor="middle" fontStyle="italic">
          Keine Streckendaten
        </SvgText>
      </>
    );
  }

  // Compute lat/lon bounding box, then scale uniformly so the circuit
  // fills the widget while preserving aspect (no stretch).
  let minLat = path[0][0], maxLat = path[0][0];
  let minLon = path[0][1], maxLon = path[0][1];
  for (const p of path) {
    if (p[0] < minLat) minLat = p[0]; if (p[0] > maxLat) maxLat = p[0];
    if (p[1] < minLon) minLon = p[1]; if (p[1] > maxLon) maxLon = p[1];
  }
  const dLat = Math.max(1e-7, maxLat - minLat);
  // 1° of longitude shrinks with latitude — correct for aspect so north
  // doesn't look squashed.
  const latMid = (minLat + maxLat) / 2;
  const dLonM = (maxLon - minLon) * Math.cos(latMid * Math.PI / 180);
  const dMax = Math.max(dLat, Math.abs(dLonM));
  const scaleY = innerH / dMax;
  const scaleX = innerW / dMax;
  const scale = Math.min(scaleX, scaleY);
  const cx0 = left + pad + innerW / 2;
  const cy0 = py   + pad + innerH / 2;

  const project = (lat: number, lon: number): [number, number] => {
    // Flat-earth around latMid; flip Y because SVG y grows downward while
    // latitude grows northward.
    const dx = (lon - (minLon + maxLon) / 2) * Math.cos(latMid * Math.PI / 180);
    const dy = (lat - (minLat + maxLat) / 2);
    return [cx0 + dx * scale, cy0 - dy * scale];
  };

  const points = path.map(p => {
    const [x, y] = project(p[0], p[1]);
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  }).join(' ');

  // Car position: interpolate between track_points bracketing timeMs.
  let carX = cx0, carY = cy0;
  {
    const N = path.length;
    if (timeMs <= path[0][2]) {
      [carX, carY] = project(path[0][0], path[0][1]);
    } else if (timeMs >= path[N - 1][2]) {
      [carX, carY] = project(path[N - 1][0], path[N - 1][1]);
    } else {
      let j = 0;
      while (j < N - 1 && path[j + 1][2] < timeMs) j++;
      const a = path[j], b = path[Math.min(j + 1, N - 1)];
      const span = Math.max(1, b[2] - a[2]);
      const f = (timeMs - a[2]) / span;
      const lat = a[0] + f * (b[0] - a[0]);
      const lon = a[1] + f * (b[1] - a[1]);
      [carX, carY] = project(lat, lon);
    }
  }

  return (
    <>
      <Rect x={left} y={py} width={boxW} height={boxH} rx={8}
            fill="#000" fillOpacity={alpha} />
      <Polyline points={points} stroke="#888" strokeWidth={1.5}
                fill="none" strokeLinejoin="round" strokeLinecap="round" />
      <Circle cx={carX} cy={carY} r={4 * fs} fill={accent}
              stroke="#fff" strokeWidth={1.5} />
    </>
  );
}

const styles = StyleSheet.create({
  root: { position: 'absolute', top: 0, left: 0 },
});
