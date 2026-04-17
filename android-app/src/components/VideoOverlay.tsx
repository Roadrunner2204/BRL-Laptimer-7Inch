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
import Svg, { Rect, Circle, Line, Text as SvgText } from 'react-native-svg';
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
};

// Left edge of a widget's bounding box given its anchor point + width.
function leftOf(anchorPx: number, w: number, a: WidgetAnchor): number {
  return a === 'start' ? anchorPx
       : a === 'end'   ? anchorPx - w
       :                 anchorPx - w / 2;
}

export default function VideoOverlay({
  width, height, timeMs, channels, delta, lapNumber, trackName, config,
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

const styles = StyleSheet.create({
  root: { position: 'absolute', top: 0, left: 0 },
});
